import assert from 'node:assert/strict';
import { test } from 'node:test';
import { parseListBans, parseListPlayers } from '../conan/parsers.js';

test('parses numbered listplayers lines with Steam IDs', () => {
  const players = parseListPlayers(`
Players connected:
0. Alice | 76561198000000001
1. Bob The Builder | Steam:76561198000000002
`);

  assert.deepEqual(players, [
    {
      gameId: '76561198000000001',
      name: 'Alice',
      steamId: '76561198000000001',
      platformId: 'steam:76561198000000001',
      online: true,
    },
    {
      gameId: '76561198000000002',
      name: 'Bob The Builder',
      steamId: '76561198000000002',
      platformId: 'steam:76561198000000002',
      online: true,
    },
  ]);
});

test('parses key/value listplayers lines with user and platform IDs', () => {
  const players = parseListPlayers('2: name=Carol userid=12345 platformid=Steam:76561198000000003');

  assert.deepEqual(players, [
    {
      gameId: '76561198000000003',
      name: 'Carol',
      platformId: 'steam:76561198000000003',
      steamId: '76561198000000003',
      online: true,
    },
  ]);
});

test('ignores unparseable listplayers lines', () => {
  assert.deepEqual(parseListPlayers('No players connected\n----'), []);
});

test('parses live empty listplayers output as no players', () => {
  assert.deepEqual(parseListPlayers('Idx | Char name | Player name | User ID | Platform ID | Platform Name'), []);
});

test('parses live table listplayers output with Conan user and platform IDs', () => {
  const players = parseListPlayers(`
Idx | Char name | Player name |      User ID |       Platform ID | Platform Name
  0 | TestExile | Tester#1000 | A-TESTUSER | 76561198000000001 |         STEAM
`);

  assert.deepEqual(players, [
    {
      gameId: '76561198000000001',
      name: 'Tester#1000',
      rconId: '0',
      characterName: 'TestExile',
      steamId: '76561198000000001',
      platformId: 'steam:76561198000000001',
      online: true,
    },
  ]);
});

test('parses listbans output into ban objects', () => {
  const bans = parseListBans(`
0. 76561198000000004 Alice griefing
platformid=Steam:76561198000000005 reason=spam
`);

  assert.deepEqual(bans, [
    {
      gameId: '76561198000000004',
      steamId: '76561198000000004',
      platformId: 'steam:76561198000000004',
      reason: 'Alice griefing',
    },
    {
      gameId: '76561198000000005',
      steamId: '76561198000000005',
      platformId: 'steam:76561198000000005',
      reason: 'spam',
    },
  ]);
});

test('parses live empty listbans output as no bans', () => {
  assert.deepEqual(parseListBans('Successfully executed: listbans'), []);
});
