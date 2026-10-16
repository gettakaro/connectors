import assert from 'node:assert/strict';
import { chmodSync, existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, statSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { test } from 'node:test';
import { parseKeyValues } from '../config.js';
import { ConfigSource, configTemplate, locateConfig, setKeys } from '../configFile.js';

const ID = '0123abcd-1111-4111-8111-111111111111';
const quiet = { info: () => undefined, warn: () => undefined };

/** A bridge folder laid out the way the release zip unpacks: <parent>/TakaroTerrariaBridge/. */
function install() {
  const parent = mkdtempSync(path.join(tmpdir(), 'terraria-install-'));
  const root = path.join(parent, 'TakaroTerrariaBridge');
  mkdirSync(path.join(root, 'dist'), { recursive: true });
  return { parent, root, file: path.join(parent, 'TakaroConfig.txt') };
}

function sourceFor(file: string, env: NodeJS.ProcessEnv = {}, ids = [ID], settleMs = 0) {
  const queue = [...ids];
  return new ConfigSource(file, env, quiet, () => queue.shift() ?? 'exhausted', settleMs);
}

function edit(file: string, writes: Record<string, string>): void {
  writeFileSync(file, setKeys(readFileSync(file, 'utf8'), writes));
}

test('the config sits beside the bridge folder, or wherever BRIDGE_CONFIG points', () => {
  const { parent, root, file } = install();
  assert.equal(locateConfig({}, root, root), file);
  assert.equal(locateConfig({ BRIDGE_CONFIG: 'conf/T.txt' }, parent, root), path.join(parent, 'conf', 'T.txt'));
});

test('fresh install: the bridge creates the file with a UUID and a unique name, and does not connect', async () => {
  const { file } = install();
  const first = await sourceFor(file).poll();
  assert.ok(first);
  assert.equal(first.config.registrationToken, '');
  assert.equal(first.config.identityToken, ID);
  assert.equal(first.config.serverName, 'Terraria Server (0123abcd)');
  const values = parseKeyValues(readFileSync(file, 'utf8'));
  assert.equal(values.identityToken, ID);
  assert.equal(values.serverName, 'Terraria Server (0123abcd)');
  assert.equal(values.registrationToken, '');
  assert.equal(statSync(file).mode & 0o777, 0o600);
});

test('a token pasted while running connects with the same identity; our own writes are no change', async () => {
  const { file } = install();
  const source = sourceFor(file);
  assert.ok(await source.poll());
  assert.equal(await source.poll(), null);
  edit(file, { registrationToken: 'rt-1' });
  const next = await source.poll();
  assert.equal(next?.config.registrationToken, 'rt-1');
  assert.equal(next?.config.identityToken, ID);
});

test('emptying registrationToken while running disconnects', async () => {
  const { file } = install();
  const source = sourceFor(file);
  await source.poll();
  edit(file, { registrationToken: 'rt-1' });
  assert.equal((await source.poll())?.config.registrationToken, 'rt-1');
  edit(file, { registrationToken: '' });
  assert.equal((await source.poll())?.config.registrationToken, '');
});

test('a file deleted while running is waited for, never recreated with a new identity', async () => {
  const { file } = install();
  const source = sourceFor(file, {}, [ID, 'second-id']);
  assert.ok(await source.poll());
  const text = readFileSync(file, 'utf8');
  rmSync(file);
  assert.equal(await source.poll(), null);
  assert.equal(existsSync(file), false);
  writeFileSync(file, setKeys(text, { registrationToken: 'rt' }));
  assert.equal((await source.poll())?.config.identityToken, ID);
});

test('an empty or half-written file keeps the settings in use', async () => {
  const { file } = install();
  const source = sourceFor(file, {}, [ID], 50);
  assert.ok(await source.poll());
  const full = setKeys(readFileSync(file, 'utf8'), { registrationToken: 'rt-2' });

  writeFileSync(file, '');
  assert.equal(await source.poll(), null);

  // A writer still busy: the text differs between the two reads, so nothing is taken.
  writeFileSync(file, full.slice(0, 40));
  const pending = source.poll();
  setTimeout(() => writeFileSync(file, full), 10);
  assert.equal(await pending, null);
  assert.equal((await source.poll())?.config.registrationToken, 'rt-2');
});

test('a file the user wrote without an identity gets one written back, keeping comments and mode', async () => {
  const { file } = install();
  writeFileSync(file, '# mine\r\nregistrationToken=rt\r\nidentityToken=\r\nserverName=\r\n');
  chmodSync(file, 0o640);
  const loaded = await sourceFor(file).poll();
  assert.equal(loaded?.config.identityToken, ID);
  assert.equal(loaded?.config.registrationToken, 'rt');
  assert.equal(readFileSync(file, 'utf8'), `# mine\r\nregistrationToken=rt\r\nidentityToken=${ID}\r\nserverName=\r\n`);
  assert.equal(statSync(file).mode & 0o777, 0o640);
});

test('an identity that cannot be written blocks the connection instead of orphaning a server', async () => {
  if (process.getuid?.() === 0) return; // root writes through the mode bits
  const { parent } = install();
  const dir = path.join(parent, 'ro');
  mkdirSync(dir);
  const file = path.join(dir, 'TakaroConfig.txt');
  writeFileSync(file, 'registrationToken=rt\nidentityToken=\n');
  chmodSync(file, 0o444);
  chmodSync(dir, 0o555);
  try {
    const loaded = await sourceFor(file).poll();
    assert.equal(loaded?.config.registrationToken, '');
    assert.match(loaded?.identityNotSaved ?? '', new RegExp(`identityToken=${ID}`));
  } finally {
    chmodSync(dir, 0o755);
  }
});

test('a file that cannot be created is waited for, not fatal', async () => {
  if (process.getuid?.() === 0) return;
  const { parent } = install();
  const dir = path.join(parent, 'ro');
  mkdirSync(dir, { mode: 0o555 });
  const file = path.join(dir, 'TakaroConfig.txt');
  try {
    const source = sourceFor(file);
    assert.equal(await source.poll(), null);
    chmodSync(dir, 0o755);
    writeFileSync(file, `registrationToken=rt\nidentityToken=${ID}\n`);
    assert.equal((await source.poll())?.config.registrationToken, 'rt');
  } finally {
    chmodSync(dir, 0o755);
  }
});

test('environment variables override the file and are never written to it', async () => {
  const { file } = install();
  const env = { TAKARO_REGISTRATION_TOKEN: 'from-env', TSHOCK_TOKEN: 'rest-env', TSHOCK_USERNAME: 'u', TSHOCK_PASSWORD: 'p' };
  const loaded = await sourceFor(file, env).poll();
  assert.equal(loaded?.config.registrationToken, 'from-env');
  assert.equal(loaded?.config.tshock.token, 'rest-env');
  assert.equal(loaded?.config.tshock.username, 'u');
  assert.doesNotMatch(readFileSync(file, 'utf8'), /from-env|rest-env/);
});

test('a file with an identity (the dev rig, takaro-maint verify) is read as it is', async () => {
  const { file } = install();
  writeFileSync(file, 'registrationToken=rt\nidentityToken=takaro-dev-terraria\n');
  chmodSync(file, 0o400);
  const loaded = await sourceFor(file).poll();
  assert.equal(loaded?.config.identityToken, 'takaro-dev-terraria');
  assert.equal(loaded?.identityNotSaved, undefined);
  assert.equal(existsSync(`${file}.tmp-${process.pid}`), false);
});

test('relative mapDir and logFiles start at the config folder (the server folder), not the working directory', async () => {
  const { parent, file } = install();
  const first = await sourceFor(file).poll();
  assert.ok(first);
  // The plugin writes <server>/tshock/takaro; TakaroConfig.txt sits in <server> beside the bridge folder.
  assert.equal(first.config.mapDir, path.join(parent, 'tshock', 'takaro'));
  assert.deepEqual(first.config.logFiles, [path.join(parent, 'tshock', 'logs')]);
  edit(file, { mapDir: '/srv/terraria/tshock/takaro', logFiles: '/var/log/tshock/,logs/,logs/*.log' });
  const next = await sourceFor(file).poll();
  assert.ok(next);
  assert.equal(next.config.mapDir, '/srv/terraria/tshock/takaro');
  assert.deepEqual(next.config.logFiles, ['/var/log/tshock/', `${path.join(parent, 'logs')}${path.sep}`, path.join(parent, 'logs', '*.log')]);
});

test('the README shows the file the bridge creates', () => {
  const readme = readFileSync(new URL('../../../README.md', import.meta.url), 'utf8');
  assert.ok(readme.includes(`\`\`\`\n${configTemplate('', '')}\`\`\``), 'README config block differs from configTemplate');
});

test('setKeys replaces the last line for a key, keeps comments and CRLF, and appends missing keys', () => {
  assert.equal(setKeys('# c\r\na=1\r\nb=2\r\n', { b: '3', c: '4' }), '# c\r\na=1\r\nb=3\r\nc=4\r\n');
  assert.equal(setKeys('# identityToken=x\nidentityToken=\n', { identityToken: 'id' }), '# identityToken=x\nidentityToken=id\n');
  assert.equal(setKeys('identityToken=a\nidentityToken=\n', { identityToken: 'id' }), 'identityToken=a\nidentityToken=id\n');
});
