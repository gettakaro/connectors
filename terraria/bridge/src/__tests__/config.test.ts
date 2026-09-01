import assert from 'node:assert/strict';
import { mkdirSync, mkdtempSync, utimesSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { test } from 'node:test';
import { loadConfig, parseKeyValues } from '../config.js';

test('parses key value config with command guards and log files', () => {
  const values = parseKeyValues(`
    # comment
    registrationToken=rt
    serverName=Terraria Local
    tshockBaseUrl=http://127.0.0.1:7878
    tshockToken=t
    commandAllowlistExact=help, save
    commandAllowlistPrefixes=say , time
    logFiles=server.log, tshock/log.txt
    enableShutdown=true
  `);

  assert.equal(values.registrationToken, 'rt');
  assert.equal(values.serverName, 'Terraria Local');
  assert.equal(values.commandAllowlistExact, 'help, save');
});

test('loads config from file and environment without leaking required secrets into examples', () => {
  const dir = mkdtempSync(path.join(tmpdir(), 'terraria-config-'));
  const file = path.join(dir, 'TakaroConfig.txt');
  writeFileSync(file, `
registrationToken=
identityToken=terraria-local-id
serverName=Terraria Local
serverChatName=Takaro
takaroWsUrl=ws://127.0.0.1:18080
tshockBaseUrl=http://127.0.0.1:7878
tshockUsername=admin
tshockPassword=password
httpPort=0
pollIntervalMs=250
logFiles=a.log,b.log
commandAllowlistExact=help
commandAllowlistPrefixes=say
enableShutdown=false
`);

  const config = loadConfig(file, { TAKARO_REGISTRATION_TOKEN: 'from-env', TSHOCK_TOKEN: 'token-env' });

  assert.equal(config.registrationToken, 'from-env');
  assert.equal(config.identityToken, 'terraria-local-id');
  assert.equal(config.serverName, 'Terraria Local');
  assert.equal(config.serverChatName, 'Takaro');
  assert.equal(config.tshock.token, 'token-env');
  assert.equal(config.tshock.username, 'admin');
  assert.equal(config.httpPort, 0);
  assert.equal(config.pollIntervalMs, 250);
  assert.deepEqual(config.logFiles, ['a.log', 'b.log']);
  assert.deepEqual(config.commandAllowlistExact, ['help']);
  assert.deepEqual(config.commandAllowlistPrefixes, ['say']);
  assert.equal(config.enableShutdown, false);
});

test('passes log directories through so the tailer can follow TShock log rotation', () => {
  const dir = mkdtempSync(path.join(tmpdir(), 'terraria-config-logs-'));
  const logs = path.join(dir, 'logs');
  const file = path.join(dir, 'TakaroConfig.txt');
  const older = path.join(logs, '2026-06-21_10-00-00.log');
  const newer = path.join(logs, '2026-06-21_11-00-00.log');
  mkdirSync(logs);
  writeFileSync(older, 'old');
  writeFileSync(newer, 'new');
  utimesSync(older, new Date('2026-06-21T10:00:00Z'), new Date('2026-06-21T10:00:00Z'));
  utimesSync(newer, new Date('2026-06-21T11:00:00Z'), new Date('2026-06-21T11:00:00Z'));
  writeFileSync(file, `
registrationToken=rt
serverName=Terraria Local
tshockBaseUrl=http://127.0.0.1:7878
tshockToken=t
logFiles=${logs}
`);

  const config = loadConfig(file, {});

  // Resolving to `newer` here would pin the bridge to whichever log was newest at startup,
  // which is exactly what breaks when TShock restarts and opens a new file.
  assert.deepEqual(config.logFiles, [logs]);
});

test('requires a registration token and one TShock authentication method', () => {
  const dir = mkdtempSync(path.join(tmpdir(), 'terraria-config-missing-'));
  const file = path.join(dir, 'TakaroConfig.txt');
  writeFileSync(file, `
serverName=Terraria Local
tshockBaseUrl=http://127.0.0.1:7878
`);

  assert.throws(() => loadConfig(file, {}), /registrationToken/);
  assert.throws(() => loadConfig(file, { TAKARO_REGISTRATION_TOKEN: 'token' }), /tshockToken or tshockUsername\/tshockPassword/);
});
