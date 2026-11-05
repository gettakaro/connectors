import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { readFileSync } from 'node:fs';
import { test } from 'node:test';
import { WebSocket, WebSocketServer } from 'ws';
import { buildManifest } from '../takaro/capabilities.js';
import { TakaroWsClient } from '../takaro/client.js';
import { ACTION_COVERAGE } from '../takaro/coverage.js';
import {
  CONNECTOR_IDENTITY,
  gameIcon,
  gameIconResponse,
  gameProfileCapability,
  normalizeGameVersion,
} from '../takaro/gameProfile.js';
import type { WsMessage } from '../takaro/protocol.js';
import { TerrariaAdapter, type TShockApi } from '../terraria/adapter.js';

const VERSION_TXT = readFileSync(new URL('../../../version.txt', import.meta.url), 'utf8').trim();
const ICON_PNG = readFileSync(new URL('../../../assets/icon-256.png', import.meta.url));
const SLUG = /^[a-z0-9][a-z0-9-]*$/;
const SEMVER = /^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(?:-((?:0|[1-9]\d*|\d*[a-zA-Z-][0-9a-zA-Z-]*)(?:\.(?:0|[1-9]\d*|\d*[a-zA-Z-][0-9a-zA-Z-]*))*))?(?:\+([0-9a-zA-Z-]+(?:\.[0-9a-zA-Z-]+)*))?$/;

test('the reported connector version is games/terraria/version.txt, strict semver', () => {
  assert.equal(CONNECTOR_IDENTITY.connector.version, VERSION_TXT);
  assert.match(CONNECTOR_IDENTITY.connector.version, SEMVER);
  assert.ok(CONNECTOR_IDENTITY.connector.version.length <= 64);
  assert.equal(CONNECTOR_IDENTITY.game, 'terraria');
  assert.equal(CONNECTOR_IDENTITY.connector.name, 'takaro-terraria');
  assert.match(CONNECTOR_IDENTITY.game, SLUG);
  assert.match(CONNECTOR_IDENTITY.connector.name, SLUG);
});

test('the shipped icon is a 256x256 PNG of at most 128 KiB, declared by the sha256 of its bytes', () => {
  assert.equal(ICON_PNG.subarray(0, 8).toString('hex'), '89504e470d0a1a0a');
  assert.equal(ICON_PNG.subarray(12, 16).toString('ascii'), 'IHDR');
  assert.equal(ICON_PNG.readUInt32BE(16), 256);
  assert.equal(ICON_PNG.readUInt32BE(20), 256);
  assert.ok(ICON_PNG.length <= 131072);

  const icon = gameIcon();
  assert.equal(icon.size, ICON_PNG.length);
  assert.equal(icon.revision, createHash('sha256').update(ICON_PNG).digest('hex').slice(0, 12));
  // Other bytes, another revision.
  assert.notEqual(gameIcon(Buffer.from('other').toString('base64')).revision, icon.revision);
});

test('the manifest declares game.profile with name, game build and icon', () => {
  const icon = gameIcon();
  const withVersion = buildManifest(null, '1.4.5.8').filter((entry) => entry.name === 'game.profile');
  assert.deepEqual(withVersion, [{
    name: 'game.profile',
    parameters: { name: 'Terraria', version: '1.4.5.8', icon: { revision: icon.revision, size: icon.size } },
  }]);
  // Before TShock answered, version is omitted rather than invented.
  assert.deepEqual(gameProfileCapability(null).parameters, { name: 'Terraria', icon: { revision: icon.revision, size: icon.size } });
  assert.ok(!buildManifest(null).some((entry) => entry.name === 'getGameIcon'), 'getGameIcon rides on game.profile');
  assert.equal(ACTION_COVERAGE.getGameIcon.status, 'live-supported');
});

test('the game build is TShock serverversion, trimmed to what Takaro accepts', () => {
  assert.equal(normalizeGameVersion('v1.4.5.8'), '1.4.5.8');
  assert.equal(normalizeGameVersion(' 1.4.4.9 '), '1.4.4.9');
  assert.equal(normalizeGameVersion('version'), 'version');
  assert.equal(normalizeGameVersion(''), null);
  assert.equal(normalizeGameVersion('x'.repeat(33)), null);
  assert.equal(normalizeGameVersion('1.4é'), null);
  assert.equal(normalizeGameVersion(undefined), null);
  assert.equal(normalizeGameVersion(1458), null);
});

class StatusOnlyTShock {
  serverversion: string | undefined = 'v1.4.5.8';
  async testToken() {
    return { success: true, rawResult: 'ok' };
  }
  async status() {
    return { name: 'Terraria', port: 7777, playercount: 0, serverversion: this.serverversion };
  }
}

test('getGameIcon answers the whole PNG of the declared revision; the adapter records the game build', async () => {
  const tshock = new StatusOnlyTShock();
  const reported: string[] = [];
  const adapter = new TerrariaAdapter(tshock as unknown as TShockApi, {
    commandAllowlistExact: [], commandAllowlistPrefixes: [], enableShutdown: false,
  }, (version) => reported.push(version));

  const response = await adapter.handleAction('getGameIcon', '{}') as { revision: string; data: string };
  assert.deepEqual(Object.keys(response).sort(), ['data', 'revision']);
  assert.ok(Buffer.from(response.data, 'base64').equals(ICON_PNG), 'data is exactly the shipped file');
  const declared = (gameProfileCapability(null).parameters as { icon: { revision: string; size: number } }).icon;
  assert.equal(response.revision, declared.revision);
  assert.equal(Buffer.from(response.data, 'base64').length, declared.size);
  assert.deepEqual(gameIconResponse(), response);

  assert.equal(adapter.gameVersion(), null);
  await adapter.handleAction('testReachability', {});
  await adapter.handleAction('testReachability', {});
  assert.equal(adapter.gameVersion(), '1.4.5.8');
  assert.deepEqual(reported, ['1.4.5.8'], 'reported once per change');
  tshock.serverversion = 'v1.4.5.9';
  await adapter.handleAction('testReachability', {});
  assert.deepEqual(reported, ['1.4.5.8', '1.4.5.9']);
  tshock.serverversion = undefined;
  await adapter.handleAction('testReachability', {});
  assert.equal(adapter.gameVersion(), '1.4.5.9', 'a status without a build keeps the last one');
});

for (const offerV1 of [true, false]) {
  test(`identify sends game and connector on protocol ${offerV1 ? '1 with game.profile' : '0 without a manifest'}`, async () => {
    const server = new WebSocketServer({ port: 0, host: '127.0.0.1' });
    await new Promise<void>((resolve) => server.once('listening', resolve));
    const address = server.address();
    if (!address || typeof address === 'string') throw new Error('server did not bind');
    const identifies: Array<Record<string, unknown>> = [];
    server.on('connection', (socket) => {
      socket.on('message', (raw) => {
        const message = JSON.parse(raw.toString()) as WsMessage;
        if (message.type !== 'identify') return;
        identifies.push(message.payload as Record<string, unknown>);
        socket.send(JSON.stringify({ type: 'identifyResponse', payload: { gameServerId: 'gs', protocolVersion: offerV1 ? 1 : 0 } }));
      });
    });
    const client = new TakaroWsClient(`ws://127.0.0.1:${address.port}`, {
      identityToken: 'identity', registrationToken: 'registration', name: 'Terraria Test',
    }, 10, 20, { offerV1, manifest: () => buildManifest(null, '1.4.5.8') });
    try {
      client.connect();
      await waitFor(() => client.identified());
      const identify = identifies[0];
      assert.equal(identify.game, 'terraria');
      assert.deepEqual(identify.connector, { name: 'takaro-terraria', version: VERSION_TXT });
      assert.equal(identify.name, 'Terraria Test');
      if (offerV1) {
        assert.deepEqual(identify.protocolVersions, [0, 1]);
        const profile = (identify.capabilities as Array<{ name: string; parameters?: unknown }>).find((c) => c.name === 'game.profile');
        assert.deepEqual(profile, gameProfileCapability('1.4.5.8'));
      } else {
        assert.equal(identify.protocolVersions, undefined);
        assert.equal(identify.capabilities, undefined);
      }
    } finally {
      client.shutdown();
      for (const socket of server.clients) {
        if (socket.readyState === WebSocket.OPEN || socket.readyState === WebSocket.CLOSING) socket.terminate();
      }
      await new Promise<void>((resolve) => server.close(() => resolve()));
    }
  });
}

async function waitFor(predicate: () => boolean, timeoutMs = 3000): Promise<void> {
  const deadline = Date.now() + timeoutMs;
  while (!predicate()) {
    if (Date.now() > deadline) throw new Error('timed out');
    await new Promise((resolve) => setTimeout(resolve, 10));
  }
}
