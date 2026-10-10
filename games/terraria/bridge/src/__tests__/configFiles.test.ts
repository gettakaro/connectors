import assert from 'node:assert/strict';
import { chmodSync, cpSync, existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, statSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { test } from 'node:test';
import { parseKeyValues } from '../config.js';
import { locateConfig, resolveFiles, setKeys } from '../configFiles.js';
import { ConfigSource } from '../configSource.js';

const SHIPPED = readFileSync(new URL('../../TakaroConfig.example.txt', import.meta.url), 'utf8');
const quiet = { info: () => undefined, warn: () => undefined, error: () => undefined };

/** A bridge folder laid out the way the release zip unpacks: <parent>/TakaroTerrariaBridge/. */
function install(parent = mkdtempSync(path.join(tmpdir(), 'terraria-install-'))) {
  const root = path.join(parent, 'TakaroTerrariaBridge');
  mkdirSync(path.join(root, 'dist'), { recursive: true });
  writeFileSync(path.join(root, 'TakaroConfig.txt'), SHIPPED);
  return { parent, root, userPath: path.join(root, 'TakaroConfig.txt') };
}

function sourceFor(root: string, env: NodeJS.ProcessEnv = {}, ids = ['11111111-1111-4111-8111-111111111111']) {
  const queue = [...ids];
  return new ConfigSource(locateConfig(env, root, root), env, quiet, () => queue.shift() ?? 'exhausted', 0);
}

function edit(file: string, writes: Record<string, string>): void {
  writeFileSync(file, setKeys(readFileSync(file, 'utf8'), writes));
}

test('the shipped file sits inside the bridge folder and its saved copy beside it', () => {
  const { parent, root, userPath } = install();
  const location = locateConfig({}, root, root);
  assert.equal(location.userPath, userPath);
  assert.equal(location.savedPath, path.join(parent, 'TakaroTerrariaBridge.saved-config.txt'));
});

test('BRIDGE_CONFIG and a TakaroConfig.txt in the working directory still win, without a saved copy', () => {
  const { parent, root } = install();
  const outside = path.join(parent, 'elsewhere.txt');
  assert.deepEqual(locateConfig({ BRIDGE_CONFIG: outside }, root, root), { userPath: outside, savedPath: null });
  const cwd = path.join(parent, 'run');
  mkdirSync(cwd);
  writeFileSync(path.join(cwd, 'TakaroConfig.txt'), 'serverName=x\n');
  assert.deepEqual(locateConfig({}, cwd, root), { userPath: path.join(cwd, 'TakaroConfig.txt'), savedPath: null });
});

test('fresh install: no token means no connection, a token saved later connects with a generated identity', async () => {
  const { parent, root, userPath } = install();
  const source = sourceFor(root);

  const first = await source.poll();
  assert.ok(first);
  assert.equal(first.config.registrationToken, '');
  assert.equal(first.identitySource, 'generated');
  assert.equal(first.config.identityToken, '11111111-1111-4111-8111-111111111111');
  assert.equal(parseKeyValues(readFileSync(userPath, 'utf8')).identityToken, first.config.identityToken);
  assert.equal(await source.poll(), null, 'our own write is not a change');

  edit(userPath, { registrationToken: 'rt-1' });
  const second = await source.poll();
  assert.ok(second);
  assert.equal(second.config.registrationToken, 'rt-1');
  assert.equal(second.config.identityToken, first.config.identityToken);
  assert.equal(second.identitySource, 'config');

  const saved = readFileSync(path.join(parent, 'TakaroTerrariaBridge.saved-config.txt'), 'utf8');
  assert.equal(parseKeyValues(saved).registrationToken, 'rt-1');
  assert.equal(parseKeyValues(saved).identityToken, first.config.identityToken);
  assert.equal(statSync(path.join(parent, 'TakaroTerrariaBridge.saved-config.txt')).mode & 0o777, 0o600);
  // The comments people read stay in the file, and its permissions with it.
  chmodSync(userPath, 0o600);
  edit(userPath, { serverName: 'Renamed' });
  await source.poll();
  assert.equal(statSync(userPath).mode & 0o777, 0o600);
  assert.match(readFileSync(userPath, 'utf8'), /Leave identityToken empty/);
});

test('upgrade by replacing the bridge folder keeps the token, the identity and the TShock settings', async () => {
  const { parent, root, userPath } = install();
  edit(userPath, { registrationToken: 'rt-1', tshockToken: 'rest-1', pollIntervalMs: '2000', httpPort: '3020' });
  const before = await sourceFor(root).poll();
  assert.ok(before);

  // README step: delete TakaroTerrariaBridge/, unzip the new one in its place.
  rmSync(root, { recursive: true });
  install(parent);
  const after = await sourceFor(root, {}, ['must-not-be-used']).poll();
  assert.ok(after);
  assert.equal(after.identitySource, 'saved');
  assert.equal(after.config.identityToken, before.config.identityToken);
  assert.equal(after.config.registrationToken, 'rt-1');
  assert.equal(after.config.tshock.token, 'rest-1');
  assert.equal(after.config.pollIntervalMs, 2000);

  const user = parseKeyValues(readFileSync(userPath, 'utf8'));
  assert.equal(user.identityToken, before.config.identityToken);
  assert.equal(user.tshockToken, 'rest-1');
  assert.equal(user.registrationToken, '', 'the registration token is never copied into the user file');
});

test('after an upgrade the user file is authoritative again: a setting put back to its default sticks', async () => {
  const { parent, root, userPath } = install();
  edit(userPath, { registrationToken: 'rt-1', pollIntervalMs: '2000' });
  await sourceFor(root).poll();
  rmSync(root, { recursive: true });
  install(parent);
  const source = sourceFor(root);
  assert.equal((await source.poll())?.config.pollIntervalMs, 2000);
  edit(userPath, { pollIntervalMs: '10000' });
  assert.equal((await source.poll())?.config.pollIntervalMs, 10000);
});

test('a token in the user file wins over the saved one; a changed token is saved', async () => {
  const { parent, root, userPath } = install();
  edit(userPath, { registrationToken: 'rt-old' });
  const source = sourceFor(root);
  await source.poll();
  edit(userPath, { registrationToken: 'rt-new' });
  assert.equal((await source.poll())?.config.registrationToken, 'rt-new');
  const saved = parseKeyValues(readFileSync(path.join(parent, 'TakaroTerrariaBridge.saved-config.txt'), 'utf8'));
  assert.equal(saved.registrationToken, 'rt-new');
});

test('an empty or half-written file keeps the settings in use', async () => {
  const { root, userPath } = install();
  const source = new ConfigSource(locateConfig({}, root, root), {}, quiet, () => 'id-1', 50);
  edit(userPath, { registrationToken: 'rt-1' });
  assert.ok(await source.poll());

  writeFileSync(userPath, '');
  assert.equal(await source.poll(), null);

  // A writer still busy: the text differs between the two reads, so nothing is taken.
  const full = setKeys(SHIPPED, { registrationToken: 'rt-2', identityToken: 'id-1' });
  writeFileSync(userPath, full.slice(0, 40));
  const pending = source.poll();
  setTimeout(() => writeFileSync(userPath, full), 10);
  assert.equal(await pending, null);
  assert.equal((await source.poll())?.config.registrationToken, 'rt-2');
});

test('a missing file is waited for, not fatal', async () => {
  const { root, userPath } = install();
  rmSync(userPath);
  const source = sourceFor(root);
  assert.equal(await source.poll(), null);
  writeFileSync(userPath, SHIPPED);
  assert.ok(await source.poll());
});

test('existing installs keep the identity they run on', () => {
  // 0.2.0 template: identityToken=terraria-local.
  assert.equal(resolveFiles('registrationToken=rt\nidentityToken=terraria-local\nserverName=S\n', null).values.identityToken, 'terraria-local');
  // Hand-written 0.2.0 config without the key, or with it empty: the bridge identified as serverName.
  const absent = resolveFiles('registrationToken=rt\nserverName=My Server\n', null, () => 'uuid');
  assert.equal(absent.identitySource, 'serverName');
  assert.equal(absent.values.identityToken, 'My Server');
  assert.equal(absent.userWrites.identityToken, 'My Server');
  assert.equal(resolveFiles('identityToken=\nserverName=My Server\n', null, () => 'uuid').values.identityToken, 'My Server');
  assert.equal(resolveFiles('serverName=\n', null, () => 'uuid').values.identityToken, 'Terraria Server');
  // A current-format file with no identity anywhere is a fresh install.
  assert.equal(resolveFiles('configFormat=2\nidentityToken=\n', null, () => 'uuid').values.identityToken, 'uuid');
});

test('a user file the bridge cannot write: the saved copy keeps the identity', async () => {
  if (process.getuid?.() === 0) return; // root writes through the mode bits
  const { root, userPath } = install();
  chmodSync(root, 0o555); // no rename into the folder, so the user file cannot be replaced
  try {
    const first = await sourceFor(root, {}, ['id-a', 'id-b']).poll();
    assert.equal(first?.config.identityToken, 'id-a');
    assert.equal(first?.identityNotSaved, undefined);
    assert.equal(parseKeyValues(readFileSync(userPath, 'utf8')).identityToken, '');
    // A restart reads the identity back from the saved copy.
    const again = await sourceFor(root, {}, ['id-c']).poll();
    assert.equal(again?.config.identityToken, 'id-a');
  } finally {
    chmodSync(root, 0o755);
  }
});

test('an identity that cannot be stored anywhere blocks the connection instead of orphaning a server', async () => {
  if (process.getuid?.() === 0) return;
  const { parent } = install();
  const dir = path.join(parent, 'ro');
  mkdirSync(dir);
  const outside = path.join(dir, 'TakaroConfig.txt');
  writeFileSync(outside, setKeys(SHIPPED, { registrationToken: 'rt' }));
  chmodSync(dir, 0o555);
  chmodSync(outside, 0o444);
  try {
    const queue = ['id-c', 'id-d'];
    const source = new ConfigSource(locateConfig({ BRIDGE_CONFIG: outside }, parent, path.join(parent, 'TakaroTerrariaBridge')), {}, quiet, () => queue.shift()!, 0);
    const loaded = await source.poll();
    assert.equal(loaded?.config.registrationToken, '');
    assert.match(loaded?.identityNotSaved ?? '', /identityToken=id-c/);
  } finally {
    chmodSync(dir, 0o755);
  }
});

test('emptying registrationToken while running disconnects; the saved token does not come back', async () => {
  const { root, userPath } = install();
  edit(userPath, { registrationToken: 'rt-1' });
  const source = sourceFor(root);
  assert.equal((await source.poll())?.config.registrationToken, 'rt-1');
  edit(userPath, { registrationToken: '' });
  assert.equal((await source.poll())?.config.registrationToken, '');
});

test('an allowlist the operator emptied stays empty after an upgrade', async () => {
  const { parent, root, userPath } = install();
  edit(userPath, { registrationToken: 'rt-1', commandAllowlistExact: '', commandAllowlistPrefixes: '' });
  await sourceFor(root).poll();
  rmSync(root, { recursive: true });
  install(parent);
  const after = await sourceFor(root).poll();
  assert.deepEqual(after?.config.commandAllowlistExact, []);
  assert.deepEqual(after?.config.commandAllowlistPrefixes, []);
});

test('a trimmed user file does not drop settings from the saved copy', async () => {
  const { parent, root, userPath } = install();
  edit(userPath, { registrationToken: 'rt-1', tshockToken: 'rest-1' });
  const source = sourceFor(root);
  const first = await source.poll();
  writeFileSync(userPath, `configFormat=2\nregistrationToken=rt-1\nidentityToken=${first?.config.identityToken}\n`);
  await source.poll();
  const saved = parseKeyValues(readFileSync(path.join(parent, 'TakaroTerrariaBridge.saved-config.txt'), 'utf8'));
  assert.equal(saved.tshockToken, 'rest-1');
});

test('writing the identity drops group and other access to the token file', async () => {
  const { root, userPath } = install();
  chmodSync(userPath, 0o644);
  await sourceFor(root).poll();
  assert.equal(statSync(userPath).mode & 0o777, 0o600);
});

test('environment variables still override the file, and are never written to disk', async () => {
  const { parent, root } = install();
  const env = { TAKARO_REGISTRATION_TOKEN: 'from-env', TSHOCK_TOKEN: 'rest-env', TSHOCK_USERNAME: 'u', TSHOCK_PASSWORD: 'p' };
  const loaded = await sourceFor(root, env).poll();
  assert.equal(loaded?.config.registrationToken, 'from-env');
  assert.equal(loaded?.config.tshock.token, 'rest-env');
  assert.equal(loaded?.config.tshock.username, 'u');
  const saved = readFileSync(path.join(parent, 'TakaroTerrariaBridge.saved-config.txt'), 'utf8');
  assert.doesNotMatch(saved, /from-env|rest-env/);
});

test('BRIDGE_CONFIG outside the bridge folder (docker, the dev rig) writes no saved copy', async () => {
  const { parent, root } = install();
  const outside = path.join(parent, 'config', 'TakaroConfig.txt');
  mkdirSync(path.dirname(outside));
  cpSync(path.join(root, 'TakaroConfig.txt'), outside);
  const loaded = await new ConfigSource(locateConfig({ BRIDGE_CONFIG: outside }, root, root), {}, quiet, () => 'id', 0).poll();
  assert.equal(loaded?.config.identityToken, 'id');
  assert.equal(existsSync(path.join(parent, 'TakaroTerrariaBridge.saved-config.txt')), false);
});

test('setKeys replaces the first line for a key, keeps comments and CRLF, and appends missing keys', () => {
  assert.equal(setKeys('# c\r\na=1\r\nb=2\r\n', { b: '3', c: '4' }), '# c\r\na=1\r\nb=3\r\nc=4\r\n');
  assert.equal(setKeys('# identityToken=x\nidentityToken=\n', { identityToken: 'id' }), '# identityToken=x\nidentityToken=id\n');
  // The last line for a key is the one parseKeyValues reads, so it is the one replaced.
  assert.equal(setKeys('identityToken=a\nidentityToken=\n', { identityToken: 'id' }), 'identityToken=a\nidentityToken=id\n');
});
