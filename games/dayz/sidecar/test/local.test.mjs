// The mod-facing HTTP server: first contact, game version, game icon.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { EventEmitter } from 'node:events';
import { LocalHttpServer, eventTimestamp } from '../dist/local/server.js';

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
