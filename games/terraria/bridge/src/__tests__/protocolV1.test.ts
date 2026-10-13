import assert from 'node:assert/strict';
import { mkdtempSync, writeFileSync } from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { test } from 'node:test';
import { WebSocketServer, WebSocket } from 'ws';
import { buildManifest } from '../takaro/capabilities.js';
import { TakaroWsClient } from '../takaro/client.js';
import { MAP_DIMENSION_ID, MapImageError, MapImageStore, mapCapability, parseMeta } from '../takaro/mapImage.js';
import type { WsMessage } from '../takaro/protocol.js';

function writeMap(dir: string, revision: string, bytes: Buffer): void {
  const file = `map-${revision}.png`;
  writeFileSync(path.join(dir, file), bytes);
  writeFileSync(path.join(dir, 'map.json'), JSON.stringify({
    revision,
    file,
    mimeType: 'image/png',
    size: bytes.length,
    width: 4200,
    height: 1200,
    bounds: { x: { min: 0, max: 67200 }, y: { min: 0, max: 19200 } },
  }));
}

test('the manifest declares live-supported actions and events only, plus the map once rendered', () => {
  const names = buildManifest(null).map((entry) => entry.name);
  assert.ok(names.includes('players.location'));
  assert.ok(names.includes('players.inventory'));
  assert.ok(names.includes('events.chat-message'));
  assert.ok(!names.includes('entities.list'), 'schema-fallback actions are never declared');
  assert.ok(!names.includes('locations.list'));
  assert.ok(!names.includes('map'));
  assert.equal(new Set(names).size, names.length);
  for (const name of names) assert.match(name, /^[a-z][a-zA-Z0-9]*(\.[a-z][a-zA-Z0-9-]*)*$/);

  const meta = parseMeta({
    revision: 'abc', file: 'map-abc.png', mimeType: 'image/png', size: 10, width: 4200, height: 1200,
    bounds: { x: { min: 0, max: 67200 }, y: { min: 0, max: 19200 } },
  });
  const map = buildManifest(meta).find((entry) => entry.name === 'map');
  assert.deepEqual(map, mapCapability(meta));
  const dimension = (map!.parameters as { dimensions: Array<Record<string, any>> }).dimensions[0];
  assert.equal(dimension.id, MAP_DIMENSION_ID);
  assert.deepEqual(dimension.axes, { horizontal: { field: 'x', direction: 'right' }, vertical: { field: 'y', direction: 'down' } });
  assert.deepEqual(dimension.bounds, { horizontal: { min: 0, max: 67200 }, vertical: { min: 0, max: 19200 } });
  assert.deepEqual(dimension.source, { type: 'image', revision: 'abc', size: 10, width: 4200, height: 1200, mimeType: 'image/png' });
});

test('map metadata from disk is validated before it is declared', () => {
  const good = {
    revision: 'r1', file: 'map-r1.png', mimeType: 'image/png', size: 1, width: 1, height: 1,
    bounds: { x: { min: 0, max: 16 }, y: { min: 0, max: 16 } },
  };
  assert.ok(parseMeta(good));
  assert.equal(parseMeta({ ...good, file: '../../etc/passwd' }), null);
  assert.equal(parseMeta({ ...good, revision: 'has space' }), null);
  assert.equal(parseMeta({ ...good, width: 9000 }), null);
  assert.equal(parseMeta({ ...good, mimeType: 'image/gif' }), null);
  assert.equal(parseMeta({ ...good, bounds: { x: { min: 5, max: 5 }, y: { min: 0, max: 1 } } }), null);
  assert.equal(parseMeta(null), null);
});

test('getMapImage serves exact chunks of the declared revision and refuses bad ranges', async () => {
  const dir = mkdtempSync(path.join(os.tmpdir(), 'terraria-map-'));
  const bytes = Buffer.from(Array.from({ length: 2500 }, (_, i) => i % 251));
  const store = new MapImageStore(dir);
  assert.equal(await store.refresh(), false, 'no map yet');
  await assert.rejects(store.chunk({ dimension: MAP_DIMENSION_ID, offset: 0, length: 10 }), (err: MapImageError) => err.code === 'not_found');

  writeMap(dir, 'rev1', bytes);
  assert.equal(await store.refresh(), true);
  assert.equal(await store.refresh(), false, 'an unchanged revision is not a change');

  const parts: Buffer[] = [];
  for (let offset = 0; offset < bytes.length; offset += 1024) {
    // Takaro sends args as a JSON string.
    const chunk = await store.chunk(JSON.stringify({ dimension: MAP_DIMENSION_ID, offset, length: 1024 }));
    assert.equal(chunk.revision, 'rev1');
    parts.push(Buffer.from(chunk.data, 'base64'));
  }
  assert.deepEqual(parts.map((part) => part.length), [1024, 1024, 452]);
  assert.deepEqual(Buffer.concat(parts), bytes);

  await assert.rejects(store.chunk({ dimension: MAP_DIMENSION_ID, offset: 2500, length: 1 }), (err: MapImageError) => err.code === 'invalid_args');
  await assert.rejects(store.chunk({ dimension: MAP_DIMENSION_ID, offset: 0, length: 1024 * 1024 + 1 }), (err: MapImageError) => err.code === 'invalid_args');
  await assert.rejects(store.chunk({ dimension: 'nether', offset: 0, length: 1 }), (err: MapImageError) => err.code === 'not_found');

  writeMap(dir, 'rev2', Buffer.from('new'));
  assert.equal(await store.refresh(), true);
  assert.equal((await store.chunk({ dimension: MAP_DIMENSION_ID, offset: 0, length: 3 })).revision, 'rev2');
});

test('version 1: identify offers [0, 1] with the manifest, events carry seq and are resent until acknowledged', async () => {
  const server = new WebSocketServer({ port: 0, host: '127.0.0.1' });
  await new Promise<void>((resolve) => server.once('listening', resolve));
  const address = server.address();
  if (!address || typeof address === 'string') throw new Error('server did not bind');
  const identifies: Array<Record<string, unknown>> = [];
  const events: Array<{ connection: number; seq: number; streamId: string; type: string }> = [];
  const frames: WsMessage[] = [];
  let connections = 0;
  let ackUpTo = 0;

  server.on('connection', (socket) => {
    const connection = ++connections;
    socket.on('message', (raw) => {
      const message = JSON.parse(raw.toString()) as WsMessage;
      frames.push(message);
      if (message.type === 'identify') {
        identifies.push(message.payload as Record<string, unknown>);
        socket.send(JSON.stringify({ type: 'identifyResponse', payload: { gameServerId: 'gs', protocolVersion: 1 } }));
      }
      if (message.type === 'gameEvent') {
        const payload = message.payload as { seq: number; streamId: string; type: string };
        events.push({ connection, ...payload });
        if (ackUpTo) socket.send(JSON.stringify({ type: 'eventAck', payload: { streamId: payload.streamId, upTo: ackUpTo } }));
      }
      if (message.type === 'capabilities') {
        socket.send(JSON.stringify({ type: 'capabilitiesResponse', payload: null, requestId: message.requestId }));
      }
    });
  });

  let manifest: unknown[] = [{ name: 'players.list' }];
  const client = new TakaroWsClient(`ws://127.0.0.1:${address.port}`, {
    identityToken: 'identity', registrationToken: 'registration', name: 'Terraria Test',
  }, 10, 20, { offerV1: true, manifest: () => manifest });

  try {
    client.connect();
    await waitFor(() => client.identified());
    assert.equal(client.getProtocolVersion(), 1);
    assert.deepEqual(identifies[0].protocolVersions, [0, 1]);
    assert.deepEqual(identifies[0].capabilities, [{ name: 'players.list' }]);

    client.sendGameEvent('chat-message', { msg: 'one' });
    client.sendGameEvent('chat-message', { msg: 'two' });
    await waitFor(() => events.length === 2);
    assert.deepEqual(events.map((e) => e.seq), [1, 2]);
    assert.equal(client.unacknowledged(), 2);

    // Drop the connection without an ack: both events come back on the new connection.
    for (const socket of server.clients) socket.terminate();
    await waitFor(() => connections === 2 && client.identified() && events.length === 4);
    assert.deepEqual(events.slice(2).map((e) => [e.connection, e.seq]), [[2, 1], [2, 2]]);
    assert.equal(new Set(events.map((e) => e.streamId)).size, 1, 'one stream per process');

    ackUpTo = 3;
    client.sendGameEvent('log', { msg: 'three' });
    await waitFor(() => client.unacknowledged() === 0);

    // Errors use the version 1 shape.
    client.sendError('req-1', 'Player 1 is not online', 'not_found');
    await waitFor(() => frames.some((f) => f.requestId === 'req-1'));
    const error = frames.find((f) => f.requestId === 'req-1')!;
    assert.equal(error.type, 'response');
    assert.deepEqual(error.error, { code: 'not_found', message: 'Player 1 is not online' });

    // A manifest replacement sends the complete list again.
    manifest = [{ name: 'players.list' }, { name: 'map', parameters: { dimensions: [] } }];
    let accepted = false;
    client.once('capabilitiesAccepted', () => { accepted = true; });
    assert.equal(client.sendCapabilities(), true);
    await waitFor(() => accepted);
    assert.deepEqual((frames.find((f) => f.type === 'capabilities')!.payload as { capabilities: unknown[] }).capabilities, manifest);
  } finally {
    client.shutdown();
    for (const socket of server.clients) {
      if (socket.readyState === WebSocket.OPEN || socket.readyState === WebSocket.CLOSING) socket.terminate();
    }
    await new Promise<void>((resolve) => server.close(() => resolve()));
  }
});

test('version 0 fallback: a Takaro that answers without protocolVersion gets plain events and error frames', async () => {
  const server = new WebSocketServer({ port: 0, host: '127.0.0.1' });
  await new Promise<void>((resolve) => server.once('listening', resolve));
  const address = server.address();
  if (!address || typeof address === 'string') throw new Error('server did not bind');
  const frames: WsMessage[] = [];
  server.on('connection', (socket) => {
    socket.on('message', (raw) => {
      const message = JSON.parse(raw.toString()) as WsMessage;
      frames.push(message);
      if (message.type === 'identify') socket.send(JSON.stringify({ type: 'identifyResponse', payload: { gameServerId: 'gs' } }));
    });
  });
  const client = new TakaroWsClient(`ws://127.0.0.1:${address.port}`, {
    identityToken: 'identity', registrationToken: 'registration', name: 'Terraria Test',
  }, 10, 20, { offerV1: true, manifest: () => [] });
  try {
    client.connect();
    await waitFor(() => client.identified());
    assert.equal(client.getProtocolVersion(), 0);
    assert.equal(client.sendCapabilities(), false);
    client.sendGameEvent('log', { msg: 'x' });
    client.sendError('req-0', 'boom');
    await waitFor(() => frames.some((f) => f.requestId === 'req-0'));
    const event = frames.find((f) => f.type === 'gameEvent')!;
    assert.deepEqual(event.payload, { type: 'log', data: { msg: 'x' } });
    assert.equal(client.unacknowledged(), 0);
    assert.deepEqual(frames.find((f) => f.requestId === 'req-0'), { type: 'error', requestId: 'req-0', payload: { message: 'boom' } });
  } finally {
    client.shutdown();
    for (const socket of server.clients) socket.terminate();
    await new Promise<void>((resolve) => server.close(() => resolve()));
  }
});

test('a version 1 identify failure (top-level error) is reported, not treated as identified', async () => {
  const server = new WebSocketServer({ port: 0, host: '127.0.0.1' });
  await new Promise<void>((resolve) => server.once('listening', resolve));
  const address = server.address();
  if (!address || typeof address === 'string') throw new Error('server did not bind');
  server.on('connection', (socket) => {
    socket.on('message', () => {
      socket.send(JSON.stringify({ type: 'identifyResponse', payload: null, error: { code: 'invalid_args', message: 'No registrationToken provided' } }));
    });
  });
  const client = new TakaroWsClient(`ws://127.0.0.1:${address.port}`, {
    identityToken: 'identity', registrationToken: 'registration', name: 'Terraria Test',
  }, 1000, 1000, { offerV1: true, manifest: () => [] });
  let reported: unknown = null;
  client.on('identifyError', (error) => { reported = error; });
  try {
    client.connect();
    await waitFor(() => reported !== null);
    assert.deepEqual(reported, { code: 'invalid_args', message: 'No registrationToken provided' });
    assert.equal(client.identified(), false);
  } finally {
    client.shutdown();
    for (const socket of server.clients) socket.terminate();
    await new Promise<void>((resolve) => server.close(() => resolve()));
  }
});

async function waitFor(predicate: () => boolean, timeoutMs = 3000): Promise<void> {
  const deadline = Date.now() + timeoutMs;
  while (!predicate()) {
    if (Date.now() > deadline) throw new Error('timed out');
    await new Promise((resolve) => setTimeout(resolve, 10));
  }
}
