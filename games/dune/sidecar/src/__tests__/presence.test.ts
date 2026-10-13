import { describe, expect, it } from 'vitest';
import { PresencePoller } from '../dune/presence.js';
import type { DunePlayerRow, TakaroPlayer } from '../dune/types.js';
import { mockPlayer } from '../testing/mockBattlegroup.js';

interface Recorded {
  type: string;
  data: Record<string, unknown>;
}

function harness(options: { initialOnline?: TakaroPlayer[]; transferGraceMs?: number } = {}) {
  let roster: DunePlayerRow[] = [];
  let now = 1_000_000;
  const events: Recorded[] = [];
  const saved: TakaroPlayer[][] = [];
  const poller = new PresencePoller({
    roster: async () => roster,
    emit: (type, data) => events.push({ type, data }),
    initialOnline: options.initialOnline,
    save: (players) => saved.push(players),
    transferGraceMs: options.transferGraceMs ?? 45_000,
    now: () => now,
  });
  return {
    poller,
    events,
    saved,
    set: (rows: DunePlayerRow[]) => (roster = rows),
    advance: (ms: number) => (now += ms),
    tick: () => poller.pollOnce(),
  };
}

const TESTER = mockPlayer();
const CHANI = mockPlayer({ flsId: 'AAAA1111BBBB2222', funcomId: 'PLAYER#2', platformId: '76561198000000002', characterName: 'Chani', accountId: 2 });

describe('presence differ', () => {
  it('emits player-connected once per arrival, never twice for the same player', async () => {
    const h = harness();
    h.set([TESTER]);
    await h.tick();
    await h.tick();
    expect(h.events.map((e) => e.type)).toEqual(['player-connected']);
    expect((h.events[0].data.player as TakaroPlayer)).toMatchObject({
      gameId: '6FF6498F4074E3DE',
      name: 'Tester',
      steamId: '76561190000000001',
      platformId: 'steam:76561190000000001',
    });

    h.set([TESTER, CHANI]);
    await h.tick();
    expect(h.events).toHaveLength(2);
    expect((h.events[1].data.player as TakaroPlayer).name).toBe('Chani');
  });

  it('a map transfer inside the grace window produces NO connect/disconnect pair', async () => {
    const h = harness({ transferGraceMs: 45_000 });
    h.set([TESTER]);
    await h.tick();
    h.events.length = 0;

    // Hagga Basin → Deep Desert: the row leaves `online_status = Online` for a few seconds.
    h.set([]);
    await h.tick();
    expect(h.events).toEqual([]);
    expect(h.poller.pendingLeaves()).toEqual(['6FF6498F4074E3DE']);

    h.advance(10_000);
    h.set([{ ...TESTER, map: 'Deep Desert', partitionId: 31 }]);
    await h.tick();
    expect(h.events).toEqual([]);
    expect(h.poller.pendingLeaves()).toEqual([]);
    expect(h.poller.onlinePlayers()).toHaveLength(1);
  });

  it('a real disconnect is reported once the grace window expires', async () => {
    const h = harness({ transferGraceMs: 45_000 });
    h.set([TESTER]);
    await h.tick();
    h.events.length = 0;

    h.set([]);
    await h.tick();
    expect(h.events).toEqual([]);

    h.advance(46_000);
    await h.tick();
    expect(h.events.map((e) => e.type)).toEqual(['player-disconnected']);
    expect(h.poller.onlinePlayers()).toEqual([]);
  });

  it('restart: a player already in the persisted online set is not re-announced, one who left IS reported gone', async () => {
    const remembered: TakaroPlayer[] = [
      { gameId: TESTER.flsId, name: 'Tester' },
      { gameId: CHANI.flsId, name: 'Chani' },
    ];
    const h = harness({ initialOnline: remembered, transferGraceMs: 0 });
    h.set([TESTER]); // Chani left while the sidecar was down
    await h.tick();
    expect(h.events.map((e) => e.type)).toEqual(['player-disconnected']);
    expect((h.events[0].data.player as TakaroPlayer).name).toBe('Chani');
    // No connect replay for Tester: Takaro was already told about him before the restart.
    expect(h.events.filter((e) => e.type === 'player-connected')).toHaveLength(0);
  });

  it('a life_state EDGE into a dead state emits exactly one player-death, with a position and a cause', async () => {
    const h = harness();
    h.set([TESTER]);
    await h.tick();
    h.events.length = 0;

    h.set([{ ...TESTER, lifeState: 'DeadBySandworm', deathLocation: { x: 5, y: 6, z: 7 } }]);
    await h.tick();
    await h.tick(); // still dead: not a second death
    expect(h.events.map((e) => e.type)).toEqual(['player-death']);
    expect(h.events[0].data.position).toEqual({ x: 5, y: 6, z: 7, dimension: 'survival_1' });
    expect(h.events[0].data.msg).toBe('Tester died (sandworm)');

    // Respawn, then die again: a fresh edge, a fresh event.
    h.set([{ ...TESTER, lifeState: 'Alive' }]);
    await h.tick();
    h.set([{ ...TESTER, lifeState: 'Dead' }]);
    await h.tick();
    expect(h.events.filter((e) => e.type === 'player-death')).toHaveLength(2);
    expect(h.events.at(-1)!.data.msg).toBe('Tester died');
  });

  it('a corpse found on the first poll is not reported as a death we witnessed', async () => {
    const h = harness();
    h.set([{ ...TESTER, lifeState: 'Dead' }]);
    await h.tick();
    expect(h.events.map((e) => e.type)).toEqual(['player-connected']);
  });

  it('persists the online set after every tick', async () => {
    const h = harness();
    h.set([TESTER]);
    await h.tick();
    expect(h.saved.at(-1)).toHaveLength(1);
    h.set([]);
    await h.tick();
    h.advance(60_000);
    await h.tick();
    expect(h.saved.at(-1)).toEqual([]);
  });

  it('a roster failure is reported, not thrown, and the online set survives it', async () => {
    let fail = false;
    const events: Recorded[] = [];
    const poller = new PresencePoller({
      roster: async () => {
        if (fail) throw new Error('postgres is down');
        return [TESTER];
      },
      emit: (type, data) => events.push({ type, data }),
      transferGraceMs: 0,
    });
    await poller.pollOnce();
    fail = true;
    await expect(poller.pollOnce()).rejects.toThrow('postgres is down');
    expect(poller.onlinePlayers()).toHaveLength(1);
    expect(events.filter((e) => e.type === 'player-disconnected')).toHaveLength(0);
  });
});

// =================================================================================================
// Two sources, one edge. The plugin's PostLogin hook and this differ both see the same join; whichever
// is slower must not announce it a second time. Measured live 2026-09-21: presence emitted
// `player-connected` at 19:20:06 and the plugin's delayed hint emitted another at 19:20:34 — two Takaro
// rows for one join. `note`/`forget` are how the plugin path hands the edge over.
// =================================================================================================

describe('handing an edge over to the plugin source', () => {
  it('note() suppresses the connect the next poll would otherwise emit', async () => {
    const h = harness();
    h.poller.note({ gameId: TESTER.flsId!, name: 'TakaroTest' }, TESTER);
    h.set([TESTER]);
    await h.tick();
    expect(h.events.filter((e) => e.type === 'player-connected')).toHaveLength(0);
    expect(h.poller.onlinePlayers().map((p) => p.gameId)).toEqual([TESTER.flsId]);
  });

  it('forget() suppresses the disconnect the next poll would otherwise emit, grace window included', async () => {
    const h = harness();
    h.set([TESTER]);
    await h.tick();
    expect(h.events.filter((e) => e.type === 'player-connected')).toHaveLength(1);
    h.poller.forget(TESTER.flsId!);
    h.set([]);
    await h.tick();
    h.advance(120_000);
    await h.tick();
    expect(h.events.filter((e) => e.type === 'player-disconnected')).toHaveLength(0);
  });

  it('still emits normally for a player no source handed over', async () => {
    const h = harness();
    h.set([TESTER, CHANI]);
    await h.tick();
    h.poller.forget(TESTER.flsId!);
    h.set([]);
    await h.tick();
    h.advance(120_000);
    await h.tick();
    const gone = h.events.filter((e) => e.type === 'player-disconnected');
    expect(gone).toHaveLength(1);
    expect((gone[0].data.player as { name: string }).name).toBe('Chani');
  });
});
