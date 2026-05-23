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
//                                   ←  {ok:true}
//
// GET  /gameserver/<id>/poll        →  drain queue of pending Takaro requests
//                                   ←  {operations:[{operationId, action, argsJson}]}
//
// POST /gameserver/<id>/operation/<opId>/result
//                                   →  response(requestId=opId, payload)
//                                   ←  {ok:true}

import http from 'node:http';
import { logger } from '../logger.js';
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

  constructor(
    private port: number,
    private ws: TakaroWsClient,
    private serverName: string,
    private identityToken: string,
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
        argsJson: typeof payload.args === 'string' ? payload.args : JSON.stringify(payload.args ?? {}),
      });
      this.operationActions.set(requestId, payload.action);
      logger.debug(`Queued ${payload.action} (op=${requestId})`);
    });
  }

  start(): void {
    this.server = http.createServer((req, res) => this.handle(req, res));
    this.server.listen(this.port, '127.0.0.1', () => {
      logger.info(`HTTP listening on 127.0.0.1:${this.port} (mod-facing)`);
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

    try {
      // Health
      if (method === 'GET' && path === '/health') {
        return this.send(res, 200, {
          ok: true,
          identified: this.ws.identified(),
          gameServerId: this.ws.getGameServerId(),
          pendingOperations: this.operationQueue.length,
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
          const batch = body ? JSON.parse(body) : { events: [] };
          const events = (batch.events || []) as TakaroEvent[];
          for (const ev of events) {
            // strip the type field from the data payload — Takaro takes type separately
            const { type, ...rest } = ev;
            this.ws.sendGameEvent(type as GameEventType, rest);
          }
          logger.debug(`Forwarded ${events.length} events`);
          return this.send(res, 200, { ok: true, forwarded: events.length });
        }

        if (method === 'GET' && parsed.rest === 'poll') {
          const ops = this.operationQueue.splice(0, this.operationQueue.length);
          return this.send(res, 200, { operations: ops });
        }

        const opMatch = parsed.rest.match(/^operation\/([^/]+)\/result$/);
        if (method === 'POST' && opMatch) {
          const opId = opMatch[1]!;
          const body = await this.readBody(req);
          const obj = body ? JSON.parse(body) : {};
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
