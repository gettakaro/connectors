// Protocol v1: map Dimension, XYZ tiles, positions tagged with the Dimension, acknowledged events.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { WebSocketServer } from 'ws';
import { parseSurvey } from '../dist/map/survey.js';
import { prepareSurvey, renderFrameTile, isFrameTile, FRAME_TILE_SIZE, FRAME_MAX_ZOOM } from '../dist/map/render.js';
import { PNG_SIGNATURE } from '../dist/map/png.js';
import { MapService, MapTileError, parseFrameTileArgs, worldDisplayName } from '../dist/map/service.js';
import { buildManifest, withCharacter, withDimension, eventWithDimension, ACTION_CAPABILITIES } from '../dist/ws/capabilities.js';
import { TakaroWsClient } from '../dist/ws/client.js';
import { bridgeHealthy, slimArgs } from '../dist/local/server.js';

// Same synthetic 1024 m world as map.test.mjs: sea in the south quarter, a 20x10 m building at (512, 512).
function syntheticSurvey(world = 'testland') {
  const cols = 32, cell = 32, size = 1024, treecell = 64, tcols = 16;
  const hex4 = (v) => v.toString(16).padStart(4, '0');
  const lines = ['takaro-dayz-survey 1', `world ${world} size ${size} cell ${cell} cols ${cols} rows ${cols} treecell ${treecell}`];
  for (let r = 0; r < cols; r++) {
    let h = '', s = '', w = '';
    for (let c = 0; c < cols; c++) {
      const sea = r < 8;
      const height = sea ? -10 : 5 + Math.max(0, 200 - Math.hypot(c - 24, r - 24) * 20);
      h += hex4(Math.round((height + 100) * 10));
      s += sea ? '0' : r === 16 ? '3' : c < 16 ? '1' : '2';
      w += sea ? '1' : '0';
    }
    lines.push(`h ${r} ${h}`, `s ${r} ${s}`, `w ${r} ${w}`);
  }
  lines.push('b 5120 5120 1000 0 -100 -50 100 50');
  for (let r = 0; r < tcols; r++) lines.push(`t ${r} ${(r > 10 ? '20' : '00').repeat(tcols)}`);
  lines.push('S 0 sea_floor', 'S 1 cp_grass', 'S 2 cp_conifer_common1', 'S 3 asphalt_ext', 'end 1 80');
  return lines.join('\n') + '\n';
}

test('the manifest declares server-held characters that end on death', () => {
  const entry = buildManifest(null).find((c) => c.name === 'players.characters');
  assert.deepEqual(entry.parameters, { held: 'server', ends: ['death'] });
  assert.equal(buildManifest(null).some((c) => c.name === 'players.progression'), false);
});

test('flat mod character fields become one character object', () => {
  const player = { gameId: '1', name: 'Limon', characterId: '1_2_3_4', characterName: 'Linda' };
  assert.deepEqual(withCharacter(player), { gameId: '1', name: 'Limon', character: { id: '1_2_3_4', name: 'Linda' } });
});

test('a player with no character id gets no character member', () => {
  const offline = { gameId: '1', name: 'Limon', characterId: '', characterName: '' };
  assert.deepEqual(withCharacter(offline), { gameId: '1', name: 'Limon' });
});

test('a character without a name falls back to the player name', () => {
  assert.deepEqual(withCharacter({ name: 'Limon', characterId: 'x', characterName: '' }).character, { id: 'x', name: 'Limon' });
});

const px = (rgba, x, y) => Array.from(rgba.slice((y * FRAME_TILE_SIZE + x) * 4, (y * FRAME_TILE_SIZE + x) * 4 + 4));
const BUILDING = [112, 92, 84, 255];

function serviceWith(world) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'dayz-map-v1-'));
  fs.writeFileSync(path.join(dir, `${world}.survey`), syntheticSurvey(world));
  return { svc: new MapService(dir), dir };
}

test('declares the surveyed world as a tiled Dimension: x right, z up, bounds 0..size', () => {
  const { svc, dir } = serviceWith('chernarusplus');
  assert.deepEqual(svc.dimension(), {
    id: 'chernarusplus',
    name: 'Chernarus',
    axes: { horizontal: { field: 'x', direction: 'right' }, vertical: { field: 'z', direction: 'up' } },
    bounds: { horizontal: { min: 0, max: 1024 }, vertical: { min: 0, max: 1024 } },
    source: { type: 'tiled', tileSize: 256, minZoom: 0, maxZoom: FRAME_MAX_ZOOM },
  });
  fs.rmSync(dir, { recursive: true });
  assert.equal(new MapService(fs.mkdtempSync(path.join(os.tmpdir(), 'dayz-map-v1-'))).dimension(), null);
  assert.equal(worldDisplayName('enoch'), 'Livonia');
  assert.equal(worldDisplayName('deerisle'), 'deerisle');
});

test('XYZ frame tiles: row 0 is north, the south-west tile holds the sea, the building lands where it stands', () => {
  const p = prepareSurvey(parseSurvey(syntheticSurvey()));
  // zoom 2 on a 1024 m world: 4x4 tiles of 256 m. Tile (0, 3) is the south-west corner: sea.
  const [r, g, b, a] = px(renderFrameTile(p, 2, 0, 3), 128, 128);
  assert.equal(a, 255);
  assert.ok(b > r && b > g, `sea pixel should be blue, got ${[r, g, b]}`);
  // Tile (0, 0) is the north-west corner: land, not sea.
  const [r2, g2, b2] = px(renderFrameTile(p, 2, 0, 0), 128, 128);
  assert.ok(g2 > b2, `land pixel should be green, got ${[r2, g2, b2]}`);
  // The building at (512, 512) sits in tile (2, 1): east 512..768, north 512..768 (row 1 from the top).
  // East 515 is pixel 3, north 517 is pixel 768 - 517 = 251 (1 m per pixel).
  const t = renderFrameTile(p, 2, 2, 1);
  assert.deepEqual(px(t, 3, 251), BUILDING);
  assert.notDeepEqual(px(t, 9, 251), BUILDING);
  assert.ok(isFrameTile(0, 0, 0) && isFrameTile(FRAME_MAX_ZOOM, 31, 31));
  assert.ok(!isFrameTile(1, 2, 0) && !isFrameTile(FRAME_MAX_ZOOM + 1, 0, 0) && !isFrameTile(1, -1, 0));
});

test('frameTile answers {mimeType, data} and refuses unknown dimensions and tiles off the pyramid', () => {
  const { svc, dir } = serviceWith('chernarusplus');
  const tile = svc.frameTile('chernarusplus', 0, 0, 0);
  assert.equal(tile.mimeType, 'image/png');
  const png = Buffer.from(tile.data, 'base64');
  assert.ok(png.subarray(0, 8).equals(PNG_SIGNATURE));
  assert.equal(png.readUInt32BE(16), 256);
  assert.deepEqual(svc.frameTile('chernarusplus', 0, 0, 0), tile);
  assert.throws(() => svc.frameTile('enoch', 0, 0, 0), (e) => e instanceof MapTileError && e.code === 'not_found');
  assert.throws(() => svc.frameTile('chernarusplus', 1, 2, 0), (e) => e instanceof MapTileError && e.code === 'invalid_args');
  // The legacy v0 tiles stay as they were.
  assert.ok(Buffer.from(svc.tile(1, 1, 4), 'base64').subarray(0, 8).equals(PNG_SIGNATURE));
  fs.rmSync(dir, { recursive: true });
});

test('parseFrameTileArgs needs {dimension, zoom, x, y}', () => {
  assert.deepEqual(parseFrameTileArgs('{"dimension":"enoch","zoom":2,"x":"1","y":3}'), { dimension: 'enoch', zoom: 2, x: 1, y: 3 });
  assert.throws(() => parseFrameTileArgs({ x: 0, y: 0, z: 0 }), (e) => e.code === 'invalid_args');
  assert.throws(() => parseFrameTileArgs({ dimension: 'enoch', zoom: 1.5, x: 0, y: 0 }), (e) => e.code === 'invalid_args');
});

test('manifest: every mod action and event, plus the map once surveyed', () => {
  const names = buildManifest(null).map((c) => c.name);
  assert.ok(names.includes('players.location') && names.includes('events.player-death'));
  assert.ok(!names.includes('map'));
  assert.equal(names.length, ACTION_CAPABILITIES.length + 6 + 1);
  const dim = { id: 'chernarusplus' };
  const map = buildManifest(dim).find((c) => c.name === 'map');
  assert.deepEqual(map, { name: 'map', parameters: { dimensions: [dim] } });
});

test('positions carry the Dimension; an offline player (0, 0, 0) is unknown', () => {
  assert.deepEqual(withDimension('getPlayerLocation', { x: 6700.5, y: 9.2, z: 2600 }, 'chernarusplus'), { x: 6700.5, y: 9.2, z: 2600, dimension: 'chernarusplus' });
  assert.equal(withDimension('getPlayerLocation', { x: 0, y: 0, z: 0 }, 'chernarusplus'), null);
  assert.deepEqual(
    withDimension('listLocations', [{ code: 'Balota', name: 'Balota', position: { x: 4500, y: 0, z: 2400 } }], 'chernarusplus'),
    [{ code: 'Balota', name: 'Balota', position: { x: 4500, y: 0, z: 2400, dimension: 'chernarusplus' } }],
  );
  assert.deepEqual(withDimension('getPlayers', [{ gameId: '1' }], 'chernarusplus'), [{ gameId: '1' }]);
  assert.deepEqual(eventWithDimension({ msg: 'x', position: { x: 1, y: 2, z: 3 } }, 'enoch'), { msg: 'x', position: { x: 1, y: 2, z: 3, dimension: 'enoch' } });
  assert.deepEqual(eventWithDimension({ msg: 'x' }, 'enoch'), { msg: 'x' });
});

// A fake Takaro: answers identify with the given protocol version and records frames.
function fakeTakaro(version) {
  const wss = new WebSocketServer({ port: 0 });
  const frames = [];
  const sockets = [];
  wss.on('connection', (sock) => {
    sockets.push(sock);
    sock.on('message', (raw) => {
      const msg = JSON.parse(raw.toString());
      frames.push(msg);
      if (msg.type === 'identify') {
        sock.send(JSON.stringify({ type: 'identifyResponse', payload: { gameServerId: 'gs-1', protocolVersion: version } }));
      }
    });
  });
  const url = `ws://127.0.0.1:${wss.address().port}`;
  return { wss, frames, sockets, url };
}

const waitFor = async (pred, ms = 3000) => {
  const end = Date.now() + ms;
  while (!pred()) {
    if (Date.now() > end) throw new Error('timed out');
    await new Promise((r) => setTimeout(r, 10));
  }
};

test('v1 connection: manifest on identify, numbered events resent until acknowledged, {code, message} errors', async () => {
  const takaro = fakeTakaro(1);
  let dimension = null;
  const client = new TakaroWsClient(takaro.url, { identityToken: 'i', registrationToken: 'r', name: 'n' }, {
    offerV1: true,
    manifest: () => buildManifest(dimension),
  });
  client.reconnectMs = 20;
  client.connect();
  await waitFor(() => client.identified());
  const identify = takaro.frames.find((f) => f.type === 'identify');
  assert.deepEqual(identify.payload.protocolVersions, [0, 1]);
  assert.ok(!identify.payload.capabilities.some((c) => c.name === 'map'));
  assert.equal(client.getProtocolVersion(), 1);

  // The survey finishes after identify: one capabilities replacement, then nothing while unchanged.
  dimension = { id: 'chernarusplus' };
  assert.equal(client.refreshCapabilities(), true);
  assert.equal(client.refreshCapabilities(), false);
  await waitFor(() => takaro.frames.some((f) => f.type === 'capabilities'));

  client.sendGameEvent('chat-message', { msg: 'a' });
  client.sendGameEvent('chat-message', { msg: 'b' });
  await waitFor(() => takaro.frames.filter((f) => f.type === 'gameEvent').length === 2);
  const [e1, e2] = takaro.frames.filter((f) => f.type === 'gameEvent');
  assert.equal(e1.payload.seq, 1);
  assert.equal(e2.payload.seq, 2);
  assert.equal(e1.payload.streamId, e2.payload.streamId);
  assert.equal(client.unacknowledged(), 2);
  takaro.sockets[0].send(JSON.stringify({ type: 'eventAck', payload: { streamId: e1.payload.streamId, upTo: 1 } }));
  await waitFor(() => client.unacknowledged() === 1);

  // Reconnect: the unacknowledged event goes again with the same stream and seq.
  takaro.sockets[0].terminate();
  await waitFor(() => takaro.frames.filter((f) => f.type === 'identify').length === 2 && client.identified());
  await waitFor(() => takaro.frames.filter((f) => f.type === 'gameEvent').length === 3);
  const resent = takaro.frames.filter((f) => f.type === 'gameEvent')[2];
  assert.deepEqual([resent.payload.seq, resent.payload.streamId, resent.payload.data.msg], [2, e1.payload.streamId, 'b']);

  client.sendError('req-1', 'no such tile', 'not_found');
  await waitFor(() => takaro.frames.some((f) => f.requestId === 'req-1'));
  assert.deepEqual(takaro.frames.find((f) => f.requestId === 'req-1'), { type: 'response', requestId: 'req-1', error: { code: 'not_found', message: 'no such tile' } });
  client.shutdown();
  takaro.wss.close();
});

test('v0 connection: plain events and error frames, nothing buffered', async () => {
  const takaro = fakeTakaro(0);
  const client = new TakaroWsClient(takaro.url, { identityToken: 'i', registrationToken: 'r', name: 'n' }, { offerV1: true, manifest: () => [] });
  client.connect();
  await waitFor(() => client.identified());
  assert.equal(client.getProtocolVersion(), 0);
  assert.equal(client.refreshCapabilities(), false);
  client.sendGameEvent('log', { msg: 'x' });
  client.sendError('req-2', 'boom');
  await waitFor(() => takaro.frames.some((f) => f.requestId === 'req-2'));
  assert.deepEqual(takaro.frames.find((f) => f.type === 'gameEvent').payload, { type: 'log', data: { msg: 'x' } });
  assert.deepEqual(takaro.frames.find((f) => f.requestId === 'req-2'), { type: 'error', requestId: 'req-2', payload: { message: 'boom' } });
  assert.equal(client.unacknowledged(), 0);
  client.shutdown();
  takaro.wss.close();
});

test('TAKARO_PROTOCOL=0 sends the old identify', async () => {
  const takaro = fakeTakaro(0);
  const client = new TakaroWsClient(takaro.url, { identityToken: 'i', registrationToken: 'r', name: 'n' });
  client.connect();
  await waitFor(() => client.identified());
  const identify = takaro.frames.find((f) => f.type === 'identify');
  assert.deepEqual(identify.payload, { identityToken: 'i', registrationToken: 'r', name: 'n' });
  client.shutdown();
  takaro.wss.close();
});

test('flat and nested player ids reach the mod as the same gameId', () => {
  const nested = JSON.parse(slimArgs(JSON.stringify({ player: { gameId: '7656', roles: ['x'] }, reason: 'r' })));
  assert.deepEqual(nested, { player: { gameId: '7656' }, reason: 'r' });
  const flat = JSON.parse(slimArgs(JSON.stringify({ gameId: '7656', playerId: 'p', gameServerId: 'g', roles: [] })));
  assert.deepEqual(flat, { gameId: '7656' });
});

test('args without a player pass through in every encoding', () => {
  assert.equal(slimArgs('[]'), '[]');
  assert.equal(slimArgs('{}'), '{}');
  assert.equal(slimArgs('{"message":"hi"}'), '{"message":"hi"}');
  assert.equal(slimArgs('not json'), 'not json');
});

test('health is ok only while Takaro is connected and the mod is polling', () => {
  const now = 100_000;
  assert.equal(bridgeHealthy(true, true, now - 1_000, now), true);
  assert.equal(bridgeHealthy(true, true, now - 60_000, now), false);
  assert.equal(bridgeHealthy(true, true, null, now), false);
  assert.equal(bridgeHealthy(false, true, now - 1_000, now), false);
  assert.equal(bridgeHealthy(true, false, now - 1_000, now), false);
});
