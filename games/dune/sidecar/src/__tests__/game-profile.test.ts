import { createHash } from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { describe, expect, it } from 'vitest';
import { Bridge } from '../bridge.js';
import { MAP_DIMENSIONS } from '../dune/mapDimensions.js';
import { buildManifest } from '../takaro/capabilities.js';
import {
  CONNECTOR_IDENTITY,
  GAME_ICON,
  GameBuild,
  cleanGameVersion,
  gameIconPayload,
  gameProfileCapability,
  parseGameBuild,
} from '../takaro/gameProfile.js';
import { createIdentify, type WsMessage } from '../takaro/protocol.js';
import { CONNECTOR_VERSION } from '../version.js';
import { harness } from './helpers.js';

const GAME_DIR = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..', '..');
const ICON_FILE = path.join(GAME_DIR, 'assets', 'icon-256.png');
const BOOT_LINE = '[2026.10.09-15.18.35:018][  0][55]LogNetVersion: Log: DuneSandbox 1.5.3.7, NetCL: 2147284, EngineNetworkVersion: 32';

describe('connector identity', () => {
  it('reports the release version from version.txt as strict semver', () => {
    const released = fs.readFileSync(path.join(GAME_DIR, 'version.txt'), 'utf8').trim();
    expect(CONNECTOR_VERSION).toBe(released);
    expect(CONNECTOR_IDENTITY).toEqual({ name: 'takaro-dune', version: released });
    expect(CONNECTOR_VERSION).toMatch(/^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(?:-[0-9A-Za-z.-]+)?(?:\+[0-9A-Za-z.-]+)?$/);
  });

  it('identify carries game and connector on both versions', () => {
    const v0 = createIdentify({ identityToken: 'id', registrationToken: 'reg' }).payload as Record<string, unknown>;
    expect(v0).toEqual({ identityToken: 'id', registrationToken: 'reg', game: 'dune', connector: CONNECTOR_IDENTITY });
    const v1 = createIdentify({ identityToken: 'id', registrationToken: '' }, { protocolVersions: [0, 1], capabilities: [] }).payload as Record<
      string,
      unknown
    >;
    expect(v1).toMatchObject({ game: 'dune', connector: CONNECTOR_IDENTITY, protocolVersions: [0, 1] });
    expect(v1.game).toMatch(/^[a-z0-9][a-z0-9-]*$/);
    expect((v1.connector as { name: string }).name).toMatch(/^[a-z0-9][a-z0-9-]*$/);
  });
});

describe('game icon', () => {
  it('is the committed 256 x 256 PNG of at most 128 KiB, with revision and size taken from its bytes', () => {
    const file = fs.readFileSync(ICON_FILE);
    expect(GAME_ICON.bytes.equals(file)).toBe(true); // run `npm run icon` after changing the PNG
    expect(file.subarray(0, 8).toString('hex')).toBe('89504e470d0a1a0a');
    expect(file.subarray(12, 16).toString('ascii')).toBe('IHDR');
    expect([file.readUInt32BE(16), file.readUInt32BE(20)]).toEqual([256, 256]);
    expect(GAME_ICON.size).toBe(file.length);
    expect(GAME_ICON.size).toBeLessThanOrEqual(131072);
    expect(GAME_ICON.revision).toBe(createHash('sha256').update(file).digest('hex').slice(0, 12));
  });

  it('getGameIcon answers the declared revision with the whole file', async () => {
    const h = await harness();
    const sent: WsMessage[] = [];
    const bridge = new Bridge({ adapter: h.adapter, takaro: { send: (m) => (sent.push(m), true), sendGameEvent: () => true } });
    await bridge.handleRequest({ type: 'request', requestId: 'icon-1', payload: { action: 'getGameIcon', args: '{}' } });
    expect(sent[0]).toEqual({ type: 'response', requestId: 'icon-1', payload: gameIconPayload() });
    const payload = sent[0].payload as { revision: string; data: string };
    const declared = (gameProfileCapability(null).parameters as { icon: { revision: string; size: number } }).icon;
    expect(payload.revision).toBe(declared.revision);
    expect(Buffer.from(payload.data, 'base64').length).toBe(declared.size);
  });
});

describe('game.profile capability', () => {
  it('is in the manifest with the display name, the icon and the game build when known', () => {
    const unknown = buildManifest(MAP_DIMENSIONS).find((c) => c.name === 'game.profile');
    expect(unknown).toEqual({
      name: 'game.profile',
      parameters: { name: 'Dune: Awakening', icon: { revision: GAME_ICON.revision, size: GAME_ICON.size } },
    });
    const known = buildManifest(MAP_DIMENSIONS, '1.5.3.7').find((c) => c.name === 'game.profile');
    expect(known?.parameters).toMatchObject({ name: 'Dune: Awakening', version: '1.5.3.7' });
    expect(buildManifest([]).filter((c) => c.name === 'game.profile')).toHaveLength(1);
  });

  it('keeps the name and version inside Takaro limits', () => {
    expect([...'Dune: Awakening'].length).toBeLessThanOrEqual(64);
    expect(cleanGameVersion(' 1.5.3.7 ')).toBe('1.5.3.7');
    expect(cleanGameVersion('x'.repeat(33))).toBeNull();
    expect(cleanGameVersion('1.5\u0007')).toBeNull();
    expect((gameProfileCapability('é').parameters as Record<string, unknown>).version).toBeUndefined();
  });
});

describe('game build', () => {
  it('reads the build from the map server LogNetVersion line', () => {
    expect(parseGameBuild(BOOT_LINE)).toBe('1.5.3.7');
    expect(parseGameBuild('LogNetVersion: Log: Set ProjectVersion to 1.5.3.7. Version Checksum will be recalculated')).toBeNull();
    expect(parseGameBuild('LogInit: Log: Build: 1.5.3.0')).toBeNull();
  });

  it('scans the boot lines of an existing log and follows a map restart', () => {
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'dune-build-'));
    const file = path.join(dir, 'DuneSandbox_PIDX-1.log');
    fs.writeFileSync(file, `LogInit: Log: Build: 1.5.3.0\n${BOOT_LINE}\nLogWorld: Bringing World up\n`);
    const build = new GameBuild();
    build.scanFile(file);
    expect(build.version()).toBe('1.5.3.7');
    expect(build.observe(BOOT_LINE)).toBe(false);
    expect(build.observe('LogNetVersion: Log: DuneSandbox 1.5.4.0, NetCL: 2150000, EngineNetworkVersion: 32')).toBe(true);
    expect(build.version()).toBe('1.5.4.0');

    const missing = new GameBuild();
    missing.scanFile(path.join(dir, 'absent.log'));
    expect(missing.version()).toBeNull();
    fs.rmSync(dir, { recursive: true, force: true });
  });
});
