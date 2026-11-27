// Local HTTP server the @TakaroIntegration DayZ mod talks to.
//
// The mod was built against a polling REST shape (POST /events, GET /poll,
// POST /operation/<id>/result, POST /register). This server speaks that shape
// and translates each call into the equivalent Takaro WS message.
//
// Mod                                  Bridge                          Takaro
// ───────────────────────────────────  ──────────────────────────────  ─────────────────
// POST /gameserver/register         →  identify (over WS, on boot)
//                                                       <───  identifyResponse(gameServerId)
//                                   ←  {identityToken, gameServerId}
//
// POST /gameserver/<id>/events      →  for each event: gameEvent(type, data)
//                                   ←  {ok:true}, or 503 while the WS is not identified (mod requeues)
//
// GET  /gameserver/<id>/poll        →  drain queue of pending Takaro requests
//                                   ←  {operations:[{operationId, action, argsJson}]}
//
// POST /gameserver/<id>/operation/<opId>/result
//                                   →  response(requestId=opId, payload)
//                                   ←  {ok:true}

import http from 'node:http';
import { DEBUG, logger, truncate } from '../logger.js';
import type { TakaroWsClient } from '../ws/client.js';
import type { GameEventType, WsMessage } from '../ws/protocol.js';
import { DISABLED_MAP_INFO, MapTileError, parseFrameTileArgs, parseTileArgs, type MapService } from '../map/service.js';
import { eventWithDimension, withCharacter, withDimension } from '../ws/capabilities.js';
import { Catalogue, humanizeCatalogue, type CatalogueEntry } from '../catalogue.js';
import { normalizeGameVersion, type GameIcon } from '../gameProfile.js';

/** Actions the mod answers (TakaroCommandDispatcher.Dispatch). Anything else is not queued. */
export const MOD_ACTIONS = new Set([
  'testReachability', 'getPlayer', 'getPlayers', 'listPlayers', 'getPlayerLocation', 'getPlayerInventory',
  'listItems', 'listEntities', 'listLocations', 'listBans', 'giveItem', 'sendMessage', 'teleportPlayer',
  'kickPlayer', 'banPlayer', 'unbanPlayer', 'executeConsoleCommand', 'executeCommand', 'shutdown',
]);

/** Operations the sidecar sends the mod for itself; their answers never reach Takaro. */
const INTERNAL_PREFIX = 'sidecar-catalogue-';

interface PendingOperation {
  operationId: string; // = WS requestId
  action: string;
  argsJson: string;
}

interface TakaroEvent {
  type: string;
  timestamp?: string;
  msg?: string;
  channel?: string;
  player?: unknown;
  recipient?: unknown;
  attacker?: unknown;
  position?: unknown;
  weapon?: string;
  entity?: string;
  raw?: string;
}

// The DayZ mod uses Enforce Script's JsonSerializer, which has two quirks
// Takaro's DTOs reject:
//   1. booleans serialize as integers (0/1)
//   2. wrapping arrays inside objects (e.g. {players:[...]}) is natural in
//      Enforce Script but Takaro expects bare arrays for list-type results.
// We normalise both per-action here so the mod can stay simple.

const BOOL_FIELDS_BY_ACTION: Record<string, string[]> = {
  testReachability: ['connectable'],
  getPlayers: ['online'],
  listPlayers: ['online'],
  getPlayer: ['online'],
};

function coerceBools(obj: unknown, fields: string[]): unknown {
  if (obj == null) return obj;
  if (Array.isArray(obj)) return obj.map((v) => coerceBools(v, fields));
  if (typeof obj === 'object') {
    const out: Record<string, unknown> = {};
    for (const [k, v] of Object.entries(obj as Record<string, unknown>)) {
      if (fields.includes(k) && (v === 0 || v === 1)) {
        out[k] = v === 1;
      } else if (typeof v === 'object') {
        out[k] = coerceBools(v, fields);
      } else {
        out[k] = v;
      }
    }
    return out;
  }
  return obj;
}

// Per-action shape adapters to match Takaro DTOs.
function shapeForAction(action: string | undefined, raw: unknown): unknown {
  if (!action) return raw;

  // Apply bool coercion first
  const boolFields = BOOL_FIELDS_BY_ACTION[action];
  let v = boolFields ? coerceBools(raw, boolFields) : raw;

  // Per-action structural adjustments
  if (action === 'getPlayers' || action === 'listPlayers') {
    // Expected: IGamePlayer[]. Mod returns {players: [...]} — unwrap.
    if (v && typeof v === 'object' && Array.isArray((v as { players?: unknown[] }).players)) {
      v = (v as { players: unknown[] }).players;
    }
    if (Array.isArray(v)) v = v.map(withCharacter);
  } else if (action === 'getPlayer') {
    v = withCharacter(v);
  } else if (action === 'listBans') {
    if (v && typeof v === 'object' && Array.isArray((v as { bans?: unknown[] }).bans)) {
      v = (v as { bans: unknown[] }).bans;
    }
    // Takaro needs a name on every banned player and writes it onto the player record, so a
    // placeholder would rename the player. The mod sends the last real name it saw; a ban
    // without one (a player never seen on this server) is left out.
    if (Array.isArray(v)) {
      v = v.flatMap((b) => {
        const ban = { ...(b as Record<string, unknown>) };
        const p = { ...((ban.player as Record<string, unknown>) ?? {}) };
        const gid = String(p.gameId ?? '');
        if (!gid || typeof p.name !== 'string' || p.name === '') return [];
        if (!p.steamId && /^7656\d{13}$/.test(gid)) p.steamId = gid;
        ban.player = p;
        return [ban];
      });
    }
  } else if (action === 'listItems' || action === 'listEntities') {
    if (v && typeof v === 'object' && Array.isArray((v as { items?: unknown[] }).items)) {
      v = (v as { items: unknown[] }).items;
    }
    if (Array.isArray(v)) v = humanizeCatalogue(v as CatalogueEntry[]);
  }

  return v;
}

export class LocalHttpServer {
  private server: http.Server | null = null;
  private operationQueue: PendingOperation[] = [];
  // Track action per outstanding operation so we can apply per-action coercion
  // when the mod returns a result.
  private operationActions = new Map<string, string>();

  // Cached identity for the mod's first-run "register" round-trip.
  private cachedIdentity = { identityToken: '', gameServerId: '' };

  // Liveness counters surfaced on /health, so "is the mod actually talking to us?"
  // is answerable without reading logs.
  private lastModContactAt: string | null = null;
  private lastPollAt: string | null = null;

  /** Epoch ms of the mod's last call of any kind, or null before the first one. */
  lastModContactMs(): number | null {
    return this.lastModContactAt ? Date.parse(this.lastModContactAt) : null;
  }

  /** Epoch ms of the mod's last /poll, or null before the first one. */
  lastPollAtMs(): number | null {
    return this.lastPollAt ? Date.parse(this.lastPollAt) : null;
  }
  private lastEventAt: string | null = null;
  private eventsForwarded = 0;
  // M4: events answered with 503 (WS down) so the mod requeues them; counted per attempt.
  private eventsDeferred = 0;
  // F9: RPT-tailed `log` events are emitted straight to the WS (they never pass
  // through this HTTP server), so the tail reports its count in here.
  private logsForwarded: () => number = () => 0;
  /** Class name to human name, for kill and death events. */
  readonly catalogue = new Catalogue();
  private gameVersion: string | null = null;
  private gameVersionListener: (v: string) => void = () => {};
  private catalogueRequested = false;
  private modContactListener: (() => void) | null = null;
  private modContacted = false;

  constructor(
    private port: number,
    private ws: TakaroWsClient,
    private serverName: string,
    private identityToken: string,
    private bindAddress: string = '127.0.0.1',
    private map: MapService | null = null,
    private gameIcon: GameIcon | null = null,
  ) {
    this.cachedIdentity.identityToken = identityToken;

    // Whenever the WS gets identified or re-identified, refresh the cached gameServerId
    // so subsequent register calls from the mod return the latest.
    this.ws.on('identified', (gameServerId: string) => {
      this.cachedIdentity.gameServerId = gameServerId;
    });

    // Takaro requests come in as WS messages; queue them so the next /poll drains.
    this.ws.on('request', (msg: WsMessage) => {
      const requestId = msg.requestId;
      if (!requestId) {
        logger.warn('Got Takaro request with no requestId');
        return;
      }
      const payload = msg.payload as { action?: string; args?: unknown } | undefined;
      if (!payload?.action) {
        logger.warn(`Got Takaro request with no action: ${JSON.stringify(msg)}`);
        this.ws.sendError(requestId, 'No action');
        return;
      }
      // Reachability is answered here from the mod's last call (it polls every second, and asks
      // for its identity every few seconds before that), so Takaro's jobs that test reachability
      // right after identify (catalogue, icon) are not refused while the mod is still registering.
      if (payload.action === 'testReachability') {
        this.ws.sendResponse(requestId, reachability(this.lastModContactMs(), Date.now()));
        return;
      }
      if (payload.action === 'getGameIcon') {
        if (this.gameIcon) this.ws.sendResponse(requestId, { revision: this.gameIcon.revision, data: this.gameIcon.data });
        else this.ws.sendError(requestId, 'This connector ships no game icon', 'not_found');
        return;
      }
      // An action DayZ does not have (getPlayerProgression: no levels) or does not know is
      // answered here with the protocol's `unsupported`, not handed to the mod as a game error.
      if (!MOD_ACTIONS.has(payload.action) && payload.action !== 'getMapInfo' && payload.action !== 'getMapTile') {
        this.ws.sendError(requestId, `DayZ connector does not support ${payload.action}`, 'unsupported');
        return;
      }
      // Map requests are answered here from the world survey; the mod never sees them.
      if (payload.action === 'getMapInfo' || payload.action === 'getMapTile') {
        this.answerMap(requestId, payload.action, payload.args);
        return;
      }
      this.operationQueue.push({
        operationId: requestId,
        action: payload.action,
        argsJson: slimArgs(typeof payload.args === 'string' ? payload.args : JSON.stringify(payload.args ?? {})),
      });
      this.operationActions.set(requestId, payload.action);
      logger.debug(`Queued ${payload.action} (op=${requestId})`);
    });
  }

  /** The Dimension positions carry on a protocol v1 connection, or null on v0 / without a map. */
  private positionDimension(): string | null {
    if (this.ws.getProtocolVersion() !== 1 || !this.map) return null;
    return this.map.dimension()?.id ?? null;
  }

  private answerMap(requestId: string, action: string, args: unknown): void {
    try {
      if (action === 'getMapTile' && this.ws.getProtocolVersion() === 1) {
        if (!this.map) throw new MapTileError('not_found', 'Map tiles are not configured (set DAYZ_LOG_DIR or DAYZ_MAP_DIR)');
        const { dimension, zoom, x, y } = parseFrameTileArgs(args);
        this.ws.sendResponse(requestId, this.map.frameTile(dimension, zoom, x, y));
        return;
      }
      if (action === 'getMapInfo') {
        this.ws.sendResponse(requestId, this.map ? this.map.mapInfo() : DISABLED_MAP_INFO);
        return;
      }
      if (!this.map) throw new Error('Map tiles are not configured (set DAYZ_LOG_DIR or DAYZ_MAP_DIR)');
      const { x, y, z } = parseTileArgs(args);
      this.ws.sendResponse(requestId, this.map.tile(x, y, z));
    } catch (err) {
      logger.debug(`${action} failed: ${(err as Error).message}`);
      this.ws.sendError(requestId, (err as Error).message, err instanceof MapTileError ? err.code : 'game_error');
    }
  }

  /** The server build the mod last reported, or null before its first poll. */
  getGameVersion(): string | null {
    return this.gameVersion;
  }

  onGameVersion(fn: (v: string) => void): void {
    this.gameVersionListener = fn;
  }

  /** Called once, on the mod's first request (register, events or poll). */
  onModContact(fn: () => void): void {
    this.modContactListener = fn;
  }

  /** True once the mod has called in since this sidecar started. */
  modHasContacted(): boolean {
    return this.modContacted;
  }

  /**
   * Asks the mod for both catalogues once, so kill and death events carry human names even
   * before Takaro's own catalogue sync. The mod caches them, so Takaro's later requests are free.
   */
  private requestCatalogue(): void {
    if (this.catalogueRequested) return;
    this.catalogueRequested = true;
    for (const action of ['listEntities', 'listItems']) {
      const id = INTERNAL_PREFIX + action;
      this.operationQueue.push({ operationId: id, action, argsJson: '{}' });
      this.operationActions.set(id, action);
    }
  }

  /** F9: lets index.ts surface the RPT tail's forwarded-line count on /health. */
  setLogCounter(fn: () => number): void {
    this.logsForwarded = fn;
  }

  start(): void {
    this.server = http.createServer((req, res) => this.handle(req, res));
    this.server.listen(this.port, this.bindAddress, () => {
      logger.info(`HTTP listening on ${this.bindAddress}:${this.port} (mod-facing)`);
    });
  }

  stop(): void {
    this.server?.close();
  }

  private send(res: http.ServerResponse, code: number, body: object | string): void {
    const payload = typeof body === 'string' ? body : JSON.stringify(body);
    res.statusCode = code;
    res.setHeader('Content-Type', 'application/json');
    res.setHeader('Content-Length', Buffer.byteLength(payload).toString());
    res.end(payload);
  }

  private async readBody(req: http.IncomingMessage): Promise<string> {
    return new Promise((resolve, reject) => {
      const chunks: Buffer[] = [];
      req.on('data', (c) => chunks.push(c as Buffer));
      req.on('end', () => resolve(Buffer.concat(chunks).toString('utf8')));
      req.on('error', reject);
    });
  }

  private parseGameServerPath(path: string): { gameServerId: string; rest: string } | null {
    // /gameserver/<id>/<rest>
    const m = path.match(/^\/gameserver\/([^/]+)\/(.+)$/);
    if (!m) return null;
    return { gameServerId: m[1]!, rest: m[2]! };
  }

  private async handle(req: http.IncomingMessage, res: http.ServerResponse): Promise<void> {
    const url = req.url || '/';
    const method = req.method || 'GET';
    const path = url.split('?')[0]!.replace(/\/+$/, '') || '/';

    if (DEBUG) logger.debug(`HTTP ${method} ${url}`);
    if (path !== '/health') {
      this.lastModContactAt = new Date().toISOString();
      if (!this.modContacted) {
        this.modContacted = true;
        this.modContactListener?.();
      }
    }

    try {
      // Health
      if (method === 'GET' && path === '/health') {
        return this.send(res, 200, {
          ok: bridgeHealthy(this.ws.identified(), this.ws.connected(), this.lastPollAtMs(), Date.now()),
          identified: this.ws.identified(),
          wsConnected: this.ws.connected(),
          gameServerId: this.ws.getGameServerId(),
          protocolVersion: this.ws.getProtocolVersion(),
          unacknowledgedEvents: this.ws.unacknowledged(),
          pendingOperations: this.operationQueue.length,
          lastModContactAt: this.lastModContactAt,
          lastPollAt: this.lastPollAt,
          lastEventAt: this.lastEventAt,
          eventsForwarded: this.eventsForwarded,
          eventsDeferred: this.eventsDeferred,
          logsForwarded: this.logsForwarded(),
        });
      }

      // Mod-facing register: returns the Takaro identity we already established
      if (method === 'POST' && path === '/gameserver/register') {
        if (!this.cachedIdentity.gameServerId) {
          // WS hasn't identified yet; tell mod to retry shortly.
          return this.send(res, 503, { error: 'Bridge still identifying with Takaro' });
        }
        return this.send(res, 200, {
          identityToken: this.cachedIdentity.identityToken,
          gameServerId: this.cachedIdentity.gameServerId,
        });
      }

      // /gameserver/<id>/events  (POST)  — forward each event to Takaro
      // /gameserver/<id>/poll    (GET)   — drain queued operations
      // /gameserver/<id>/operation/<opId>/result  (POST) — relay to Takaro as response
      const parsed = this.parseGameServerPath(path);
      if (parsed) {
        if (method === 'POST' && parsed.rest === 'events') {
          const body = await this.readBody(req);
          if (DEBUG) logger.debug(`HTTP BODY events ${truncate(body)}`);
          const batch = body ? parseModJson(body) : { events: [] };
          const events = (batch.events || []) as TakaroEvent[];
          // M4 fix: while the Takaro WS is closed (or open but not identified yet),
          // ws.send() only logs "Cannot send gameEvent: WS not open" and drops the
          // frame, yet the mod used to get 200 and forgot the batch. Answer 503 so
          // the mod requeues it (TakaroBridge.OnFlushComplete) and retries on its
          // next flush. The check runs right before the synchronous send loop:
          // Node dispatches the socket's close event on a later tick, so the state
          // cannot change mid-batch and a batch is either sent whole or not at all
          // (no partial batch, no duplicates on retry).
          if (events.length > 0 && !(this.ws.connected() && this.ws.identified())) {
            this.eventsDeferred += events.length;
            logger.warn(
              `Takaro WS not ready (connected=${this.ws.connected()} identified=${this.ws.identified()}): ` +
                `503 for ${events.length} event(s), mod will requeue`,
            );
            return this.send(res, 503, { error: 'Takaro WebSocket not connected; retry', deferred: events.length });
          }
          const dimension = this.positionDimension();
          for (const ev of events) {
            // strip the type field from the data payload — Takaro takes type separately
            const { type, ...fields } = ev;
            const named = this.catalogue.translateEvent(type, fields);
            const rest = dimension ? eventWithDimension(named, dimension) : named;
            // The mod stamps the real UTC time of the event (to the second). Keep it, so an
            // event the mod held through a Takaro outage keeps its own time; fall back to the
            // receipt time when the stamp is missing or implausible.
            const modTimestamp = rest.timestamp;
            rest.timestamp = eventTimestamp(modTimestamp, Date.now());
            if (DEBUG && modTimestamp !== rest.timestamp) {
              logger.debug(`Event ${type}: mod timestamp '${modTimestamp}' replaced with ${rest.timestamp}`);
            }
            this.ws.sendGameEvent(type as GameEventType, rest);
          }
          if (events.length > 0) {
            this.eventsForwarded += events.length;
            this.lastEventAt = new Date().toISOString();
          }
          logger.debug(`Forwarded ${events.length} events`);
          return this.send(res, 200, { ok: true, forwarded: events.length });
        }

        if (method === 'GET' && parsed.rest === 'poll') {
          this.lastPollAt = new Date().toISOString();
          const reported = normalizeGameVersion(new URLSearchParams(url.split('?')[1] ?? '').get('gameVersion') ?? undefined);
          if (reported && reported !== this.gameVersion) {
            this.gameVersion = reported;
            logger.info(`DayZ server build ${reported}`);
            this.gameVersionListener(reported);
          }
          this.requestCatalogue();
          const ops = this.operationQueue.splice(0, this.operationQueue.length);
          return this.send(res, 200, { operations: ops });
        }

        const opMatch = parsed.rest.match(/^operation\/([^/]+)\/result$/);
        if (method === 'POST' && opMatch) {
          const opId = opMatch[1]!;
          const body = await this.readBody(req);
          if (DEBUG) logger.debug(`HTTP BODY result op=${opId} ${truncate(body)}`);
          const obj = body ? parseModJson(body) : {};
          const action = this.operationActions.get(opId);
          this.operationActions.delete(opId);
          if (opId.startsWith(INTERNAL_PREFIX)) {
            const shaped = obj.ok ? shapeForAction(action, obj.result ?? {}) : null;
            if (Array.isArray(shaped)) {
              if (action === 'listItems') this.catalogue.setItems(shaped as CatalogueEntry[]);
              else this.catalogue.setEntities(shaped as CatalogueEntry[]);
              logger.info(`Catalogue: ${shaped.length} ${action === 'listItems' ? 'items' : 'entities'} for event names`);
            } else {
              logger.warn(`Catalogue ${action} failed: ${obj.error ?? 'no list'}`);
            }
            return this.send(res, 200, { ok: true });
          }
          if (obj.ok) {
            const shaped = shapeForAction(action, obj.result ?? {});
            if (Array.isArray(shaped) && action === 'listItems') this.catalogue.setItems(shaped as CatalogueEntry[]);
            if (Array.isArray(shaped) && action === 'listEntities') this.catalogue.setEntities(shaped as CatalogueEntry[]);
            const dimension = this.positionDimension();
            this.ws.sendResponse(opId, dimension ? withDimension(action, shaped, dimension) : shaped);
          } else {
            this.ws.sendError(opId, obj.error || 'Unknown error');
          }
          logger.debug(`Relayed result for op ${opId} action=${action ?? '?'} (ok=${obj.ok})`);
          return this.send(res, 200, { ok: true });
        }
      }

      this.send(res, 404, { error: 'Not found', path });
    } catch (err) {
      logger.error(`HTTP handler error: ${(err as Error).stack || (err as Error).message}`);
      this.send(res, 500, { error: 'Internal' });
    }
  }
}

/**
 * M3 fix: the mod builds JSON by string concatenation. An unescaped control
 * character (e.g. the newline inside Enforce's "JSON ERROR:\n..." text) made
 * JSON.parse throw, the result was never relayed, and Takaro timed out after
 * 10 s instead of seeing the error. Retry with raw control chars escaped.
 */
export function parseModJson(body: string): any {
  try {
    return JSON.parse(body);
  } catch (e) {
    const cleaned = body.replace(/[\u0000-\u001f]/g, (c) =>
      c === '\n' ? '\\n' : c === '\r' ? '\\r' : c === '\t' ? '\\t' : '\\u' + c.charCodeAt(0).toString(16).padStart(4, '0'),
    );
    return JSON.parse(cleaned);
  }
}

/** Events older than this, or stamped in the future, get the receipt time instead. */
const MAX_EVENT_AGE_MS = 24 * 3600_000;
const MAX_CLOCK_SKEW_MS = 60_000;

/** The mod's UTC stamp as ISO 8601 when it is plausible, else the receipt time. */
export function eventTimestamp(modTimestamp: unknown, nowMs: number): string {
  if (typeof modTimestamp === 'string' && /^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(\.\d+)?Z$/.test(modTimestamp)) {
    const t = Date.parse(modTimestamp);
    if (Number.isFinite(t) && t <= nowMs + MAX_CLOCK_SKEW_MS && t >= nowMs - MAX_EVENT_AGE_MS) return new Date(t).toISOString();
  }
  return new Date(nowMs).toISOString();
}

/** testReachability: the game is reachable while the mod keeps calling in. */
export function reachability(lastModContactMs: number | null, nowMs: number): { connectable: boolean; reason: string | null } {
  if (lastModContactMs !== null && nowMs - lastModContactMs < MOD_POLL_STALE_MS) return { connectable: true, reason: null };
  return { connectable: false, reason: 'The DayZ server is not running, or the @TakaroIntegration mod is not loaded or cannot reach the sidecar' };
}

/** The mod polls about once a second, so this much silence means the game is gone. */
const MOD_POLL_STALE_MS = 15_000;

/** True only while events can flow end to end: Takaro identified us and the mod is polling. */
export function bridgeHealthy(identified: boolean, wsConnected: boolean, lastPollMs: number | null, nowMs: number): boolean {
  return identified && wsConnected && lastPollMs !== null && nowMs - lastPollMs < MOD_POLL_STALE_MS;
}

/**
 * M3 fix: for teleportPlayer/kickPlayer/banPlayer/... Takaro sends the whole
 * PlayerOnGameserver row as `player` (roles, permissions, inventory, steam
 * profile: several KB). The Enforce side truncates that string (observed: the
 * argsJson ended mid-`platformId`, so x/y/z and reason were lost and teleport
 * failed with "missing x/y/z"). The mod only ever needs player.gameId, so the
 * nested object is reduced to {gameId} before queueing.
 */
export function slimArgs(argsJson: string): string {
  try {
    const obj = JSON.parse(argsJson);
    if (obj && typeof obj === 'object' && obj.player && typeof obj.player === 'object') {
      obj.player = { gameId: obj.player.gameId };
      // Some mod handlers read a flat gameId only (getPlayerLocation); give them both shapes.
      if (obj.gameId === undefined && typeof obj.player.gameId === 'string') obj.gameId = obj.player.gameId;
      return JSON.stringify(obj);
    }
    // unbanPlayer: Takaro sends the PlayerOnGameserver row flat (no `player` key).
    if (obj && typeof obj === 'object' && typeof obj.gameId === 'string' && obj.playerId && obj.gameServerId) {
      return JSON.stringify({ gameId: obj.gameId });
    }
  } catch {
    /* not JSON: pass through */
  }
  return argsJson;
}
