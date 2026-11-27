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
  } else if (action === 'listBans') {
    if (v && typeof v === 'object' && Array.isArray((v as { bans?: unknown[] }).bans)) {
      v = (v as { bans: unknown[] }).bans;
    }
    // M3 fix: Takaro rejected `{player:{gameId}}` ("gameserver responded with
    // bad data"); its IGamePlayer needs name too. The mod only stores the Steam64,
    // so fill steamId/name from it.
    if (Array.isArray(v)) {
      v = v.map((b) => {
        const ban = { ...(b as Record<string, unknown>) };
        const p = { ...((ban.player as Record<string, unknown>) ?? {}) };
        const gid = String(p.gameId ?? '');
        if (gid) {
          if (!p.steamId && /^7656\d{13}$/.test(gid)) p.steamId = gid;
          if (!p.name) p.name = gid;
        }
        ban.player = p;
        return ban;
      });
    }
  } else if (action === 'listItems') {
    if (v && typeof v === 'object' && Array.isArray((v as { items?: unknown[] }).items)) {
      v = (v as { items: unknown[] }).items;
    }
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

  constructor(
    private port: number,
    private ws: TakaroWsClient,
    private serverName: string,
    private identityToken: string,
    private bindAddress: string = '127.0.0.1',
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
      this.operationQueue.push({
        operationId: requestId,
        action: payload.action,
        argsJson: slimArgs(typeof payload.args === 'string' ? payload.args : JSON.stringify(payload.args ?? {})),
      });
      this.operationActions.set(requestId, payload.action);
      logger.debug(`Queued ${payload.action} (op=${requestId})`);
    });
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
    if (path !== '/health') this.lastModContactAt = new Date().toISOString();

    try {
      // Health
      if (method === 'GET' && path === '/health') {
        return this.send(res, 200, {
          ok: true,
          identified: this.ws.identified(),
          wsConnected: this.ws.connected(),
          gameServerId: this.ws.getGameServerId(),
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
          for (const ev of events) {
            // strip the type field from the data payload — Takaro takes type separately
            const { type, ...rest } = ev;
            // F2: the mod stamps in-game world time at minute resolution, which
            // Takaro shows as wildly wrong event times. Ingest time in the bridge
            // is within milliseconds of the real event, so it wins.
            const modTimestamp = rest.timestamp;
            rest.timestamp = new Date().toISOString();
            if (DEBUG && modTimestamp) {
              logger.debug(`Event ${type}: replaced mod timestamp '${modTimestamp}' with ${rest.timestamp}`);
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
          if (obj.ok) {
            const result = shapeForAction(action, obj.result ?? {});
            this.ws.sendResponse(opId, result);
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
