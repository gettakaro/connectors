import http, { type IncomingMessage, type ServerResponse } from 'node:http';
import type { PluginEvent, PluginHealth, PluginInventoryItem, PluginPlayer, Position } from '../enshrouded/types.js';

export interface MockRequest {
  method: string;
  path: string;
  query: Record<string, string>;
  body?: any;
  auth?: string;
}

/** In-memory implementation of the Enshrouded plugin HTTP contract, for tests and local dry runs. */
export class MockPlugin {
  token = 'test-token';
  health: PluginHealth = {
    status: 'ok',
    version: '0.1.0-mock',
    gameBuild: '1024233',
    capabilities: { logEvents: 'ok', players: 'ok', gameThread: 'ok', chatEvents: 'unimplemented' },
  };
  players: PluginPlayer[] = [
    { gameId: '76561198000005875', name: 'Limon', steamId: '76561198000005875', peerId: '0(1)', group: 'Admins', online: true, position: { x: 10.5, y: 20, z: -3 } },
    { gameId: '76561198000001111', name: 'Guest', steamId: '76561198000001111', peerId: '0(2)', group: 'Guests', online: true },
  ];
  locations: Record<string, Position> = { '76561198000005875': { x: 10.5, y: 20, z: -3 }, '76561198000001111': { x: 1, y: 2, z: 3 } };
  inventories: Record<string, PluginInventoryItem[]> = {
    '76561198000005875': [{ code: 'Wood', name: 'Wood Log', amount: 25 }, { code: 'Sword_Iron', name: 'Iron Sword', amount: 1, quality: 3 }],
  };
  items = [{ code: 'Wood', name: 'Wood Log', description: 'Basic material' }, { code: 'Torch', name: 'Torch' }];
  entities = [
    { code: 'Scavenger', name: 'Scavenger', type: 'enemy' },
    { code: 'Blacksmith', name: 'Blacksmith', type: 'npc' },
    { code: 'Wolf', name: 'Wolf', type: 'animal' },
  ];
  locationsList = [{ code: 'cradle', name: 'Cradle', position: { x: 0, y: 100, z: 0 }, radius: 50 }];
  bans: Array<{ gameId: string; steamId?: string; name: string; reason?: string; expiresAt?: string | null }> = [];
  events: PluginEvent[] = [];
  seqBase = 0;
  bootId: string | undefined = undefined;
  unimplemented = new Set<string>();
  requests: MockRequest[] = [];
  commandHandler: (command: string) => { success: boolean; output: string } = (command) => ({ success: true, output: `ran ${command}` });

  private server: http.Server | null = null;

  pushEvent(type: string, data: unknown): PluginEvent {
    const last = this.events.length ? this.events[this.events.length - 1].seq : this.seqBase;
    const event = { seq: last + 1, type, data, ts: new Date().toISOString() };
    this.events.push(event);
    return event;
  }

  url(): string {
    const addr = this.server?.address();
    if (!addr || typeof addr !== 'object') throw new Error('mock plugin not listening');
    return `http://127.0.0.1:${addr.port}`;
  }

  lastRequest(method: string, path: string): MockRequest | undefined {
    return [...this.requests].reverse().find((r) => r.method === method && r.path === path);
  }

  start(port = 0): Promise<string> {
    this.server = http.createServer((req, res) => void this.route(req, res));
    return new Promise((resolve) => this.server!.listen(port, '127.0.0.1', () => resolve(this.url())));
  }

  stop(): Promise<void> {
    const s = this.server;
    this.server = null;
    return new Promise((resolve) => (s ? s.close(() => resolve()) : resolve()));
  }

  private async route(req: IncomingMessage, res: ServerResponse): Promise<void> {
    const url = new URL(req.url ?? '/', 'http://x');
    const path = url.pathname;
    const method = req.method ?? 'GET';
    const body = method === 'POST' ? await readBody(req) : undefined;
    const record: MockRequest = { method, path, query: Object.fromEntries(url.searchParams), body, auth: req.headers.authorization };
    this.requests.push(record);

    if (req.headers.authorization !== `Bearer ${this.token}`) return json(res, 401, { error: 'unauthorized' });

    const routeKey = `${method} ${path.replace(/^\/players\/[^/]+/, '/players/:id')}`;
    if (this.unimplemented.has(routeKey)) return json(res, 501, { error: 'not implemented in this build' });

    const playerMatch = /^\/players\/([^/]+)(\/location|\/inventory)?$/.exec(path);
    if (method === 'GET' && playerMatch) {
      const id = decodeURIComponent(playerMatch[1]);
      const player = this.players.find((p) => p.gameId === id || p.steamId === id);
      if (!player) return json(res, 404, { error: 'player not found' });
      if (playerMatch[2] === '/location') return json(res, 200, this.locations[player.gameId] ?? player.position);
      if (playerMatch[2] === '/inventory') return json(res, 200, this.inventories[player.gameId] ?? []);
      return json(res, 200, player);
    }

    switch (`${method} ${path}`) {
      case 'GET /health':
        return json(res, 200, this.health);
      case 'GET /players':
        return json(res, 200, this.players);
      case 'GET /events': {
        const since = Number(url.searchParams.get('since') ?? 0);
        const seq = this.events.length ? this.events[this.events.length - 1].seq : this.seqBase;
        return json(res, 200, { ...(this.bootId ? { bootId: this.bootId } : {}), seq, events: this.events.filter((e) => e.seq > since) });
      }
      case 'POST /message':
        if (!body?.text) return json(res, 400, { error: 'text required' });
        return json(res, 200, { ok: true });
      case 'POST /teleport':
      case 'POST /give':
      case 'POST /kick': {
        const player = this.players.find((p) => p.gameId === body?.gameId);
        if (!player) return json(res, 404, { error: 'player not online' });
        if (path === '/teleport') this.locations[player.gameId] = { x: body.x, y: body.y, z: body.z };
        if (path === '/kick') player.online = false;
        return json(res, 200, { ok: true });
      }
      case 'POST /ban': {
        const player = this.players.find((p) => p.gameId === body?.gameId || p.steamId === body?.gameId);
        this.bans.push({ gameId: player?.gameId ?? body.gameId, steamId: player?.steamId ?? body.gameId, name: player?.name ?? body.gameId, reason: body.reason, expiresAt: body.expiresAt ?? null });
        return json(res, 200, { ok: true });
      }
      case 'POST /unban':
        this.bans = this.bans.filter((b) => b.gameId !== body?.gameId && b.steamId !== body?.gameId);
        return json(res, 200, { ok: true });
      case 'GET /bans':
        return json(res, 200, this.bans);
      case 'GET /items':
        return json(res, 200, this.items);
      case 'GET /entities':
        return json(res, 200, this.entities);
      case 'GET /locations':
        return json(res, 200, this.locationsList);
      case 'POST /command':
        return json(res, 200, this.commandHandler(String(body?.command ?? '')));
      case 'POST /shutdown':
        return json(res, 200, { ok: true });
      default:
        return json(res, 404, { error: `no route ${method} ${path}` });
    }
  }
}

function json(res: ServerResponse, status: number, body: unknown): void {
  res.writeHead(status, { 'content-type': 'application/json' });
  res.end(JSON.stringify(body));
}

async function readBody(req: IncomingMessage): Promise<any> {
  const chunks: Buffer[] = [];
  for await (const chunk of req) chunks.push(Buffer.isBuffer(chunk) ? chunk : Buffer.from(chunk));
  const raw = Buffer.concat(chunks).toString('utf8');
  return raw ? JSON.parse(raw) : {};
}
