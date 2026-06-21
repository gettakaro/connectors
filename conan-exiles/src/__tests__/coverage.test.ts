import assert from 'node:assert/strict';
import { test } from 'node:test';
import {
  ACTION_COVERAGE,
  EVENT_COVERAGE,
  getActionCoverage,
  getEventCoverage,
} from '../takaro/coverage.js';
import { ALL_GAME_EVENT_TYPES, ALL_GAME_SERVER_ACTIONS } from '../takaro/protocol.js';

test('coverage registry accounts for every Takaro action', () => {
  assert.deepEqual(Object.keys(ACTION_COVERAGE).sort(), [...ALL_GAME_SERVER_ACTIONS].sort());

  for (const action of ALL_GAME_SERVER_ACTIONS) {
    const coverage = getActionCoverage(action);
    assert.ok(coverage.status);
    assert.ok(coverage.responseShape);
    assert.ok(coverage.liveVerification);
    assert.ok(coverage.reason);
  }
});

test('coverage registry accounts for every Takaro event type', () => {
  assert.deepEqual(Object.keys(EVENT_COVERAGE).sort(), [...ALL_GAME_EVENT_TYPES].sort());

  for (const eventType of ALL_GAME_EVENT_TYPES) {
    const coverage = getEventCoverage(eventType);
    assert.ok(coverage.status);
    assert.ok(coverage.payloadShape);
    assert.ok(coverage.liveVerification);
    assert.ok(coverage.reason);
  }
});

test('coverage registry separates real support, schema fallbacks, and unsupported actions', () => {
  assert.equal(getActionCoverage('getPlayers').status, 'live-supported');
  assert.equal(getActionCoverage('sendMessage').status, 'live-supported');
  assert.equal(getActionCoverage('getPlayerInventory').status, 'live-supported');
  assert.equal(getActionCoverage('getPlayerLocation').status, 'live-supported');
  assert.equal(getActionCoverage('listItems').status, 'live-supported');
  assert.equal(getActionCoverage('listEntities').status, 'live-supported');
  assert.equal(getActionCoverage('listLocations').status, 'live-supported');
  assert.equal(getActionCoverage('getMapInfo').status, 'schema-fallback');
  assert.equal(getActionCoverage('giveItem').status, 'live-supported');
  assert.equal(getActionCoverage('teleportPlayer').status, 'live-supported');
});

test('coverage registry records unavailable Conan/Pippi runtime gaps', () => {
  assert.match(getActionCoverage('getMapTile').reason, /not available/i);
});

test('coverage registry records Conan log-backed death and entity kill events', () => {
  assert.equal(getEventCoverage('player-death').status, 'live-supported');
  assert.match(getEventCoverage('player-death').reason, /KillCharacterWithRagdoll/i);
  assert.equal(getEventCoverage('entity-killed').status, 'live-supported');
  assert.match(getEventCoverage('entity-killed').reason, /KillCharacterWithRagdoll/i);
});
