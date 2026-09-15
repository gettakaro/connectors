import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { afterEach, beforeEach, describe, expect, it } from 'vitest';
import { Bridge } from '../bridge.js';
import { FileCursorStore, MemoryCursorStore } from '../enshrouded/cursorStore.js';
import { EventPoller } from '../enshrouded/eventPoller.js';
import { mapPluginEvent } from '../enshrouded/mapping.js';
import { FileOnlineStore, MemoryOnlineStore } from '../enshrouded/onlineStore.js';
import { EnshroudedPluginClient } from '../enshrouded/pluginClient.js';
import { MockPlugin } from '../testing/mockPlugin.js';

const SID = '76561198000005875';
const player = { gameId: SID, name: 'Limon', steamId: SID, peerId: '0(1)' };
const takaroPlayer = { gameId: SID, name: 'Limon', steamId: SID, platformId: `steam:${SID}` };

describe('player-death non-player killer', () => {
  it('forwards killerEntity in msg when there is no player attacker', () => {
    expect(mapPluginEvent({ type: 'player-death', data: { player, killerEntity: 'Enemy_Fogger_Depleted', position: { x: 1, y: 2, z: 3 } } })).toEqual({
      type: 'player-death',
      data: { player: takaroPlayer, position: { x: 1, y: 2, z: 3 }, msg: 'Limon was killed by Enemy_Fogger_Depleted' },
    });
  });
  it('no msg for a fall death, and a player attacker wins over killerEntity', () => {
    expect(mapPluginEvent({ type: 'player-death', data: { player } })?.data).not.toHaveProperty('msg');
    const d = mapPluginEvent({ type: 'player-death', data: { player, attacker: { gameId: SID, name: 'Bob' }, killerEntity: 'X' } })?.data;
    expect(d).toHaveProperty('attacker');
    expect(d).not.toHaveProperty('msg');
  });
});

describe('plugin bootId restart detection', () => {
  let mock: MockPlugin;
  beforeEach(async () => {
    mock = new MockPlugin();
    await mock.start();
  });
  afterEach(async () => mock.stop());

  it('persists bootId with the cursor and resets on a new bootId even when the new seq is already higher', async () => {
    const client = new EnshroudedPluginClient({ baseUrl: mock.url(), token: mock.token });
    const store = new MemoryCursorStore();
    const emitted: string[] = [];
    let restarts = 0;
    const poller = () =>
      new EventPoller({ getEvents: (s) => client.getEvents(s), emit: (_t, d: any) => void emitted.push(d.msg), store, onRestart: () => restarts++ });

    mock.bootId = 'aaaa';
    mock.pushEvent('log', { msg: 'old1' });
    mock.pushEvent('log', { msg: 'old2' });
    expect(await poller().pollOnce()).toBe(2);
    expect(store.state).toEqual({ seq: 2, bootId: 'aaaa' });

    // server restarts; new process already has 5 events before the sidecar polls again
    mock.events = [];
    mock.bootId = 'bbbb';
    for (const m of ['n1', 'n2', 'n3', 'n4', 'n5']) mock.pushEvent('log', { msg: m });
    const p = poller();
    expect(await p.pollOnce()).toBe(5);
    expect(restarts).toBe(1);
    expect(emitted).toEqual(['old1', 'old2', 'n1', 'n2', 'n3', 'n4', 'n5']);
    expect(store.state).toEqual({ seq: 5, bootId: 'bbbb' });

    // same boot: no replay
    expect(await poller().pollOnce()).toBe(5);
    expect(emitted).toHaveLength(7);
  });

  it('adopts a bootId for a legacy cursor without replaying', async () => {
    const client = new EnshroudedPluginClient({ baseUrl: mock.url(), token: mock.token });
    mock.pushEvent('log', { msg: 'a' });
    mock.pushEvent('log', { msg: 'b' });
    mock.bootId = 'cccc';
    const store = new MemoryCursorStore({ seq: 2 });
    const emitted: unknown[] = [];
    const p = new EventPoller({ getEvents: (s) => client.getEvents(s), emit: (_t, d) => void emitted.push(d), store });
    mock.pushEvent('log', { msg: 'c' });
    expect(await p.pollOnce()).toBe(3);
    expect(emitted).toEqual([{ msg: 'c' }]);
    expect(store.state).toEqual({ seq: 3, bootId: 'cccc' });
  });
});

describe('online reconciliation', () => {
  let mock: MockPlugin;
  let dir: string;
  beforeEach(async () => {
    mock = new MockPlugin();
    await mock.start();
    dir = fs.mkdtempSync(path.join(os.tmpdir(), 'ensh-online-'));
  });
  afterEach(async () => {
    await mock.stop();
    fs.rmSync(dir, { recursive: true, force: true });
  });

  const makeBridge = (onlineStore: FileOnlineStore | MemoryOnlineStore, events: Array<[string, any]>) =>
    new Bridge({
      plugin: new EnshroudedPluginClient({ baseUrl: mock.url(), token: mock.token }),
      takaro: { send: () => true, sendGameEvent: (t, d) => (events.push([t, d]), true) },
      cursorStore: new MemoryCursorStore(),
      onlineStore,
      logFile: '/nonexistent',
      logTailMode: 'never',
      pollIntervalMs: 60000,
      healthCheckIntervalMs: 60000,
    });

  it('tracks forwarded connects in the store and sends player-disconnected on startup for players no longer on the server', async () => {
    const file = path.join(dir, 'online-players.json');
    const events1: Array<[string, any]> = [];
    const b1 = makeBridge(new FileOnlineStore(file), events1);
    b1.noteConnectionEvent('player-connected', { player: takaroPlayer });
    expect(new FileOnlineStore(file).load()).toEqual([takaroPlayer]);

    // server restarted without logging the leave: plugin lists nobody
    mock.players = [];
    const events: Array<[string, any]> = [];
    const b2 = makeBridge(new FileOnlineStore(file), events);
    try {
      await b2.startEvents();
      expect(events).toContainEqual(['player-disconnected', { player: takaroPlayer }]);
      expect(new FileOnlineStore(file).load()).toEqual([]);
      expect(b2.onlinePlayers()).toEqual([]);
      // location lookup for the just-disconnected player is answered (Takaro stores the event only then)
      expect(b2.eventLocationFallback({ gameId: SID })).toEqual({ x: 0, y: 0, z: 0 });
    } finally {
      b2.stopEvents();
      b1.stopEvents();
    }
  });

  it('keeps players that are still online and disconnect events remove them', async () => {
    const store = new MemoryOnlineStore([{ ...takaroPlayer, gameId: mock.players[0].gameId, steamId: mock.players[0].gameId }]);
    const events: Array<[string, any]> = [];
    const b = makeBridge(store, events);
    try {
      expect(await b.reconcileOnline()).toEqual([]);
      expect(events.filter((e) => e[0] === 'player-disconnected')).toEqual([]);
      b.noteConnectionEvent('player-disconnected', { player: { gameId: mock.players[0].gameId } });
      expect(store.players).toEqual([]);
    } finally {
      b.stopEvents();
    }
  });

  it('postpones when the plugin is unreachable', async () => {
    const store = new MemoryOnlineStore([takaroPlayer]);
    const events: Array<[string, any]> = [];
    const b = makeBridge(store, events);
    await mock.stop();
    expect(await b.reconcileOnline()).toEqual([]);
    expect(events).toEqual([]);
    expect(store.players).toEqual([takaroPlayer]);
    await mock.start();
  });
});
