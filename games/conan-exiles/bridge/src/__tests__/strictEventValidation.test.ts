import assert from 'node:assert/strict';
import { test } from 'node:test';
import { validateStrictModEvent } from '../mod/strictEventValidation.js';
import type { ConanPlayer } from '../conan/parsers.js';

const knownPlayers: ConanPlayer[] = [{
  gameId: '76561198000735875',
  name: 'Limon#67642',
  steamId: '76561198000735875',
  platformId: 'steam:76561198000735875',
  online: true,
}];

test('strict mod event validation accepts chat from known stable player identity', async () => {
  await validateStrictModEvent('chat-message', {
    message: 'hello from client',
    player: {
      platformId: 'steam:76561198000735875',
      name: 'Limon#67642',
    },
  }, async () => knownPlayers);
});

test('strict mod event validation rejects chat without stable player identity', async () => {
  await assert.rejects(
    validateStrictModEvent('chat-message', {
      message: 'display name only is not enough',
      player: { name: 'Limon#67642' },
    }, async () => knownPlayers),
    /requires player gameId, steamId, or platformId/,
  );
});

test('strict mod event validation rejects chat from unknown stable player identity', async () => {
  await assert.rejects(
    validateStrictModEvent('chat-message', {
      message: 'spoofed identity',
      player: { platformId: 'steam:76561198000000000' },
    }, async () => knownPlayers),
    /rejected unknown player identity/,
  );
});

test('strict mod event validation rejects chat when no known players are available', async () => {
  await assert.rejects(
    validateStrictModEvent('chat-message', {
      message: 'no player snapshot',
      player: { gameId: '76561198000735875' },
    }, async () => []),
    /has no known Conan players/,
  );
});
