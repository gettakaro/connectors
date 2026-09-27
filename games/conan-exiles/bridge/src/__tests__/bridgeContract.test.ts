import assert from 'node:assert/strict';
import { randomUUID } from 'node:crypto';
import type { AddressInfo } from 'node:net';
import { after, test } from 'node:test';
import { WebSocketServer, type WebSocket } from 'ws';
import { startBridge, type RunningBridge } from '../app.js';
import type { BridgeConfig } from '../config.js';
import type { TargetStamp } from '../targetStamp.js';
import { startFakeRconServer, type FakeRconServer } from './helpers/fakeRcon.js';

/**
 * The portable end-to-end check: a real bridge between a fake Conan server and a fake
 * Takaro, in one process.
 *
 * Everything Takaro actually asks a Conan server for goes over a real WebSocket into the
 * real client, through the real adapter, out of a real RCON socket and back. What it does
 * not cover is stated rather than implied: there is no game here, so nothing about chat
 * delivery, the Pippi mod helper, the DevKit package or gameplay events is verified by
 * this file — `takaro-maint verify` and the dev-servers rig do that against a real server.
 */

// Deliberately not UUID-shaped: the bridge treats the id as an opaque string, and a
// literal UUID in a test reads like a real one somebody pasted in.
const GAME_SERVER_ID = 'contract-test-game-server';
const STAMP: TargetStamp = {
  target: 'linux-25488622',
  fingerprint: '0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef',
  game: 'conan-exiles',
  platform: 'linux',
  revision: '25488622',
  connectorVersion: '1.0.2-test',
  sourceRevision: 'deadbeef',
};
const LISTPLAYERS_HEADER = 'Idx | Char name | Player name | User ID | Platform ID | Platform Name';

const RCON_RESPONSES: Record<string, string> = {
  help: 'Available commands: help, listplayers, shutdown',
  listplayers: LISTPLAYERS_HEADER,
  shutdown: 'Shutting down',
};

class FakeTakaro {
  readonly server: WebSocketServer;
  identifyCount = 0;
  lastIdentify: Record<string, unknown> | null = null;
  private socket: WebSocket | null = null;
  private readonly pending = new Map<string, (value: unknown) => void>();
  private readonly waiters: (() => void)[] = [];

  private constructor(server: WebSocketServer) {
    this.server = server;
    server.on('connection', (socket) => {
      this.socket = socket;
      socket.send(JSON.stringify({ type: 'connected' }));
      socket.on('message', (raw) => this.handle(socket, String(raw)));
    });
  }

  static async start(): Promise<FakeTakaro> {
    const server = new WebSocketServer({ host: '127.0.0.1', port: 0 });
    await new Promise<void>((resolve) => server.once('listening', resolve));
    return new FakeTakaro(server);
  }

  get url(): string {
    return `ws://127.0.0.1:${(this.server.address() as AddressInfo).port}/`;
  }

  private handle(socket: WebSocket, raw: string): void {
    const message = JSON.parse(raw) as { type: string; requestId?: string; payload?: unknown };
    if (message.type === 'identify') {
      this.identifyCount += 1;
      this.lastIdentify = (message.payload ?? {}) as Record<string, unknown>;
      socket.send(JSON.stringify({ type: 'identifyResponse', payload: { gameServerId: GAME_SERVER_ID } }));
      for (const waiter of this.waiters.splice(0)) waiter();
      return;
    }
    if ((message.type === 'response' || message.type === 'error') && message.requestId) {
      const resolve = this.pending.get(message.requestId);
      if (resolve) {
        this.pending.delete(message.requestId);
        resolve(message.type === 'error' ? { error: message.payload } : message.payload);
      }
    }
  }

  /** Ask the connector to perform one action and wait for its answer. */
  async request(action: string, args: Record<string, unknown> = {}): Promise<unknown> {
    const requestId = randomUUID();
    const answered = new Promise<unknown>((resolve) => this.pending.set(requestId, resolve));
    this.socket?.send(JSON.stringify({ type: 'request', requestId, payload: { action, args } }));
    return withTimeout(answered, 5000, `no response to ${action}`);
  }

  /** Resolve once at least `minimum` identify frames have arrived. */
  async identifiedAtLeast(minimum: number, timeoutMs = 5000): Promise<void> {
    if (this.identifyCount >= minimum) return;
    await withTimeout(
      new Promise<void>((resolve) => {
        const check = (): void => {
          if (this.identifyCount >= minimum) resolve();
          else this.waiters.push(check);
        };
        this.waiters.push(check);
      }),
      timeoutMs,
      `only ${this.identifyCount} identify frame(s) arrived, wanted ${minimum}`,
    );
  }

  disconnect(code: number, reason: string): void {
    this.socket?.close(code, reason);
    this.socket = null;
  }

  async stop(): Promise<void> {
    for (const client of this.server.clients) client.terminate();
    await new Promise<void>((resolve) => this.server.close(() => resolve()));
  }
}

function withTimeout<T>(promise: Promise<T>, ms: number, message: string): Promise<T> {
  return Promise.race([
    promise,
    new Promise<T>((_resolve, reject) => setTimeout(() => reject(new Error(message)), ms).unref()),
  ]);
}

function config(rcon: FakeRconServer, url: string): BridgeConfig {
  return {
    registrationToken: 'a-throwaway-registration-token',
    identityToken: 'takaro-contract-test',
    serverName: 'takaro contract test',
    takaroWsUrl: url,
    rcon: {
      host: '127.0.0.1',
      port: rcon.port,
      password: 'secret',
      timeoutMs: 2000,
      commandGapMs: 0,
    },
    databasePath: null,
    itemCatalogPath: null,
    httpPort: 0,
    pollIntervalMs: 60_000,
    enableLogEvents: false,
    logFiles: [],
    requireModSourceAttribution: false,
  };
}

const started: { bridge?: RunningBridge; rcon?: FakeRconServer; takaro?: FakeTakaro } = {};

after(async () => {
  await started.bridge?.stop();
  await started.rcon?.close();
  await started.takaro?.stop();
});

test('the bridge answers every supported Takaro action over a real RCON socket', async () => {
  const rcon = await startFakeRconServer('secret', RCON_RESPONSES);
  started.rcon = rcon;
  const takaro = await FakeTakaro.start();
  started.takaro = takaro;

  // A base of 50 ms rather than the shipped 3 s: the reconnect behaviour is the same,
  // the wait is not.
  const bridge = await startBridge(config(rcon, takaro.url), { stamp: STAMP, reconnectMs: { base: 50, max: 100 } });
  started.bridge = bridge;

  await takaro.identifiedAtLeast(1);
  assert.deepEqual(takaro.lastIdentify, {
    identityToken: 'takaro-contract-test',
    registrationToken: 'a-throwaway-registration-token',
    name: 'takaro contract test',
  });

  const health = await (await fetch(`http://127.0.0.1:${bridge.healthPort()}/health`)).json();
  assert.equal(health.ok, true);
  assert.equal(health.takaroIdentified, true);
  assert.equal(health.gameServerId, GAME_SERVER_ID);
  assert.equal(health.target.target, 'linux-25488622');
  assert.equal(health.target.connectorVersion, '1.0.2-test');

  assert.deepEqual(await takaro.request('testReachability'), { connectable: true, reason: null });
  assert.ok(rcon.commands.includes('help'), 'reachability is a real RCON command, not a socket probe');

  assert.deepEqual(await takaro.request('getPlayers'), []);
  assert.ok(rcon.commands.includes('listplayers'));

  assert.deepEqual(await takaro.request('executeConsoleCommand', { command: 'listplayers' }), {
    success: true,
    rawResult: LISTPLAYERS_HEADER,
  });

  // Chat has no path at all without Enhanced Pippi and the mod helper, and the bridge is
  // expected to say so rather than broadcast over vanilla RCON and call it a message.
  const chat = (await takaro.request('sendMessage', { message: 'hello' })) as {
    success: boolean;
    error: string;
  };
  assert.equal(chat.success, false);
  assert.match(chat.error, /chat bridge is not connected/);

  const tile = (await takaro.request('getMapTile', { x: 0, y: 0, z: 0 })) as { success: boolean; error: string };
  assert.equal(tile.success, false);
  assert.match(tile.error, /not supported by the Conan Exiles RCON sidecar/);
});

test('the bridge comes back after Takaro closes the socket, and is usable again', async () => {
  const takaro = started.takaro!;
  const rcon = started.rcon!;
  const before = takaro.identifyCount;

  takaro.disconnect(1001, 'going away');
  await takaro.identifiedAtLeast(before + 1);

  assert.deepEqual(await takaro.request('testReachability'), { connectable: true, reason: null });
  assert.ok(rcon.commands.filter((command) => command === 'help').length >= 2);
});

test('shutdown reaches the server as an RCON command, and stop closes the health port', async () => {
  const takaro = started.takaro!;
  const rcon = started.rcon!;

  assert.deepEqual(await takaro.request('shutdown'), { success: true, rawResult: 'Shutting down' });
  assert.ok(rcon.commands.includes('shutdown'));

  const port = started.bridge!.healthPort();
  await started.bridge!.stop();
  started.bridge = undefined;
  await assert.rejects(() => fetch(`http://127.0.0.1:${port}/health`));
});
