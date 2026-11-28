// Poll watchdog: exit only for a stale network namespace (only loopback left).
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { PollWatchdog, namespaceHasExternalInterface } from '../dist/watchdog.js';

const TICK = 5000;
const make = (alive) => new PollWatchdog({ thresholdMs: 60_000, tickMs: TICK, namespaceAlive: () => alive.value });

function run(w, from, to, lastPoll) {
  let r;
  for (let t = from; t <= to; t += TICK) r = w.tick(t, lastPoll);
  return r;
}

test('live namespace, never polled: ok', () => {
  assert.equal(run(make({ value: true }), 0, 600_000, null).verdict, 'ok');
});

test('live namespace, recent poll: ok', () => {
  assert.equal(run(make({ value: true }), 0, 50_000, 0).verdict, 'ok');
});

test('live namespace, mod silent past threshold (game hung or host stalled): stays up', () => {
  assert.equal(run(make({ value: true }), 0, 600_000, 0).verdict, 'silent');
});

test('only loopback left for the threshold: exit', () => {
  const alive = { value: true };
  const w = make(alive);
  run(w, 0, 30_000, 25_000);
  alive.value = false;
  assert.equal(run(w, 35_000, 90_000, 25_000).verdict, 'ok');
  const r = run(w, 95_000, 95_000, 25_000);
  assert.equal(r.verdict, 'exit');
  assert.equal(r.silentMs, 70_000);
});

test('stale namespace exits even if the mod never polled this bridge', () => {
  const w = make({ value: false });
  assert.equal(run(w, 0, 55_000, null).verdict, 'ok');
  assert.equal(run(w, 60_000, 60_000, null).verdict, 'exit');
});

test('namespace coming back resets the stale clock', () => {
  const alive = { value: false };
  const w = make(alive);
  run(w, 0, 50_000, null);
  alive.value = true;
  run(w, 55_000, 55_000, null);
  alive.value = false;
  assert.equal(run(w, 60_000, 110_000, null).verdict, 'ok');
  assert.equal(run(w, 115_000, 120_000, null).verdict, 'exit');
});

test('a bridge stall is not counted as mod silence', () => {
  const w = make({ value: true });
  run(w, 0, 30_000, 0);
  const late = w.tick(180_000, 0); // event loop blocked for 150 s
  assert.equal(late.verdict, 'ok');
  assert.equal(late.stalledMs, 145_000);
  assert.equal(run(w, 185_000, 235_000, 0).verdict, 'ok');
  assert.equal(run(w, 245_000, 245_000, 0).verdict, 'silent');
});

test('namespaceHasExternalInterface reads the real interfaces', () => {
  assert.equal(typeof namespaceHasExternalInterface(), 'boolean');
});
