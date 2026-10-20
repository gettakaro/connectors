import { describe, expect, it, vi } from 'vitest';
import { DeathCoalescer } from '../dune/deathCoalescer.js';
import { EventPoller } from '../dune/eventPoller.js';
import { MemoryCursorStore } from '../dune/cursorStore.js';
import { mapPluginEvent } from '../dune/mapping.js';
import { PluginJoin } from '../dune/pluginJoin.js';
import type { DunePlayerRow, PluginPlayer } from '../dune/types.js';

const FLS = '6FF6498F4074E3DE';
const OTHER_FLS = 'AA11BB22CC33DD44';

function join(players: PluginPlayer[], roster: Partial<DunePlayerRow>[]): PluginJoin {
  return new PluginJoin({
    players: async () => ({ players }),
    roster: async () => roster as DunePlayerRow[],
    ttlMs: 0,
  });
}

// =================================================================================================
// The identity join. The plugin has Postgres row ids; Takaro wants the FLS id.
// =================================================================================================

describe('plugin -> FLS id join', () => {
  it('joins on accountId, which is a primary key, in both directions', async () => {
    const j = join(
      [{ ref: 'acct:42', accountId: 42, characterName: 'Tester' }],
      [{ flsId: FLS, accountId: 42, characterName: 'Tester' }],
    );
    const forward = await j.flsIdFor('acct:42');
    expect(forward).toMatchObject({ flsId: FLS, ref: 'acct:42' });
    // Both routes agreed here, and the audit trail says so.
    expect(forward?.how).toBe('accountId+characterName');
    expect((await j.refFor(FLS))?.ref).toBe('acct:42');
  });

  it('reports the ROW character name, not the account display name the plugin read off PlayerState', async () => {
    // Measured live 2026-09-21: at PostLogin the plugin's `PlayerNamePrivate` still holds the Funcom account name
    // `Tester`, while `player_state.character_name` is `TakaroTest`. Takaro was told `Tester`, so the onboarding
    // welcome greeted the wrong name. The plugin name stays the MATCHING key; the row is the identity authority.
    const j = join(
      [{ ref: 'acct:1', accountId: 1, characterName: 'Tester' }],
      [{ flsId: FLS, accountId: 1, characterName: 'TakaroTest' }],
    );
    const r = await j.flsIdFor('acct:1');
    expect(r?.characterName).toBe('TakaroTest');
    expect(r?.how).toBe('accountId');
  });

  it('accepts a bigint id that arrives as a string', async () => {
    const j = join(
      [{ ref: 'acct:9007199254740993', accountId: '9007199254740993', characterName: 'Big' }],
      [{ flsId: FLS, accountId: '9007199254740993' as unknown as number, characterName: 'Big' }],
    );
    expect((await j.flsIdFor('acct:9007199254740993'))?.flsId).toBe(FLS);
  });

  it('falls back to the character name, and records that weaker route', async () => {
    const j = join(
      [{ ref: 'name:Tester', characterName: 'tester' }], // the plugin could not read any id
      [{ flsId: FLS, accountId: 42, characterName: 'Tester' }],
    );
    const r = await j.flsIdFor('name:Tester');
    expect(r?.flsId).toBe(FLS);
    expect(r?.how).toBe('characterName');
  });

  it('REFUSES an ambiguous character name rather than picking one', async () => {
    const j = join(
      [{ ref: 'name:Twin', characterName: 'Twin' }],
      [
        { flsId: FLS, accountId: 1, characterName: 'Twin' },
        { flsId: OTHER_FLS, accountId: 2, characterName: 'Twin' },
      ],
    );
    expect(await j.flsIdFor('name:Twin')).toBeNull();
    expect(j.status()).toMatchObject({ ambiguousNames: 1, resolved: 0, unresolved: 1 });
  });

  it('REFUSES a join where the two routes disagree', async () => {
    // accountId 1 is Tester in Postgres, but the plugin reported the character name of someone else:
    // one of the two reads is wrong and we do not know which, so nothing is claimed.
    const j = join(
      [{ ref: 'acct:1', accountId: 1, characterName: 'Someone Else' }],
      [
        { flsId: FLS, accountId: 1, characterName: 'Tester' },
        { flsId: OTHER_FLS, accountId: 2, characterName: 'Someone Else' },
      ],
    );
    expect(await j.flsIdFor('acct:1')).toBeNull();
    expect(j.status()).toMatchObject({ disagreements: 1 });
  });

  it('never invents a join when the roster does not contain the player', async () => {
    const j = join([{ ref: 'acct:99', accountId: 99, characterName: 'Ghost' }], [{ flsId: FLS, accountId: 1, characterName: 'Tester' }]);
    expect(await j.flsIdFor('acct:99')).toBeNull();
    // ...not even when there is exactly one online player to "obviously" mean.
    expect(await j.refFor(FLS)).toBeNull();
  });

  it('survives an unreachable plugin without throwing and reports the error', async () => {
    const j = new PluginJoin({
      players: async () => {
        throw new Error('plugin unreachable');
      },
      roster: async () => [],
      ttlMs: 0,
    });
    expect(await j.flsIdFor('acct:1')).toBeNull();
    expect(String(j.status().error)).toMatch(/unreachable/);
  });
});

// =================================================================================================
// Event mapping. A plugin payload carries no gameId at all.
// =================================================================================================

describe('mapPluginEvent with the plugin payload shape', () => {
  const resolvePlayer = (source: Record<string, unknown>): { gameId: string; name: string } | null =>
    source.ref === 'acct:1' ? { gameId: FLS, name: 'Tester' } : null;

  it('drops a connect whose player cannot be joined, instead of inventing a gameId from the name', () => {
    const unjoinable = { type: 'player-connected', data: { ref: 'acct:77', characterName: 'Nobody', hint: true } };
    expect(mapPluginEvent(unjoinable, { resolvePlayer })).toBeNull();
    // Without a resolver the legacy shape still maps, which is what the plugin-absent tests rely on.
    expect(mapPluginEvent({ type: 'player-connected', data: { flsId: FLS, characterName: 'Tester' } })?.data).toMatchObject({
      player: { gameId: FLS },
    });
  });

  it('maps a joined connect', () => {
    const mapped = mapPluginEvent({ type: 'player-connected', data: { ref: 'acct:1', characterName: 'Tester' } }, { resolvePlayer });
    expect(mapped?.data).toMatchObject({ player: { gameId: FLS, name: 'Tester' } });
  });

  it('maps an attributed player-death, keeping victim and killer the right way round', () => {
    const mapped = mapPluginEvent(
      {
        type: 'player-death',
        data: {
          ref: 'acct:1',
          characterName: 'Tester',
          killer: { ref: 'acct:1', characterName: 'Tester' },
          position: { x: 1, y: 2, z: 3 },
          attribution: 'params',
        },
      },
      { resolvePlayer },
    );
    // The VICTIM is `player`; the killer is `attacker`. Getting this backwards is F20.
    expect(mapped?.data.player).toMatchObject({ gameId: FLS });
    expect(mapped?.data.attacker).toMatchObject({ gameId: FLS });
    expect(mapped?.data.position).toEqual({ x: 1, y: 2, z: 3 });
  });

  it('an NPC killer is named in msg, and a DEV name never is', () => {
    const mapped = mapPluginEvent(
      { type: 'player-death', data: { ref: 'acct:1', killer: null, killerEntityCode: 'ADuneNpcCharacter' } },
      { resolvePlayer },
    );
    expect(mapped?.data.attacker).toBeUndefined();
    // The class name is NOT interpolated into the player-facing message (catalogue rule).
    expect(mapped?.data.msg).toBe('Tester was killed by an NPC');
    expect(mapped?.data.msg).not.toMatch(/ADuneNpcCharacter/);
    expect(mapped?.data.killerEntityCode).toBe('ADuneNpcCharacter');
  });

  it('entity-killed is DROPPED when the entity cannot be NAMED, and kept when the catalogue names it', () => {
    const raw = {
      type: 'entity-killed',
      data: { player: { ref: 'acct:1' }, entity: null, entityCode: 'DuneCritterBase', nameIsClassName: true, weaponCode: 'BP_Dart_C' },
    };
    // No catalogue entry: there is no honest name to send, so the event is dropped rather than forwarded with
    // `DuneCritterBase` standing in for a creature (memory: catalogue-human-names). The drop is counted, not silent.
    const drops: string[] = [];
    const unnamed = mapPluginEvent(raw, { resolvePlayer, onDrop: (r) => drops.push(r) });
    expect(unnamed).toBeNull();
    expect(drops).toEqual(['entityKilledUnnamed']);

    const named = mapPluginEvent(raw, { resolvePlayer, entityName: (code) => (code === 'DuneCritterBase' ? 'Desert Mouse' : undefined) });
    expect(named?.data).toMatchObject({ entity: 'Desert Mouse' });

    // `entityCode` / `nameIsClassName` / `weaponCode` / `attribution` must NOT be built into the payload: Takaro
    // validates gameEvents with `forbidNonWhitelisted: true` and threw away a real kill over `entityCode`.
    expect(Object.keys(named?.data ?? {}).sort()).toEqual(['entity', 'player', 'weapon']);
    // `BP_Dart_C` is a dev name, so it can never BE the weapon. Lane L2d degrades it to the damage-type
    // CATEGORY instead of the empty string: "Ranged" is obviously a category rather than a thing you can
    // pick up, so it stays clear of the catalogue rule while still telling a player something.
    expect(named?.data.weapon).toBe('Ranged');
  });

  it('entity-killed with no joinable killer is dropped: Takaro cannot credit a kill to nobody', () => {
    expect(
      mapPluginEvent({ type: 'entity-killed', data: { player: { ref: 'acct:99' }, entityCode: 'DuneCritterBase' } }, { resolvePlayer }),
    ).toBeNull();
  });

  it('weapon is always a string, because Takaro drops the event otherwise (F20)', () => {
    // The catalogue has to name the entity first, or guard 1 drops the whole event before `weapon` matters.
    const mapped = mapPluginEvent(
      { type: 'entity-killed', data: { player: { ref: 'acct:1' }, entityCode: 'X' } },
      { resolvePlayer, entityName: () => 'Desert Mouse' },
    );
    expect(mapped?.data.weapon).toBe('');
  });
});

// =================================================================================================
// One death per death.
// =================================================================================================

describe('death coalescing', () => {
  function rig(windowMs = 1000) {
    const emitted: { type: string; data: Record<string, unknown> }[] = [];
    let now = 1_000_000;
    const timers: { fn: () => void; at: number }[] = [];
    const c = new DeathCoalescer({
      emit: (type, data) => emitted.push({ type, data }),
      windowMs,
      now: () => now,
      setTimer: (fn, ms) => {
        const t = { fn, at: now + ms };
        timers.push(t);
        return t;
      },
      clearTimer: (h) => {
        const i = timers.indexOf(h as { fn: () => void; at: number });
        if (i >= 0) timers.splice(i, 1);
      },
    });
    const advance = (ms: number): void => {
      now += ms;
      for (const t of [...timers]) {
        if (t.at <= now) {
          timers.splice(timers.indexOf(t), 1);
          t.fn();
        }
      }
    };
    return { c, emitted, advance, timers };
  }

  it('the plugin death replaces the held life_state edge: ONE event, the attributed one', () => {
    const { c, emitted, advance } = rig();
    c.fromLifeState(FLS, { player: { gameId: FLS }, msg: 'Tester died' });
    expect(emitted).toHaveLength(0); // held
    c.fromPlugin(FLS, { player: { gameId: FLS }, attacker: { gameId: OTHER_FLS }, msg: 'killed' });
    advance(5000);
    expect(emitted).toHaveLength(1);
    expect(emitted[0].data.attacker).toBeDefined();
    expect(c.status()).toMatchObject({ fromPlugin: 1, fromLifeState: 0, suppressedLifeState: 1 });
  });

  it('with no plugin the life_state edge is emitted after the window — late, never lost', () => {
    const { c, emitted, advance } = rig(1000);
    c.fromLifeState(FLS, { player: { gameId: FLS }, msg: 'Tester died' });
    advance(999);
    expect(emitted).toHaveLength(0);
    advance(2);
    expect(emitted).toHaveLength(1);
    expect(c.status()).toMatchObject({ fromLifeState: 1 });
  });

  it('a plugin death followed by the life_state edge is still ONE event', () => {
    const { c, emitted, advance } = rig(1000);
    c.fromPlugin(FLS, { player: { gameId: FLS }, attacker: { gameId: OTHER_FLS } });
    expect(emitted).toHaveLength(1);
    c.fromLifeState(FLS, { player: { gameId: FLS } });
    advance(5000);
    expect(emitted).toHaveLength(1);
    expect(c.status()).toMatchObject({ suppressedDuplicate: 1 });
  });

  it('two different players dying at once are two events', () => {
    const { c, emitted, advance } = rig(1000);
    c.fromLifeState(FLS, { player: { gameId: FLS } });
    c.fromLifeState(OTHER_FLS, { player: { gameId: OTHER_FLS } });
    advance(2000);
    expect(emitted).toHaveLength(2);
  });

  it('a genuine second death after a revival is a second event', () => {
    const { c, emitted, advance } = rig(1000);
    c.fromPlugin(FLS, { player: { gameId: FLS } });
    advance(1500);
    c.revived(FLS);
    c.fromPlugin(FLS, { player: { gameId: FLS } });
    expect(emitted).toHaveLength(2);
  });

  it('a plugin death trailing the emitted life_state edge by more than the window is still ONE event', () => {
    // Steam build 26051014, dehydration defeat: Postgres said Dead 13 s before the plugin's death arrived.
    const { c, emitted, advance } = rig(5000);
    c.fromLifeState(FLS, { player: { gameId: FLS }, msg: 'Tester died' });
    advance(5001);
    expect(emitted).toHaveLength(1);
    advance(8000);
    c.fromPlugin(FLS, { player: { gameId: FLS }, msg: 'Tester died' });
    expect(emitted).toHaveLength(1);
    expect(c.status()).toMatchObject({ fromLifeState: 1, fromPlugin: 0, suppressedDuplicate: 1 });
  });

  it('without an observed revival the dead mark expires after maxDeadMs', () => {
    const emitted: unknown[] = [];
    let now = 0;
    const c = new DeathCoalescer({ emit: (_t, d) => emitted.push(d), windowMs: 1000, maxDeadMs: 60_000, now: () => now });
    c.fromPlugin(FLS, { player: { gameId: FLS } });
    now += 59_999;
    c.fromPlugin(FLS, { player: { gameId: FLS } });
    expect(emitted).toHaveLength(1);
    now += 2;
    c.fromPlugin(FLS, { player: { gameId: FLS } });
    expect(emitted).toHaveLength(2);
  });

  it('flush() on shutdown emits what is still held rather than dropping it', () => {
    const { c, emitted } = rig(60_000);
    c.fromLifeState(FLS, { player: { gameId: FLS } });
    expect(emitted).toHaveLength(0);
    c.flush();
    expect(emitted).toHaveLength(1);
    expect(c.pendingIds()).toEqual([]);
  });
});

// =================================================================================================
// The cursor and bootId contract, with the joined mapper in place.
// =================================================================================================

describe('event poller against the plugin ring', () => {
  it('does not replay after a sidecar restart, and resets on a plugin restart (new bootId)', async () => {
    const store = new MemoryCursorStore();
    const emitted: number[] = [];
    const ring = [
      { seq: 1, type: 'entity-killed', data: { player: { ref: 'acct:1' }, entityCode: 'X' } },
      { seq: 2, type: 'entity-killed', data: { player: { ref: 'acct:1' }, entityCode: 'Y' } },
    ];
    let bootId = 'boot-a';
    const make = (): EventPoller =>
      new EventPoller({
        getEvents: async (since) => ({ bootId, seq: ring.at(-1)?.seq ?? 0, events: ring.filter((e) => e.seq > since) }),
        emit: (_type, _data, seq) => {
          emitted.push(seq!);
          return true;
        },
        store,
        mapEvent: (e) =>
          mapPluginEvent(e, {
            resolvePlayer: (s) => (s.ref === 'acct:1' ? { gameId: FLS, name: 'Tester' } : null),
            entityName: (code) => `Creature ${code}`,
          }),
      });

    await make().pollOnce();
    expect(emitted).toEqual([1, 2]);
    expect(store.load()).toMatchObject({ seq: 2, bootId: 'boot-a' });

    // A brand-new sidecar with the SAME plugin process: nothing is replayed.
    await make().pollOnce();
    expect(emitted).toEqual([1, 2]);

    // The map process restarted: its seq starts over, and the bootId says so even though seq 1 and 2
    // are numerically "old". Everything is re-read.
    bootId = 'boot-b';
    const restarted = make();
    const seen: string[] = [];
    await new EventPoller({
      getEvents: async (since) => ({ bootId, seq: 2, events: ring.filter((e) => e.seq > since) }),
      emit: (type, _d, seq) => {
        seen.push(`${type}:${seq}`);
        return true;
      },
      store,
      onRestart: () => seen.push('restart'),
      mapEvent: (e) => mapPluginEvent(e, { resolvePlayer: () => ({ gameId: FLS, name: 'Tester' }), entityName: (code) => `Creature ${code}` }),
    }).pollOnce();
    expect(seen[0]).toBe('restart');
    expect(seen).toContain('entity-killed:1');
    void restarted;
  });

  it('an event whose player cannot be joined is skipped without blocking the cursor', async () => {
    const store = new MemoryCursorStore();
    const emit = vi.fn(() => true);
    await new EventPoller({
      getEvents: async () => ({
        bootId: 'b',
        seq: 3,
        events: [
          { seq: 1, type: 'entity-killed', data: { player: { ref: 'acct:unknown' }, entityCode: 'X' } },
          { seq: 2, type: 'entity-killed', data: { player: { ref: 'acct:1' }, entityCode: 'Y' } },
        ],
      }),
      emit,
      store,
      mapEvent: (e) =>
        mapPluginEvent(e, {
          resolvePlayer: (s) => (s.ref === 'acct:1' ? { gameId: FLS, name: 'Tester' } : null),
          entityName: (code) => `Creature ${code}`,
        }),
    }).pollOnce();
    expect(emit).toHaveBeenCalledTimes(1);
    // The unjoinable event does not wedge the ring: the cursor moved past both.
    expect(store.load().seq).toBe(2);
  });
});

// =================================================================================================
// LANE L2d. The weapon name.
//
// The live defect: Tester's two melee kills reached Takaro with `weapon: ""`, because the plugin only
// had the damage-TYPE class (`BP_DmgType_Melee_Quick_C`) and a dev name may never be published as a
// display name. The plugin now reads the weapon off the killer as an ITEM TEMPLATE ID, which is the
// code space the catalogue already speaks — so the whole fix hinges on that lookup happening, and on
// every degrade below it staying honest.
// =================================================================================================

describe('weapon names', () => {
  const resolvePlayer = (): { gameId: string; name: string } => ({ gameId: FLS, name: 'TakaroTest' });
  const itemName = (code: string): string | undefined =>
    ({ ScrapMetalKnife: 'Scrap Metal Knife', ChoamSda2: 'Maula Pistol', MiningTool_1h_Standard: 'Improvised Cutteray' })[code];

  const kill = (data: Record<string, unknown>): Record<string, unknown> | undefined =>
    mapPluginEvent(
      { type: 'entity-killed', data: { player: { ref: 'acct:1' }, entity: 'Scavenger Cut-throat', ...data } },
      { resolvePlayer, entityName: () => 'Scavenger Cut-throat', itemName },
    )?.data;

  it("THE case: a melee kill carries the knife's display name, not the damage type", () => {
    const out = kill({
      weapon: null,
      weaponItemCode: 'ScrapMetalKnife',
      weaponCode: 'ScrapMetalKnife',
      damageTypeCode: 'BP_DmgType_Melee_Quick_C',
      weaponSource: 'meleeCache:damageTypeClassMatch',
    });
    expect(out?.weapon).toBe('Scrap Metal Knife');
    // ...and nothing else leaked into the whitelisted payload.
    expect(Object.keys(out ?? {}).sort()).toEqual(['entity', 'player', 'weapon']);
  });

  it('a ranged kill resolves the weapon component name through the same catalogue', () => {
    expect(kill({ weaponItemCode: 'ChoamSda2', weaponCode: 'ChoamSda2' })?.weapon).toBe('Maula Pistol');
  });

  it('an item code the catalogue cannot name never reaches Takaro as a code', () => {
    // `D_T9_Unreleased_Thing` is not in the catalogue and is obviously a dev id, so it degrades to the
    // damage-type category rather than being published verbatim (memory: catalogue-human-names).
    const out = kill({ weaponItemCode: 'D_T9_Unreleased_Thing', weaponCode: 'D_T9_Unreleased_Thing', damageTypeCode: 'BP_DmgType_Melee_Slow_Unshielded_C' });
    expect(out?.weapon).toBe('Melee');
  });

  it('a catalogue row whose name is still a dev name is refused', () => {
    const out = mapPluginEvent(
      // A catalogue that answers with `BP_Weird_C` is answering with a dev name, so the lookup is
      // discarded and the degrade chain continues — it does not get published just because a row existed.
      { type: 'entity-killed', data: { player: { ref: 'acct:1' }, entity: 'Scavenger Thug', weaponItemCode: 'D_Weird_Thing', weaponCode: 'D_Weird_Thing' } },
      { resolvePlayer, entityName: () => 'Scavenger Thug', itemName: () => 'BP_Weird_C' },
    )?.data;
    expect(out?.weapon).toBe('');
  });

  it('no weapon and no damage type at all is the empty string, never a missing field', () => {
    const out = kill({ weapon: null, weaponItemCode: null, weaponCode: null, damageTypeCode: null });
    // Takaro requires `weapon` to be a STRING and drops the whole event when it is absent (VEIN F20).
    expect(out?.weapon).toBe('');
    expect('weapon' in (out ?? {})).toBe(true);
  });

  it('an environmental damage type gets NO weapon: "Fall" in a weapon column reads as a weapon', () => {
    expect(kill({ damageTypeCode: 'BP_DmgType_Fall_C' })?.weapon).toBe('');
    expect(kill({ damageTypeCode: 'BP_DmgType_Dehydration_C' })?.weapon).toBe('');
  });

  it('player-death names the weapon in msg, which is the only whitelisted place it can go', () => {
    const out = mapPluginEvent(
      {
        type: 'player-death',
        data: {
          player: { ref: 'acct:1' },
          killer: null,
          killerEntityCode: 'BP_Npc_SoldierBase_Character_Baked_C',
          weaponItemCode: 'ChoamSda2',
          weaponCode: 'ChoamSda2',
        },
      },
      { resolvePlayer, itemName },
    )?.data;
    expect(out?.msg).toBe('TakaroTest was killed by an NPC with a Maula Pistol');
    // `EventPlayerDeath` has no weapon field at all, so nothing else may be attached.
    expect('weapon' in (out ?? {})).toBe(false);
  });

  it('an environmental death is never given a weapon, even if the payload carries one', () => {
    const out = mapPluginEvent(
      { type: 'player-death', data: { player: { ref: 'acct:1' }, killer: null, lifeState: 'DeadBySandworm', weaponItemCode: 'ScrapMetalKnife' } },
      { resolvePlayer, itemName },
    )?.data;
    expect(out?.msg).toBe('TakaroTest died (sandworm)');
  });
});
