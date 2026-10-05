import assert from 'node:assert/strict';
import { test } from 'node:test';
import { PresenceGate } from '../events/presenceGate.js';
import type { GameEvent } from '../takaro/protocol.js';

const joined = (gameId: string): GameEvent => ({ type: 'player-connected', data: { player: { gameId, name: gameId } } }) as GameEvent;
const left = (gameId: string): GameEvent => ({ type: 'player-disconnected', data: { player: { gameId, name: gameId } } }) as GameEvent;

test('a join or leave seen by both the poller and the log is reported once', () => {
  const gate = new PresenceGate();
  assert.equal(gate.accept(joined('Bob J. Test')), true);
  assert.equal(gate.accept(joined('Bob J. Test')), false);
  assert.equal(gate.accept(left('Bob J. Test')), true);
  assert.equal(gate.accept(left('Bob J. Test')), false);
  assert.equal(gate.accept(joined('Bob J. Test')), true);
});

test('other events and other players pass, and a reset forgets who is online', () => {
  const gate = new PresenceGate();
  assert.equal(gate.accept(joined('A')), true);
  assert.equal(gate.accept(joined('B')), true);
  assert.equal(gate.accept({ type: 'chat-message', data: { msg: 'hi' } } as GameEvent), true);
  gate.reset();
  assert.equal(gate.accept(joined('A')), true);
});
