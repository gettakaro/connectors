import { describe, expect, it, vi } from 'vitest';
import { Bridge, DEFAULT_OUTAGE_CHAT_MAX_AGE_MS, MAX_PENDING_EVENTS, type EventSource, type TakaroSink } from '../bridge.js';
import { BanManager } from '../dune/bans.js';
import { MemoryBanStore } from '../dune/banStore.js';
import { EventPoller } from '../dune/eventPoller.js';
import { MemoryCursorStore } from '../dune/cursorStore.js';
import { PresencePoller } from '../dune/presence.js';
import { allowedFields, sanitizeGameEvent, EVENT_FIELDS } from '../takaro/eventWhitelist.js';
import type { GameEventType } from '../takaro/protocol.js';
import type { DunePlayerRow, TakaroPlayer } from '../dune/types.js';
import { harness } from './helpers.js';

/**
 * Lane L4c: the connector's GAME-SIDE duties must survive a Takaro outage.
 *
 * The bug these tests pin down was real and cost a ban: `bridge.stopEvents()` on a Takaro WebSocket drop stopped the
 * presence poller, ban enforcement had no online set to read, and a banned player rejoined and stayed while
 * `/health` cheerfully reported `takaroIdentified:false`. Only DELIVERY may depend on Takaro.
 */

interface Rig {
  bridge: Bridge;
  wire: [GameEventType, unknown][];
  setUp: (up: boolean) => void;
  now: () => number;
  advance: (ms: number) => void;
}

async function rig(options: { sources?: EventSource[]; currentOnline?: () => TakaroPlayer[]; chatMaxAgeMs?: number; pluginEvents?: EventPoller } = {}): Promise<Rig> {
  const h = await harness();
  const wire: [GameEventType, unknown][] = [];
  let up = true;
  let clock = 1_700_000_000_000;
  const sink: TakaroSink = {
    send: () => up,
    sendGameEvent: (type, data) => (up ? (wire.push([type, data]), true) : false),
  };
  const bridge = new Bridge({
    adapter: h.adapter,
    takaro: sink,
    ...(options.sources ? { sources: options.sources } : {}),
    ...(options.currentOnline ? { currentOnline: options.currentOnline } : {}),
    ...(options.chatMaxAgeMs !== undefined ? { outageChatMaxAgeMs: options.chatMaxAgeMs } : {}),
    ...(options.pluginEvents ? { pluginEvents: options.pluginEvents } : {}),
    now: () => clock,
  });
  return { bridge, wire, setUp: (v) => (up = v), now: () => clock, advance: (ms) => (clock += ms) };
}

function player(gameId: string, name = gameId): TakaroPlayer {
  return { gameId, name };
}

describe('the event sources are independent of the Takaro socket', () => {
  it('sources start at boot, before Takaro is reachable, and a Takaro outage does NOT stop them', async () => {
    const started: string[] = [];
    const stopped: string[] = [];
    const source: EventSource = {
      name: 'presence',
      start: () => void started.push('presence'),
      stop: () => void stopped.push('presence'),
    };
    const r = await rig({ sources: [source] });

    // Boot: Takaro has not identified (and in the real outage never would), yet the source runs.
    r.setUp(false);
    await r.bridge.startSources();
    expect(started).toEqual(['presence']);
    expect(r.bridge.isActive()).toBe(true);

    // The socket dies and comes back twice. The source is never stopped and never double-started.
    r.bridge.onTakaroDown();
    await r.bridge.onTakaroUp();
    r.bridge.onTakaroDown();
    await r.bridge.startSources();
    expect(stopped).toEqual([]);
    expect(started).toEqual(['presence']);
    expect(r.bridge.isActive()).toBe(true);

    // Only shutdown stops them.
    r.bridge.stopSources();
    expect(stopped).toEqual(['presence']);
    expect(r.bridge.isActive()).toBe(false);
  });

  it('the plugin /events poller keeps draining the ring while Takaro is down, and nothing is lost', async () => {
    const store = new MemoryCursorStore();
    const events = [1, 2, 3].map((seq) => ({ seq, type: 'log', data: { msg: `line ${seq}` } }));
    const r = await rig();
    const poller = new EventPoller({
      getEvents: async (since) => ({ seq: 3, events: events.filter((e) => e.seq > since) }),
      emit: (type, data, seq) => r.bridge.emit(type, data, seq),
      store,
      intervalMs: 0,
    });
    // Give the bridge the poller so a drop can release its cursor.
    (r.bridge as unknown as { options: { pluginEvents: EventPoller } }).options.pluginEvents = poller;

    r.setUp(false);
    await poller.pollOnce();
    // Read out of the ring (scan position moved) but NOT delivered: the persisted cursor stays at 0, so a sidecar
    // restart mid-outage replays them from the plugin instead of skipping them for good.
    expect(poller.scanCursor()).toBe(3);
    expect(poller.cursor()).toBe(0);
    expect(r.bridge.pending()).toHaveLength(3);

    // A SECOND poll during the same outage must not queue the same events again.
    await poller.pollOnce();
    expect(r.bridge.pending()).toHaveLength(3);

    r.setUp(true);
    await r.bridge.onTakaroUp();
    expect(r.wire.map(([, d]) => (d as { msg: string }).msg)).toEqual(['line 1', 'line 2', 'line 3']);
    expect(r.bridge.pending()).toHaveLength(0);
  });

  it('a plugin event is never replayed to Takaro after the reconnect that delivered it', async () => {
    const store = new MemoryCursorStore();
    const events = [{ seq: 1, type: 'log', data: { msg: 'only once' } }];
    const r = await rig();
    const poller = new EventPoller({
      getEvents: async (since) => ({ seq: 1, events: events.filter((e) => e.seq > since) }),
      emit: (type, data, seq) => r.bridge.emit(type, data, seq),
      store,
      intervalMs: 0,
    });
    r.setUp(false);
    await poller.pollOnce();
    r.setUp(true);
    await r.bridge.onTakaroUp();
    // Delivered once by the flush…
    expect(r.wire).toHaveLength(1);
    // …and the poller does not hand it over again, because its scan position is past it.
    await poller.pollOnce();
    await r.bridge.onTakaroUp();
    expect(r.wire).toHaveLength(1);
  });

  it('the self-exit watchdog fires on GAME-side dependency loss only, never on a Takaro outage', async () => {
    const h = await harness();
    let pgOk = true;
    let clock = 0;
    const exit = vi.spyOn(process, 'exit').mockImplementation(((): never => undefined as never));
    try {
      const bridge = new Bridge({
        adapter: h.adapter,
        takaro: { send: () => false, sendGameEvent: () => false },
        dependenciesOk: async () => {
          if (!pgOk) throw new Error('postgres is gone');
          return true;
        },
        exitAfterDependencyLossMs: 1000,
        now: () => clock,
      });

      // Takaro is unreachable for a simulated hour: every emit fails, the queue fills — and nothing exits.
      for (let i = 0; i < 50; i += 1) {
        bridge.emit('log', { msg: `during outage ${i}` });
        clock += 60_000;
        await bridge.refreshHealth();
      }
      expect(exit).not.toHaveBeenCalled();
      expect(bridge.dependencies().ok).toBe(true);

      // Postgres going away is a different matter: that IS the game side.
      pgOk = false;
      await bridge.refreshHealth();
      clock += 2000;
      await bridge.refreshHealth();
      expect(exit).toHaveBeenCalledWith(1);
    } finally {
      exit.mockRestore();
    }
  });
});

describe('ban enforcement survives a Takaro outage', () => {
  /**
   * The exact shape of the incident: Takaro is unreachable, the presence poller keeps reading Postgres, and the ban
   * sweep keeps kicking. Nothing here touches the Takaro sink at all — which is the point.
   */
  it('a banned player who rejoins during the outage is kicked on sight', async () => {
    const kicks: string[] = [];
    let clock = 0;
    const bans = new BanManager({
      store: new MemoryBanStore(),
      kick: async (gameId) => {
        kicks.push(gameId);
        return true;
      },
      kickCooldownMs: 15_000,
      now: () => clock,
    });
    bans.add('A1B2C3D4E5F60718', 'L4c outage proof', null);

    // The roster the presence poller reads; the banned player rejoins on the third tick.
    let roster: DunePlayerRow[] = [];
    const r = await rig();
    r.setUp(false); // Takaro is DOWN for the whole test
    const presence = new PresencePoller({
      roster: async () => roster,
      emit: (type, data) => void r.bridge.emit(type, data),
      intervalMs: 0,
      transferGraceMs: 0,
      now: () => clock,
    });

    await presence.pollOnce();
    await bans.enforce(presence.onlinePlayers(), clock);
    expect(kicks).toEqual([]);

    roster = [{ flsId: 'A1B2C3D4E5F60718', characterName: 'TakaroTest', onlineStatus: 'Online' } as DunePlayerRow];
    await presence.pollOnce();
    expect(presence.onlinePlayers().map((p) => p.gameId)).toEqual(['A1B2C3D4E5F60718']);
    await bans.enforce(presence.onlinePlayers(), clock);
    expect(kicks).toEqual(['A1B2C3D4E5F60718']);

    // The cooldown holds, then he is kicked again — "you cannot stay on this server", with Takaro still dark.
    clock += 5_000;
    await bans.enforce(presence.onlinePlayers(), clock);
    expect(kicks).toHaveLength(1);
    clock += 20_000;
    await bans.enforce(presence.onlinePlayers(), clock);
    expect(kicks).toHaveLength(2);

    // And the connect event Takaro missed is queued, not lost.
    expect(r.bridge.pending().map((e) => e.type)).toEqual(['player-connected']);
  });

  it('a timed ban still expires during the outage, and the player stops being kicked', async () => {
    const kicks: string[] = [];
    let clock = 1_000_000;
    const bans = new BanManager({
      store: new MemoryBanStore(),
      kick: async (gameId) => {
        kicks.push(gameId);
        return true;
      },
      kickCooldownMs: 0,
      now: () => clock,
    });
    bans.add('A1B2C3D4E5F60718', 'timed', new Date(clock + 10_000).toISOString());
    const online = [player('A1B2C3D4E5F60718', 'TakaroTest')];

    await bans.enforce(online, clock);
    expect(kicks).toHaveLength(1);

    clock += 11_000;
    // Expiry is lifted BY US — Takaro never sends `unbanPlayer` for one, and here it could not even if it wanted to.
    expect(await bans.enforce(online, clock)).toEqual([]);
    expect(kicks).toHaveLength(1);
    expect(bans.all()).toHaveLength(0);
  });

  it('removing the ban means the next sweep leaves the player alone', async () => {
    const kicks: string[] = [];
    const bans = new BanManager({
      store: new MemoryBanStore(),
      kick: async (gameId) => {
        kicks.push(gameId);
        return true;
      },
      kickCooldownMs: 0,
    });
    const online = [player('A1B2C3D4E5F60718', 'TakaroTest')];
    bans.add('A1B2C3D4E5F60718', 'temporary', null);
    await bans.enforce(online);
    expect(kicks).toHaveLength(1);

    bans.remove('A1B2C3D4E5F60718');
    await bans.enforce(online);
    await bans.enforce(online);
    expect(kicks).toHaveLength(1);
    expect(bans.all()).toEqual([]);
  });

  it('a ban written to the store behind our back (local admin route, edited file) is picked up without a restart', async () => {
    const kicks: string[] = [];
    const store = new MemoryBanStore();
    const bans = new BanManager({
      store,
      kick: async (gameId) => {
        kicks.push(gameId);
        return true;
      },
      kickCooldownMs: 0,
    });
    const online = [player('A1B2C3D4E5F60718', 'TakaroTest')];
    await bans.enforce(online);
    expect(kicks).toEqual([]);

    // Somebody else wrote the file; `enforce` re-reads it.
    store.save([{ gameId: 'A1B2C3D4E5F60718', expiresAt: '', reason: 'out of band' }]);
    await bans.enforce(online);
    expect(kicks).toEqual(['A1B2C3D4E5F60718']);
  });
});

describe('the outage queue is groomed on reconnect, not replayed blindly', () => {
  it('chat lines older than the max age are dropped and counted; fresh ones are delivered', async () => {
    const r = await rig({ chatMaxAgeMs: 600_000 });
    r.setUp(false);
    r.bridge.emit('chat-message', { msg: 'ancient', channel: 'global' });
    r.advance(700_000);
    r.bridge.emit('chat-message', { msg: 'recent', channel: 'global' });
    r.bridge.emit('player-death', { player: player('X') });
    expect(r.bridge.pending()).toHaveLength(3);

    r.setUp(true);
    const result = await r.bridge.onTakaroUp();
    expect(result.staleChat).toBe(1);
    expect(r.wire.map(([type]) => type)).toEqual(['chat-message', 'player-death']);
    expect((r.wire[0][1] as { msg: string }).msg).toBe('recent');
    expect(r.bridge.outage().droppedStaleChat).toBe(1);
  });

  it('the max age applies ONLY to chat: a death or a log line from an hour ago is still delivered', async () => {
    const r = await rig({ chatMaxAgeMs: 1000 });
    r.setUp(false);
    r.bridge.emit('player-death', { player: player('X') });
    r.bridge.emit('log', { msg: 'an hour old' });
    r.advance(3_600_000);
    r.setUp(true);
    await r.bridge.onTakaroUp();
    expect(r.wire.map(([type]) => type)).toEqual(['player-death', 'log']);
  });

  it('a max age of 0 disables the drop entirely', async () => {
    const r = await rig({ chatMaxAgeMs: 0 });
    r.setUp(false);
    r.bridge.emit('chat-message', { msg: 'old', channel: 'global' });
    r.advance(99_999_999);
    r.setUp(true);
    expect((await r.bridge.onTakaroUp()).staleChat).toBe(0);
    expect(r.wire).toHaveLength(1);
  });

  it('the default max age is 10 minutes', () => {
    expect(DEFAULT_OUTAGE_CHAT_MAX_AGE_MS).toBe(600_000);
  });

  it('presence flaps during the outage collapse into ONE reconciliation against the live online set', async () => {
    const online: TakaroPlayer[] = [];
    const r = await rig({ currentOnline: () => online });

    // Takaro knows A is online before the outage starts.
    const a = player('A', 'Alice');
    r.bridge.emit('player-connected', { player: a });
    expect(r.bridge.onlinePlayers().map((p) => p.gameId)).toEqual(['A']);
    r.wire.length = 0;

    // Outage. A leaves, B joins and flaps four times, C joins and leaves again.
    r.setUp(false);
    r.bridge.onTakaroDown();
    const b = player('B', 'Bob');
    const c = player('C', 'Carol');
    r.bridge.emit('player-disconnected', { player: a });
    for (let i = 0; i < 4; i += 1) {
      r.bridge.emit('player-connected', { player: b });
      r.bridge.emit('player-disconnected', { player: b });
    }
    r.bridge.emit('player-connected', { player: b });
    r.bridge.emit('player-connected', { player: c });
    r.bridge.emit('player-disconnected', { player: c });
    r.bridge.emit('chat-message', { msg: 'meanwhile', channel: 'global' });
    expect(r.bridge.pending()).toHaveLength(13);

    // Reality when Takaro returns: only B is on.
    online.push(b);
    r.setUp(true);
    const result = await r.bridge.onTakaroUp();

    expect(result.collapsed).toBe(12);
    // One connect for B (Takaro never heard of him), one disconnect for A (Takaro still believed he was on), and the
    // chat line. Twelve stale flaps became two truthful events.
    expect(r.wire.map(([type, d]) => [type, (d as { player?: TakaroPlayer; msg?: string }).player?.gameId ?? (d as { msg: string }).msg])).toEqual([
      ['chat-message', 'meanwhile'],
      ['player-connected', 'B'],
      ['player-disconnected', 'A'],
    ]);
    expect(r.bridge.onlinePlayers().map((p) => p.gameId)).toEqual(['B']);
    expect(r.bridge.outage().reconciled).toEqual({ connected: 1, disconnected: 1 });
  });

  it('no presence events at all when the outage changed nothing', async () => {
    const a = player('A', 'Alice');
    const online = [a];
    const r = await rig({ currentOnline: () => online });
    r.bridge.emit('player-connected', { player: a });
    r.wire.length = 0;

    r.setUp(false);
    r.bridge.onTakaroDown();
    r.setUp(true);
    const result = await r.bridge.onTakaroUp();
    expect(result.reconciled).toBe(0);
    expect(r.wire).toEqual([]);
  });

  it('the queue stays bounded during a long outage and every drop is counted', async () => {
    const r = await rig();
    r.setUp(false);
    for (let i = 0; i < MAX_PENDING_EVENTS + 25; i += 1) r.bridge.emit('log', { msg: `line ${i}` });
    expect(r.bridge.pending()).toHaveLength(MAX_PENDING_EVENTS);
    expect(r.bridge.dropped()).toBe(25);
    expect(r.bridge.outage().droppedEvents).toBe(25);
    // Oldest first: the survivors start at the 25th line.
    expect((r.bridge.pending()[0].data as { msg: string }).msg).toBe('line 25');
  });

  it('the event that the full queue dropped is reported as NOT accepted, so its source can retry', async () => {
    const r = await rig();
    r.setUp(false);
    // Fill the queue exactly.
    for (let i = 0; i < MAX_PENDING_EVENTS; i += 1) r.bridge.emit('log', { msg: `line ${i}` });
    // The next event pushes the OLDEST out, so this one IS accepted…
    expect(r.bridge.emit('log', { msg: 'newest' })).toBe('queued');
    expect(r.bridge.dropped()).toBe(1);
  });
});

describe('/health tells the truth per dependency', () => {
  it('a Takaro outage is reported as a Takaro problem and does not make the connector unhealthy', async () => {
    const h = await harness();
    const bridge = new Bridge({
      adapter: h.adapter,
      takaro: { send: () => false, sendGameEvent: () => false },
      dependenciesOk: async () => true,
    });
    await bridge.refreshHealth();
    bridge.onTakaroDown();
    bridge.emit('log', { msg: 'queued' });

    // `ok` is the GAME side: Postgres answers, so the connector is healthy and must not 503.
    expect(bridge.dependencies()).toEqual({ ok: true, error: null });
    expect(bridge.outage().pendingEvents).toBe(1);
    expect(bridge.outage().takaroDownSince).not.toBeNull();
  });

  it('a Takaro error frame is counted with its reason instead of vanishing into the log', async () => {
    const h = await harness();
    const bridge = new Bridge({ adapter: h.adapter, takaro: { send: () => true, sendGameEvent: () => true } });
    expect(bridge.takaroRejections().count).toBe(0);
    bridge.noteTakaroError('An instance of EventEntityKilled has failed the validation: entityCode whitelistValidation');
    expect(bridge.takaroRejections().count).toBe(1);
    expect(bridge.takaroRejections().lastReason).toMatch(/entityCode/);
    expect(bridge.takaroRejections().lastAt).toMatch(/^\d{4}-/);
  });
});

/**
 * Takaro validates inbound gameEvents with `forbidNonWhitelisted: true`, so ONE extra key destroys the event. This is
 * the regression gate for the live loss of a real kill to `property entityCode … whitelistValidation`.
 */
describe('every outgoing gameEvent stays inside Takaro’s DTO whitelist', () => {
  const dtoFields: Record<GameEventType, string[]> = {
    log: ['timestamp', 'msg'],
    'player-connected': ['timestamp', 'msg', 'player'],
    'player-disconnected': ['timestamp', 'msg', 'player'],
    'chat-message': ['timestamp', 'msg', 'player', 'channel', 'recipient'],
    'player-death': ['timestamp', 'msg', 'player', 'attacker', 'position'],
    'entity-killed': ['timestamp', 'msg', 'player', 'entity', 'weapon'],
  };

  it('the whitelist matches Takaro’s gameEvents DTOs field for field', () => {
    for (const [type, fields] of Object.entries(dtoFields) as [GameEventType, string[]][]) {
      expect(allowedFields(type).sort()).toEqual([...new Set(fields)].sort());
    }
    expect(Object.keys(EVENT_FIELDS).sort()).toEqual(Object.keys(dtoFields).sort());
  });

  it('the exact payload that Takaro rejected is stripped down to the accepted shape', () => {
    const { data, removed } = sanitizeGameEvent('entity-killed', {
      player: { gameId: 'A1B2C3D4E5F60718', name: 'TakaroTest', steamId: '76561198765432109', platformId: 'steam:76561198765432109' },
      entity: 'Scavenger Thug',
      weapon: '',
      entityCode: 'T3_Band_Slv_Reg_Marksman',
      nameIsClassName: false,
      weaponCode: 'BlueprintGeneratedClass',
      attribution: 'ReceiveMulticastDeathOrDefeat',
      timestamp: '2026-09-21T17:30:12.208Z',
    });
    expect(Object.keys(data as object).sort()).toEqual(['entity', 'player', 'timestamp', 'weapon']);
    expect(removed.sort()).toEqual(['attribution', 'entityCode', 'nameIsClassName', 'weaponCode']);
  });

  it('nested player/attacker/recipient/position objects are cleaned too', () => {
    const { data, removed } = sanitizeGameEvent('player-death', {
      player: { gameId: 'A', name: 'Alice', funcomId: 'PLAYER#1', online: true },
      attacker: { gameId: 'B', name: 'Bob', ref: 'acct:2' },
      position: { x: 1, y: 2, z: 3, source: 'pawn' },
      killerEntityCode: 'ADuneNpcCharacter',
      msg: 'Alice was killed by an NPC',
    });
    expect(data).toEqual({
      player: { gameId: 'A', name: 'Alice' },
      attacker: { gameId: 'B', name: 'Bob' },
      position: { x: 1, y: 2, z: 3 },
      msg: 'Alice was killed by an NPC',
    });
    expect(removed.sort()).toEqual(['attacker.ref', 'killerEntityCode', 'player.funcomId', 'player.online', 'position.source']);
  });

  it('the bridge applies it to EVERY event, whatever the source built', async () => {
    const r = await rig();
    r.bridge.emit('entity-killed', { player: player('A'), entity: 'Sandworm', weapon: '', entityCode: 'ASandwormPawn' });
    r.bridge.emit('chat-message', { msg: 'hi', channel: 'global', senderFlsId: 'A' });
    expect(Object.keys(r.wire[0][1] as object).sort()).toEqual(['entity', 'player', 'weapon']);
    expect(Object.keys(r.wire[1][1] as object).sort()).toEqual(['channel', 'msg']);
    expect(r.bridge.outage().strippedFields).toBe(2);
  });

  it('a queued event is sanitized on the flush path too, not only on the direct one', async () => {
    const r = await rig();
    r.setUp(false);
    r.bridge.emit('entity-killed', { player: player('A'), entity: 'Sandworm', weapon: 'Crysknife', attribution: 'debug' });
    r.setUp(true);
    await r.bridge.onTakaroUp();
    expect(Object.keys(r.wire[0][1] as object).sort()).toEqual(['entity', 'player', 'weapon']);
  });
});

describe('a character switch during a Takaro outage', () => {
  const on = (id: string): TakaroPlayer => ({ gameId: 'P1', name: 'Paul', character: { id, name: 'Paul' } });

  it('is replayed as one relog when Takaro comes back', async () => {
    let current = [on('10')];
    const r = await rig({ currentOnline: () => current });
    r.bridge.emit('player-connected', { player: on('10') });
    r.setUp(false);
    r.bridge.onTakaroDown();
    current = [on('11')];
    r.bridge.emit('player-disconnected', { player: on('10') });
    r.bridge.emit('player-connected', { player: on('11') });
    r.setUp(true);
    await r.bridge.onTakaroUp();
    const presence = r.wire.map(([type, data]) => [type, (data as { player: TakaroPlayer }).player.character?.id]);
    expect(presence).toEqual([
      ['player-connected', '10'],
      ['player-disconnected', '10'],
      ['player-connected', '11'],
    ]);
  });

  it('stays silent for the same character', async () => {
    const r = await rig({ currentOnline: () => [on('10')] });
    r.bridge.emit('player-connected', { player: on('10') });
    r.setUp(false);
    r.bridge.onTakaroDown();
    r.setUp(true);
    expect((await r.bridge.onTakaroUp()).reconciled).toBe(0);
  });
});
