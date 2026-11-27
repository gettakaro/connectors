// The mod-facing HTTP server: first contact, game version, game icon.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { EventEmitter } from 'node:events';
import { LocalHttpServer, eventTimestamp, reachability } from '../dist/local/server.js';

class FakeWs extends EventEmitter {
  responses = [];
  errors = [];
  identified() { return false; }
  connected() { return false; }
  getGameServerId() { return null; }
  getProtocolVersion() { return 1; }
  unacknowledged() { return 0; }
  sendResponse(id, payload) { this.responses.push({ id, payload }); }
  sendError(id, message, code) { this.errors.push({ id, message, code }); }
  refreshCapabilities() { return false; }
}

async function withServer(icon, fn) {
  const ws = new FakeWs();
  const port = 20000 + Math.floor(Math.random() * 20000);
  const local = new LocalHttpServer(port, ws, 'n', 'i', '127.0.0.1', null, icon);
  local.start();
  await new Promise((r) => setTimeout(r, 50));
  try {
    await fn({ ws, local, url: `http://127.0.0.1:${port}` });
  } finally {
    local.stop();
  }
}

test('the mod\'s first request (not /health) signals contact once; its poll reports the game build', async () => {
  await withServer(null, async ({ local, url }) => {
    let contacts = 0;
    let version = null;
    local.onModContact(() => contacts++);
    local.onGameVersion((v) => (version = v));
    await fetch(`${url}/health`);
    assert.equal(contacts, 0, '/health is not the mod');
    assert.equal(local.modHasContacted(), false);
    const reg = await fetch(`${url}/gameserver/register`, { method: 'POST', body: '{}' });
    assert.equal(reg.status, 503, 'register waits until Takaro identified the server');
    await fetch(`${url}/gameserver/local/poll?gameVersion=1.30.164289.70`);
    assert.equal(contacts, 1);
    assert.equal(local.modHasContacted(), true);
    assert.equal(version, '1.30.164289.70');
    assert.equal(local.getGameVersion(), '1.30.164289.70');
  });
});

test('getGameIcon is answered by the sidecar, or not_found without an icon', async () => {
  const icon = { revision: 'abcdef123456', size: 3, data: 'AAAA' };
  await withServer(icon, async ({ ws }) => {
    ws.emit('request', { requestId: 'r1', payload: { action: 'getGameIcon', args: '{}' } });
    assert.deepEqual(ws.responses, [{ id: 'r1', payload: { revision: 'abcdef123456', data: 'AAAA' } }]);
  });
  await withServer(null, async ({ ws }) => {
    ws.emit('request', { requestId: 'r2', payload: { action: 'getGameIcon', args: '{}' } });
    assert.equal(ws.errors[0].code, 'not_found');
  });
});

test('events keep the mod\'s UTC time unless it is missing or implausible', () => {
  const now = Date.parse('2026-11-27T17:32:11.000Z');
  assert.equal(eventTimestamp('2026-11-27T17:30:21Z', now), '2026-11-27T17:30:21.000Z');
  assert.equal(eventTimestamp(undefined, now), '2026-11-27T17:32:11.000Z');
  assert.equal(eventTimestamp('26-09-20T11:26:00Z', now), '2026-11-27T17:32:11.000Z');
  assert.equal(eventTimestamp('2026-11-27T18:40:00Z', now), '2026-11-27T17:32:11.000Z', 'future');
  assert.equal(eventTimestamp('2026-11-20T17:30:21Z', now), '2026-11-27T17:32:11.000Z', 'a week old');
});

test('actions DayZ does not have are answered unsupported, not queued for the mod', async () => {
  await withServer(null, async ({ ws, url }) => {
    ws.emit('request', { requestId: 'p1', payload: { action: 'getPlayerProgression', args: '{}' } });
    ws.emit('request', { requestId: 'u1', payload: { action: 'fooBarBaz', args: '{}' } });
    ws.emit('request', { requestId: 'g1', payload: { action: 'getPlayers', args: '{}' } });
    assert.deepEqual(ws.errors.map((e) => [e.id, e.code]), [['p1', 'unsupported'], ['u1', 'unsupported']]);
    const poll = await (await fetch(`${url}/gameserver/local/poll`)).json();
    assert.deepEqual(poll.operations.filter((o) => !o.operationId.startsWith('sidecar-')).map((o) => o.action), ['getPlayers']);
  });
});

test('listBans: real names only; a ban without a known name is left out', async () => {
  await withServer(null, async ({ ws, url }) => {
    ws.emit('request', { requestId: 'b1', payload: { action: 'listBans', args: '{}' } });
    const poll = await (await fetch(`${url}/gameserver/local/poll`)).json();
    assert.ok(poll.operations.some((o) => o.operationId === 'b1'));
    const result = [
      { player: { gameId: '76561198000000001', name: 'Alice' }, reason: 'r', expiresAt: null },
      { player: { gameId: '76561198000000002' }, reason: 'r', expiresAt: null },
    ];
    await fetch(`${url}/gameserver/local/operation/b1/result`, { method: 'POST', body: JSON.stringify({ ok: true, result }) });
    assert.deepEqual(ws.responses.find((r) => r.id === 'b1').payload, [
      { player: { gameId: '76561198000000001', name: 'Alice', steamId: '76561198000000001' }, reason: 'r', expiresAt: null },
    ]);
  });
});

test('reachability follows the mod\'s calls, answered by the sidecar', async () => {
  assert.deepEqual(reachability(1000, 5000), { connectable: true, reason: null });
  assert.equal(reachability(1000, 30000).connectable, false);
  assert.equal(reachability(null, 5000).connectable, false);
  await withServer(null, async ({ ws, url }) => {
    ws.emit('request', { requestId: 'r0', payload: { action: 'testReachability', args: '{}' } });
    assert.equal(ws.responses.find((r) => r.id === 'r0').payload.connectable, false, 'no mod yet');
    await fetch(`${url}/gameserver/register`, { method: 'POST', body: '{}' });
    ws.emit('request', { requestId: 'r1', payload: { action: 'testReachability', args: '{}' } });
    assert.deepEqual(ws.responses.find((r) => r.id === 'r1').payload, { connectable: true, reason: null });
  });
});
