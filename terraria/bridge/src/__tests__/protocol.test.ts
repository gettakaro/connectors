import assert from 'node:assert/strict';
import { test } from 'node:test';
import { WebSocketServer, WebSocket } from 'ws';
import { TerrariaAdapter, type TShockApi } from '../terraria/adapter.js';
import { TakaroWsClient } from '../takaro/client.js';
import { ALL_GAME_SERVER_ACTIONS, type WsMessage } from '../takaro/protocol.js';

class FakeTShock implements TShockApi {
  async testToken() { return { success: true, rawResult: 'ok' }; }
  async status() { return { name: 'Terraria', port: 7777, playercount: 0 }; }
  async players() { return []; }
  async broadcast(message: string) { return { success: true, rawResult: message }; }
  async rawCommand(command: string) { return { success: true, rawResult: command }; }
  async createBan() { return { success: true, rawResult: 'ban created' }; }
  async destroyBan() { return { success: true, rawResult: 'ban removed' }; }
  async listBans() { return []; }
  async shutdown() { return { success: true, rawResult: 'shutdown' }; }
}

test('Takaro WebSocket client identifies, pongs, and responds exactly once for every action', async () => {
  const server = new WebSocketServer({ port: 0, host: '127.0.0.1' });
  await new Promise<void>((resolve) => server.once('listening', resolve));
  const address = server.address();
  if (!address || typeof address === 'string') throw new Error('server did not bind');
  const url = `ws://127.0.0.1:${address.port}`;
  const responses = new Map<string, number>();
  let identified = false;
  let ponged = false;

  server.on('connection', (socket) => {
    socket.on('message', (raw) => {
      const message = JSON.parse(raw.toString()) as WsMessage;
      if (message.type === 'identify') {
        identified = true;
        socket.send(JSON.stringify({ type: 'identifyResponse', payload: { gameServerId: 'gs_terraria' } }));
        socket.send(JSON.stringify({ type: 'ping' }));
        for (const action of ALL_GAME_SERVER_ACTIONS) {
          socket.send(JSON.stringify({ type: 'request', requestId: `req-${action}`, payload: { action, args: argsFor(action) } }));
        }
      }
      if (message.type === 'pong') ponged = true;
      if (message.type === 'response') {
        responses.set(message.requestId || '', (responses.get(message.requestId || '') || 0) + 1);
      }
    });
  });

  const adapter = new TerrariaAdapter(new FakeTShock(), {
    commandAllowlistExact: ['help'],
    commandAllowlistPrefixes: ['say'],
    enableShutdown: true,
  });
  const client = new TakaroWsClient(url, {
    identityToken: 'identity',
    registrationToken: 'registration',
    name: 'Terraria Test',
  });
  client.on('request', (message) => {
    void adapter.handleAction(message.payload?.action, message.payload?.args)
      .then((result) => client.sendResponse(message.requestId!, result));
  });

  try {
    client.connect();
    await waitFor(() => responses.size === ALL_GAME_SERVER_ACTIONS.length);
  } finally {
    client.shutdown();
    for (const socket of server.clients) {
      if (socket.readyState === WebSocket.OPEN || socket.readyState === WebSocket.CLOSING) socket.terminate();
    }
    await new Promise<void>((resolve) => server.close(() => resolve()));
  }

  assert.equal(identified, true);
  assert.equal(ponged, true);
  for (const action of ALL_GAME_SERVER_ACTIONS) {
    assert.equal(responses.get(`req-${action}`), 1, action);
  }
});

test('Takaro WebSocket client sends game events in payload form after identification', async () => {
  const server = new WebSocketServer({ port: 0, host: '127.0.0.1' });
  await new Promise<void>((resolve) => server.once('listening', resolve));
  const address = server.address();
  if (!address || typeof address === 'string') throw new Error('server did not bind');
  const messages: WsMessage[] = [];

  server.on('connection', (socket) => {
    socket.on('message', (raw) => {
      const message = JSON.parse(raw.toString()) as WsMessage;
      messages.push(message);
      if (message.type === 'identify') {
        socket.send(JSON.stringify({ type: 'identifyResponse', payload: { gameServerId: 'gs_terraria' } }));
      }
    });
  });

  const client = new TakaroWsClient(`ws://127.0.0.1:${address.port}`, {
    identityToken: 'identity',
    registrationToken: 'registration',
    name: 'Terraria Test',
  });

  try {
    client.connect();
    await waitFor(() => client.identified());
    client.sendGameEvent('player-death', { player: { gameId: 'Guide', name: 'Guide' }, reason: 'test' });
    await waitFor(() => messages.some((message) => message.type === 'gameEvent'));
  } finally {
    client.shutdown();
    for (const socket of server.clients) {
      if (socket.readyState === WebSocket.OPEN || socket.readyState === WebSocket.CLOSING) socket.terminate();
    }
    await new Promise<void>((resolve) => server.close(() => resolve()));
  }

  const event = messages.find((message) => message.type === 'gameEvent')!;
  assert.deepEqual(event.payload, {
    type: 'player-death',
    data: { player: { gameId: 'Guide', name: 'Guide' }, reason: 'test' },
  });
});

function argsFor(action: string): unknown {
  if (action === 'executeConsoleCommand') return { command: 'help' };
  if (action === 'sendMessage') return { message: 'hello' };
  if (['getPlayer', 'getPlayerLocation', 'getPlayerInventory', 'kickPlayer', 'banPlayer', 'unbanPlayer', 'giveItem', 'teleportPlayer'].includes(action)) {
    return { player: { name: 'Guide', gameId: 'guide-user' }, itemCode: '1', amount: 1, x: 1, y: 2, reason: 'test' };
  }
  return {};
}

async function waitFor(predicate: () => boolean): Promise<void> {
  const started = Date.now();
  while (!predicate()) {
    if (Date.now() - started > 2000) throw new Error('timed out waiting for responses');
    await new Promise((resolve) => setTimeout(resolve, 20));
  }
}
