import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { afterEach, beforeEach, describe, expect, it } from 'vitest';
import { Bridge, shouldForwardLog, shouldTailLog } from '../bridge.js';
import { FileCursorStore, MemoryCursorStore } from '../enshrouded/cursorStore.js';
import { EventPoller } from '../enshrouded/eventPoller.js';
import { mapEntityType, mapPluginEvent } from '../enshrouded/mapping.js';
import { EnshroudedPluginClient } from '../enshrouded/pluginClient.js';
import { MockPlugin } from '../testing/mockPlugin.js';

const SID = '76561198000005875';
const player = { gameId: SID, name: 'Limon', steamId: SID, peerId: '0(1)' };
const takaroPlayer = { gameId: SID, name: 'Limon', steamId: SID, platformId: `steam:${SID}` };

describe('event mapping', () => {
  it('player-connected / player-disconnected (nested or flat player)', () => {
    expect(mapPluginEvent({ type: 'player-connected', data: { player } })).toEqual({ type: 'player-connected', data: { player: takaroPlayer } });
    expect(mapPluginEvent({ type: 'player-disconnected', data: player })).toEqual({ type: 'player-disconnected', data: { player: takaroPlayer } });
  });
  it('chat-message', () => {
    expect(mapPluginEvent({ type: 'chat-message', data: { player, message: 'hi', channel: 'Global' } })).toEqual({
      type: 'chat-message',
      data: { player: takaroPlayer, msg: 'hi', channel: 'global' },
    });
    expect(mapPluginEvent({ type: 'chat-message', data: { msg: 'sys', channel: 'team' } })).toEqual({ type: 'chat-message', data: { msg: 'sys', channel: 'team' } });
  });
  it('player-death with position and attacker', () => {
    expect(mapPluginEvent({ type: 'player-death', data: { player, position: { x: 1, y: 2, z: 3 }, attacker: { gameId: 'x', name: 'Bob', steamId: '42' } } })).toEqual({
      type: 'player-death',
      data: { player: takaroPlayer, position: { x: 1, y: 2, z: 3 }, attacker: { gameId: '42', name: 'Bob', steamId: '42', platformId: 'steam:42' } },
    });
  });
  it('entity-killed', () => {
    expect(mapPluginEvent({ type: 'entity-killed', data: { player, entity: 'Scavenger', weapon: 'Sword_Iron' } })).toEqual({
      type: 'entity-killed',
      data: { player: takaroPlayer, entity: 'Scavenger', weapon: 'Sword_Iron' },
    });
    expect(mapPluginEvent({ type: 'entity-killed', data: { player, entity: { code: 'Wolf' } } })?.data).toMatchObject({ entity: 'Wolf', weapon: '' });
  });
  it('log', () => {
    expect(mapPluginEvent({ type: 'log', data: { msg: 'line' } })).toEqual({ type: 'log', data: { msg: 'line' } });
    expect(mapPluginEvent({ type: 'log', data: { msg: '[server] Saved', level: 'info' } })).toEqual({ type: 'log', data: { msg: '[server] Saved' } });
    expect(mapPluginEvent({ type: 'log', data: 'raw' })).toEqual({ type: 'log', data: { msg: 'raw' } });
  });
  it('unknown types are dropped; entity type enum mapping', () => {
    expect(mapPluginEvent({ type: 'player-sync', data: {} })).toBeNull();
    expect(mapEntityType('Enemy')).toBe('hostile');
    expect(mapEntityType('friendly')).toBe('friendly');
    expect(mapEntityType(undefined)).toBe('neutral');
  });
});

describe('EventPoller + cursor persistence', () => {
  let mock: MockPlugin;
  let dir: string;
  beforeEach(async () => {
    mock = new MockPlugin();
    await mock.start();
    dir = fs.mkdtempSync(path.join(os.tmpdir(), 'ensh-cursor-'));
  });
  afterEach(async () => {
    await mock.stop();
    fs.rmSync(dir, { recursive: true, force: true });
  });

  const client = () => new EnshroudedPluginClient({ baseUrl: mock.url(), token: mock.token });

  it('forwards events in order, persists seq, and does not replay after restart', async () => {
    const file = path.join(dir, 'sub', 'cursor.json');
    const emitted: Array<[string, unknown]> = [];
    mock.pushEvent('player-connected', { player });
    mock.pushEvent('chat-message', { player, msg: 'hello' });
    mock.pushEvent('bogus', {});

    const p1 = new EventPoller({ getEvents: (s) => client().getEvents(s), emit: (t, d) => void emitted.push([t, d]), store: new FileCursorStore(file) });
    expect(await p1.pollOnce()).toBe(3);
    expect(emitted.map((e) => e[0])).toEqual(['player-connected', 'chat-message']);
    expect(JSON.parse(fs.readFileSync(file, 'utf8'))).toEqual({ seq: 3 });
    expect(mock.lastRequest('GET', '/events')?.query.since).toBe('0');

    mock.pushEvent('player-death', { player, position: { x: 0, y: 0, z: 0 } });
    const p2 = new EventPoller({ getEvents: (s) => client().getEvents(s), emit: (t, d) => void emitted.push([t, d]), store: new FileCursorStore(file) });
    expect(p2.cursor()).toBe(3);
    await p2.pollOnce();
    expect(mock.lastRequest('GET', '/events')?.query.since).toBe('3');
    expect(emitted.map((e) => e[0])).toEqual(['player-connected', 'chat-message', 'player-death']);
    expect(new FileCursorStore(file).load()).toEqual({ seq: 4 });
  });

  it('does not advance past events Takaro could not accept', async () => {
    mock.pushEvent('log', { msg: 'a' });
    mock.pushEvent('log', { msg: 'b' });
    const store = new MemoryCursorStore();
    let accept = false;
    const got: unknown[] = [];
    const poller = new EventPoller({ getEvents: (s) => client().getEvents(s), emit: (_t, d) => (accept ? (got.push(d), true) : false), store });
    expect(await poller.pollOnce()).toBe(0);
    accept = true;
    expect(await poller.pollOnce()).toBe(2);
    expect(got).toEqual([{ msg: 'a' }, { msg: 'b' }]);
  });

  it('resets cursor when plugin seq goes backwards (plugin restart)', async () => {
    const store = new MemoryCursorStore({ seq: 50 });
    mock.pushEvent('log', { msg: 'after restart' });
    const got: unknown[] = [];
    const poller = new EventPoller({ getEvents: (s) => client().getEvents(s), emit: (_t, d) => void got.push(d), store });
    expect(await poller.pollOnce()).toBe(1);
    expect(got).toEqual([{ msg: 'after restart' }]);
  });

  it('suppresses tailed types while log tail is active', async () => {
    mock.pushEvent('player-connected', { player });
    mock.pushEvent('chat-message', { player, msg: 'x' });
    const types: string[] = [];
    const poller = new EventPoller({
      getEvents: (s) => client().getEvents(s),
      emit: (t) => void types.push(t),
      store: new MemoryCursorStore(),
      suppress: () => new Set(['player-connected']),
    });
    expect(await poller.pollOnce()).toBe(2);
    expect(types).toEqual(['chat-message']);
  });
});

describe('log tail selection', () => {
  it('shouldForwardLog drops periodic stats spam in filtered mode', () => {
    expect(shouldForwardLog('filtered', { msg: '-------------- Session ----------------' })).toBe(false);
    expect(shouldForwardLog('filtered', { msg: '  m#0(128): up 0 (0), down 0 (0)' })).toBe(false);
    expect(shouldForwardLog('filtered', { msg: "[server] Player 'Limon' logged in with Permissions:" })).toBe(true);
    expect(shouldForwardLog('all', { msg: 'Machines:' })).toBe(true);
    expect(shouldForwardLog('none', { msg: '[server] Saved' })).toBe(false);
  });

  it('shouldTailLog', () => {
    expect(shouldTailLog('never', null)).toBe(false);
    expect(shouldTailLog('always', { status: 'ok' })).toBe(true);
    expect(shouldTailLog('auto', null)).toBe(true);
    expect(shouldTailLog('auto', { status: 'ok', capabilities: { logEvents: 'ok', players: 'ok', chatEvents: 'unimplemented' } })).toBe(false);
    expect(shouldTailLog('auto', { status: 'ok', capabilities: { logEvents: 'degraded', players: 'degraded' } })).toBe(true);
    expect(shouldTailLog('auto', { status: 'ok', capabilities: { logEvents: 'ok', players: 'degraded' } })).toBe(true);
  });

  it('bridge enables the tail when plugin events capability is degraded, and forwards log joins', async () => {
    const mock = new MockPlugin();
    await mock.start();
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'ensh-log-'));
    const logFile = path.join(dir, 'enshrouded_server.log');
    fs.writeFileSync(logFile, '[I 00:00:01,000] boot\n');
    const events: Array<[string, any]> = [];
    const bridge = new Bridge({
      plugin: new EnshroudedPluginClient({ baseUrl: mock.url(), token: mock.token }),
      takaro: { send: () => true, sendGameEvent: (t, d) => (events.push([t, d]), true) },
      cursorStore: new MemoryCursorStore(),
      logFile,
      logTailMode: 'auto',
      pollIntervalMs: 60000,
      healthCheckIntervalMs: 60000,
    });
    try {
      mock.health = { status: 'ok', capabilities: { logEvents: 'degraded', players: 'degraded' } };
      mock.pushEvent('player-connected', { player }); // must be suppressed (tail owns joins)
      await bridge.startEvents();
      expect(bridge.isLogTailActive()).toBe(true);
      fs.appendFileSync(
        logFile,
        [
          `[I 00:03:52,650] [online] Added peer 0(1) (steamid:${SID})`,
          `[I 00:04:24,062] [server] Machine '1': Player '0(0)' logged in`,
          `[I 00:04:24,063] [server] Player 'Limon' logged in with Permissions:`,
          '',
        ].join('\n'),
      );
      bridge.tailer.poll();
      await bridge.poller.pollOnce();
      expect(events).toEqual([['player-connected', { player: takaroPlayer }]]);

      mock.health = { status: 'ok', capabilities: { logEvents: 'ok', players: 'ok' } };
      await bridge.refreshHealth();
      expect(bridge.isLogTailActive()).toBe(false);
    } finally {
      bridge.stopEvents();
      await mock.stop();
      fs.rmSync(dir, { recursive: true, force: true });
    }
  });
});
