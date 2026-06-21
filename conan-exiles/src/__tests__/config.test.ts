import assert from 'node:assert/strict';
import { mkdtempSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { test } from 'node:test';
import { loadConfig } from '../config.js';

function withConfig(content: string): string {
  const dir = mkdtempSync(path.join(tmpdir(), 'conan-config-'));
  const file = path.join(dir, 'TakaroConfig.txt');
  writeFileSync(file, content);
  return file;
}

test('loads required config and applies defaults', () => {
  const file = withConfig(`
registrationToken=reg-token
serverName=Conan Test
rconHost=127.0.0.1
rconPort=25575
rconPassword=secret
`);

  const config = loadConfig(file);

  assert.equal(config.registrationToken, 'reg-token');
  assert.equal(config.identityToken, 'Conan Test');
  assert.equal(config.serverName, 'Conan Test');
  assert.equal(config.takaroWsUrl, 'wss://connect.takaro.io/');
  assert.equal(config.rcon.host, '127.0.0.1');
  assert.equal(config.rcon.port, 25575);
  assert.equal(config.rcon.password, 'secret');
  assert.equal(config.httpPort, 3010);
  assert.equal(config.databasePath, null);
  assert.equal(config.itemCatalogPath, null);
  assert.equal(config.pollIntervalMs, 10000);
  assert.equal(config.enableLogEvents, true);
  assert.deepEqual(config.logFiles, []);

  rmSync(path.dirname(file), { recursive: true, force: true });
});

test('parses optional values, comments, equals signs, booleans, and log file list', () => {
  const file = withConfig(`
# ignored
registrationToken=reg-token
identityToken=identity-token
serverName=Conan Test
takaroWsUrl=ws://localhost:8080
rconHost=192.168.1.5
rconPort=26000
rconPassword=secret=value
httpPort=4000
pollIntervalMs=5000
enableLogEvents=false
logFiles=/tmp/ConanSandbox.log, /tmp/RconCommandLog.log
databasePath=/tmp/game_0.db
itemCatalogPath=/tmp/conan-items.json
`);

  const config = loadConfig(file);

  assert.equal(config.identityToken, 'identity-token');
  assert.equal(config.takaroWsUrl, 'ws://localhost:8080');
  assert.equal(config.rcon.host, '192.168.1.5');
  assert.equal(config.rcon.port, 26000);
  assert.equal(config.rcon.password, 'secret=value');
  assert.equal(config.httpPort, 4000);
  assert.equal(config.pollIntervalMs, 5000);
  assert.equal(config.enableLogEvents, false);
  assert.deepEqual(config.logFiles, ['/tmp/ConanSandbox.log', '/tmp/RconCommandLog.log']);
  assert.equal(config.databasePath, '/tmp/game_0.db');
  assert.equal(config.itemCatalogPath, '/tmp/conan-items.json');

  rmSync(path.dirname(file), { recursive: true, force: true });
});

test('throws when required config is missing', () => {
  const file = withConfig('registrationToken=reg-token\n');

  assert.throws(() => loadConfig(file), /Missing required config: serverName/);

  rmSync(path.dirname(file), { recursive: true, force: true });
});
