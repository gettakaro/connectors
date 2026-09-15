import assert from 'node:assert/strict';
import { test } from 'node:test';
import {
  ACTION_COVERAGE,
  EVENT_COVERAGE,
  getActionCoverage,
  getEventCoverage,
  schemaFallbackForAction,
  unsupportedActionError,
} from '../takaro/coverage.js';
import { ALL_GAME_EVENT_TYPES, ALL_GAME_SERVER_ACTIONS } from '../takaro/protocol.js';

test('coverage registry accounts for every Takaro action and event', () => {
  assert.deepEqual(Object.keys(ACTION_COVERAGE).sort(), [...ALL_GAME_SERVER_ACTIONS].sort());
  assert.deepEqual(Object.keys(EVENT_COVERAGE).sort(), [...ALL_GAME_EVENT_TYPES].sort());

  for (const action of ALL_GAME_SERVER_ACTIONS) {
    const coverage = getActionCoverage(action);
    assert.ok(coverage.status);
    assert.ok(coverage.responseShape);
    assert.ok(coverage.liveVerification);
    assert.ok(coverage.reason);
  }

  for (const eventType of ALL_GAME_EVENT_TYPES) {
    const coverage = getEventCoverage(eventType);
    assert.ok(coverage.status);
    assert.ok(coverage.payloadShape);
    assert.ok(coverage.liveVerification);
    assert.ok(coverage.reason);
  }
});

test('coverage registry distinguishes live support, schema fallbacks, and unsupported actions', () => {
  assert.equal(getActionCoverage('getPlayers').status, 'live-supported');
  assert.equal(getActionCoverage('executeConsoleCommand').status, 'live-supported');
  assert.equal(getActionCoverage('listItems').status, 'live-supported');
  assert.equal(getActionCoverage('getPlayerLocation').status, 'live-supported');
  assert.equal(getActionCoverage('getPlayerInventory').status, 'live-supported');
  assert.equal(getActionCoverage('listEntities').status, 'schema-fallback');
  assert.equal(getActionCoverage('getMapInfo').status, 'schema-fallback');
  assert.equal(getActionCoverage('getMapTile').status, 'unsupported');

  assert.deepEqual(schemaFallbackForAction('listEntities'), []);
  assert.equal(schemaFallbackForAction('getPlayerInventory'), undefined);
  assert.equal(schemaFallbackForAction('listItems'), undefined);
  assert.deepEqual(schemaFallbackForAction('getMapInfo'), {
    enabled: false,
    mapBlockSize: 0,
    maxZoom: 0,
    mapSizeX: 0,
    mapSizeY: 0,
    mapSizeZ: 0,
  });
  assert.equal(unsupportedActionError('getMapTile')?.success, false);
});
