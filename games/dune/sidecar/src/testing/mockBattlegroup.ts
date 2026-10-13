import { EventEmitter } from 'node:events';
import type { Queryable } from '../dune/pg.js';
import { OPTIONAL_COLUMNS } from '../dune/pg.js';
import type { AmqpChannel, AmqpConnection, AmqpMessage } from '../dune/rmq.js';
import type { ExecRunner, PublishChannel } from '../dune/gm.js';
import type { DuneInventoryRow, DunePlayerRow } from '../dune/types.js';

/**
 * An in-memory stand-in for a Dune battlegroup: the Postgres reads, the game RabbitMQ, and the broker-exec channel.
 *
 * It sits at the `Queryable` / `AmqpChannel` / `ExecRunner` boundaries rather than emulating SQL or AMQP, because
 * what the tests need to pin down is the connector's *behaviour* — which rows it turns into which events, which
 * binds and publishes it performs in which order, and what it puts on an exec argv — not Postgres's parser. The real
 * SQL text is exercised against the live rig; everything else is exercised here, with no server running.
 */
export interface MockPlayer extends DunePlayerRow {
  inventory?: DuneInventoryRow[];
}

export interface RecordedPublish {
  exchange: string;
  routingKey: string;
  body: string;
  options: Record<string, unknown>;
}

export interface RecordedBind {
  action: 'bind' | 'unbind';
  queue: string;
  exchange: string;
  routingKey: string;
}

export class MockBattlegroup {
  players: MockPlayer[] = [];
  /** Columns the fake schema "has"; defaults to every optional column, so the happy path uses the full queries. */
  columns: Record<string, string[]> = Object.fromEntries(Object.entries(OPTIONAL_COLUMNS).map(([t, c]) => [t, [...c]]));
  /** Partition readiness for `testReachability`. */
  partitions = { expected: 1, readyAlive: 1 };
  /** Every SQL statement the connector issued, for assertions about parameterisation. */
  readonly queries: { sql: string; params: unknown[] }[] = [];
  /** Set to make the next query throw (dependency-loss tests). */
  failQueries: string | null = null;

  readonly publishes: RecordedPublish[] = [];
  readonly binds: RecordedBind[] = [];
  /**
   * Exchanges the fake broker has declared. The default is what `text-router` alone declares on a battlegroup whose
   * map process has NOT joined the broker yet — the state the rig was actually in at first boot — plus
   * `chat.map`, which the map server declares once it is on. Tests that want the map-absent case delete it.
   */
  exchanges = new Set(['heartbeats', 'chat.intercept', 'chat.whispers', 'chat.map', 'chat.proximity', 'chat.faction.1', 'notifications']);
  /** Every passive exchange probe the connector made, and whether the fake broker answered yes. */
  readonly exchangeChecks: { exchange: string; ok: boolean }[] = [];
  /** Channels opened via `createChannel`, so a test can assert the probe used a throwaway one. */
  channelsOpened = 0;
  /** Channels the fake broker killed because a passive declare 404'd. */
  channelsKilled = 0;
  readonly execCalls: { argv: string[]; stdin: string }[] = [];
  /** Erlang `eval` outcome the fake broker prints; `ok` means the publish succeeded. */
  execResult: { code: number; stdout: string; stderr: string } | null = null;

  private readonly consumers: ((msg: AmqpMessage | null) => void)[] = [];
  private consumerCounter = 0;

  // --- Postgres ----------------------------------------------------------

  /**
   * A tiny query router. It recognises the connector's four shapes (schema probe, readiness count, roster, inventory)
   * by the tables they name and answers from the in-memory rows.
   */
  get db(): Queryable {
    return {
      query: async <T>(sql: string, params: unknown[] = []): Promise<{ rows: T[] }> => {
        this.queries.push({ sql, params });
        if (this.failQueries) throw new Error(this.failQueries);
        const rows = this.route(sql, params) as T[];
        return { rows };
      },
      end: async () => {},
    };
  }

  private route(sql: string, params: unknown[]): Record<string, unknown>[] {
    if (sql.includes('information_schema.columns')) {
      const wanted = new Set((params[1] as string[] | undefined) ?? Object.keys(this.columns));
      const out: Record<string, unknown>[] = [];
      for (const [table, cols] of Object.entries(this.columns)) {
        if (!wanted.has(table)) continue;
        for (const column of cols) out.push({ table_name: table, column_name: column });
      }
      return out;
    }
    if (sql.includes('ready_alive')) {
      return [{ expected: this.partitions.expected, ready_alive: this.partitions.readyAlive }];
    }
    if (sql.includes('FROM "dune"."items"') || sql.includes('JOIN "dune"."items"')) {
      const accountId = Number(params[0]);
      // No second parameter = the read-back's "every inventory this pawn owns" query (`inventory({allTypes:true})`).
      const types = params.length > 1 ? new Set((params[1] as number[]) ?? []) : null;
      const player = this.players.find((p) => p.accountId === accountId);
      return (player?.inventory ?? [])
        .filter((i) => types === null || types.has(i.inventoryType ?? 0))
        .map((i) => ({
          template_id: i.templateId,
          stack_size: i.stackSize,
          quality_level: i.qualityLevel ?? null,
          position_index: i.positionIndex ?? null,
          inventory_type: i.inventoryType ?? 0,
        }));
    }
    if (sql.includes('FROM "dune"."world_partition" wp')) {
      return [{ partition_id: 1, name: 'Hagga Basin', map: 'survival_1' }];
    }
    if (sql.includes('"markers"')) return [];
    if (sql.includes('"player_state" ps')) {
      const onlineOnly = sql.includes("online_status::text = 'Online'");
      const needle = typeof params[0] === 'string' ? params[0].toLowerCase() : null;
      return this.players
        .filter((p) => !onlineOnly || (p.onlineStatus ?? '').toLowerCase() === 'online')
        .filter((p) => !needle || matches(p, needle))
        .map((p) => toRow(p, new Set(this.columns.actors ?? []), new Set(this.columns.player_state ?? [])));
    }
    if (sql.trim() === 'SELECT 1') return [{ '?column?': 1 }];
    return [];
  }

  // --- RabbitMQ ----------------------------------------------------------

  private channelInstance: (AmqpChannel & PublishChannel) | null = null;

  /** Memoised: the connector (and a test spy) must see the same channel object every time. */
  get channel(): AmqpChannel & PublishChannel {
    if (this.channelInstance) return this.channelInstance;
    const self = this;
    this.channelInstance = {
      async assertQueue() {
        return {};
      },
      async bindQueue(queue: string, exchange: string, routingKey: string) {
        self.binds.push({ action: 'bind', queue, exchange, routingKey });
        return {};
      },
      async unbindQueue(queue: string, exchange: string, routingKey: string) {
        self.binds.push({ action: 'unbind', queue, exchange, routingKey });
        return {};
      },
      async consume(_queue: string, onMessage: (msg: AmqpMessage | null) => void) {
        self.consumers.push(onMessage);
        self.consumerCounter += 1;
        return { consumerTag: `mock-${self.consumerCounter}` };
      },
      ack() {},
      publish(exchange: string, routingKey: string, content: Buffer, options: Record<string, unknown> = {}) {
        self.publishes.push({ exchange, routingKey, body: content.toString('utf8'), options });
        return true;
      },
      async prefetch() {
        return {};
      },
      async checkExchange(exchange: string) {
        return self.passiveDeclare(exchange, 'main');
      },
      async close() {},
    };
    return this.channelInstance;
  }

  /**
   * A passive `exchange.declare`, with the real broker's behaviour: an unknown exchange answers 404 AND closes the
   * channel it was asked on. Doing that to the connector's MAIN channel would take the chat consumer and the GM
   * publisher down with it, which is precisely the failure the throwaway-channel design exists to avoid — so the
   * mock records it and the test asserts it never happened.
   */
  private passiveDeclare(exchange: string, kind: 'main' | 'probe'): Record<string, unknown> {
    const ok = this.exchanges.has(exchange);
    this.exchangeChecks.push({ exchange, ok });
    if (!ok) {
      this.channelsKilled += 1;
      if (kind === 'main') this.mainChannelKilled = true;
      throw new Error(`Channel closed by server: 404 (NOT-FOUND) with message "NOT_FOUND - no exchange '${exchange}' in vhost '/'"`);
    }
    return {};
  }

  /** True if a passive declare was ever run on the connector's long-lived channel. It must stay false. */
  mainChannelKilled = false;

  /** A fresh channel, as `connection.createChannel()` hands out for a one-shot probe. */
  private probeChannel(): AmqpChannel & PublishChannel {
    const self = this;
    return {
      async assertQueue() {
        return {};
      },
      async bindQueue() {
        return {};
      },
      async unbindQueue() {
        return {};
      },
      async consume() {
        return { consumerTag: 'probe' };
      },
      ack() {},
      publish() {
        return true;
      },
      async checkExchange(exchange: string) {
        return self.passiveDeclare(exchange, 'probe');
      },
      async close() {},
    };
  }

  get connection(): AmqpConnection {
    const emitter = new EventEmitter();
    const self = this;
    return {
      createChannel: async () => {
        self.channelsOpened += 1;
        // The first channel is the connector's long-lived one; every later one is a throwaway.
        return self.channelsOpened === 1 ? self.channel : self.probeChannel();
      },
      close: async () => {
        emitter.emit('close');
      },
      on: (event, listener) => emitter.on(event, listener as (...args: unknown[]) => void),
    };
  }

  connect = async (): Promise<AmqpConnection> => this.connection;

  /** Pushes a `chat.intercept` delivery at the connector exactly as the broker would. */
  deliverChat(body: unknown, properties: AmqpMessage['properties'] = {}, routingKey = 'chat.map'): void {
    const content = Buffer.from(typeof body === 'string' ? body : JSON.stringify(body), 'utf8');
    const msg: AmqpMessage = { content, fields: { routingKey }, properties };
    for (const consumer of this.consumers) consumer(msg);
  }

  /** Builds the outer/inner body shape the game publishes on `chat.intercept`. */
  static chatBody(
    inner: Record<string, unknown>,
    opts: { contentKey?: 'content' | 'Content'; type?: string } = {},
  ): Record<string, unknown> {
    return { [opts.contentKey ?? 'content']: JSON.stringify(inner), Type: opts.type ?? 'TextChat' };
  }

  // --- exec --------------------------------------------------------------

  get execRunner(): ExecRunner {
    return async (argv, stdin) => {
      this.execCalls.push({ argv, stdin });
      return this.execResult ?? { code: 0, stdout: 'TAKARO_PUBLISH result=ok exchange=heartbeats routing=notifications label=test\n', stderr: '' };
    };
  }

  // --- convenience -------------------------------------------------------

  setOnline(flsId: string, online: boolean): void {
    const player = this.players.find((p) => p.flsId === flsId);
    if (player) player.onlineStatus = online ? 'Online' : 'Offline';
  }

  setLifeState(flsId: string, state: string): void {
    const player = this.players.find((p) => p.flsId === flsId);
    if (player) player.lifeState = state;
  }
}

/**
 * One roster row, shaped like the real schema rather than like a convenient guess. Every value below is the type the
 * shipped `01_Dune.sql` gives it:
 *
 * | fixture field | real column | real type |
 * |---|---|---|
 * | `flsId` | `accounts."user"` (view over `encrypted_accounts."user"`) | `TEXT NOT NULL UNIQUE`, 16 hex in practice |
 * | `funcomId` | `accounts.funcom_id` (`decrypt_user_data(encrypted_funcom_id)`) | `TEXT` |
 * | `platformId` / `platformName` | `accounts.platform_id` / `.platform_name` | `TEXT` — SteamID64 when steam |
 * | `accountId` | `encrypted_player_state.account_id` | `BIGINT` |
 * | `characterName` | `player_state.character_name` (decrypted) | `TEXT` |
 * | `onlineStatus` | `player_state.online_status` | `PlayerConnectionStatus` = Offline \| **LoggingOut** \| Online |
 * | `lifeState` | `player_state.life_state` | `PlayerLifeState` = Alive \| Dead \| DeadByCoriolis \| DeadBySandworm |
 * | `position` | `(actors.transform).location` | composite `Transform(location Vector, rotation Quaternion)` |
 * | `deathLocation` | `(player_state.death_location).location` | composite `DeathLocation(location Vector, map Text, dimension Integer)` |
 * | `serverId` | `player_state.server_id` | **`TEXT`** (`Survival_1`), FK to `farm_state.server_id` — not a number |
 * | `partitionId` | `actors.partition_id` | `BIGINT` → `world_partition.partition_id` |
 * | `map` | `world_partition.label` / `actors.map` | `TEXT` (the rig's Survival partition is labelled `Abbir`) |
 * | `worldMap` | `world_partition.map` | `TEXT` (`Survival_1`, `DeepDesert_1`, `SH_Arrakeen`, …) |
 * | `reconnectGraceEnd` | `player_state.reconnect_grace_period_end` | `TIMESTAMP` (UTC, nullable) |
 * | `transferCount` | `player_state.transfer_count` | `INTEGER NOT NULL DEFAULT 0` |
 * | `inventory[].templateId` | `items.template_id` | `TEXT NOT NULL` |
 * | `inventory[].stackSize` | `items.stack_size` | `BIGINT NOT NULL CHECK (> 0)` |
 * | `inventory[].qualityLevel` | `items.quality_level` | `BIGINT NOT NULL DEFAULT 0` |
 * | `inventory[].inventoryType` | `inventories.inventory_type` | `INTEGER` (0 = backpack) |
 */
export function mockPlayer(overrides: Partial<MockPlayer> = {}): MockPlayer {
  return {
    flsId: '6FF6498F4074E3DE',
    funcomId: 'PLAYER#12345',
    platformId: '76561190000000001',
    platformName: 'steam',
    accountId: 1,
    characterName: 'Tester',
    onlineStatus: 'Online',
    lifeState: 'Alive',
    position: { x: 21723, y: 219189, z: 1200 },
    serverId: 'Survival_1',
    partitionId: 1,
    map: 'Abbir',
    worldMap: 'Survival_1',
    reconnectGraceEnd: null,
    transferCount: 0,
    inventory: [{ templateId: 'PowerPack2', stackSize: 2, qualityLevel: 1, positionIndex: 0, inventoryType: 0 }],
    ...overrides,
  };
}

function matches(p: MockPlayer, needle: string): boolean {
  return [p.flsId, p.funcomId, p.platformId, p.characterName].some((v) => (v ?? '').toLowerCase() === needle);
}

/** Mirrors the connector's column probing: a column the fake schema lacks comes back NULL, as Postgres would. */
function toRow(p: MockPlayer, actorColumns: Set<string>, playerStateColumns: Set<string>): Record<string, unknown> {
  const position = actorColumns.has('transform') ? p.position : null;
  const death = playerStateColumns.has('death_location') ? p.deathLocation : null;
  return {
    fls_id: p.flsId,
    funcom_id: p.funcomId ?? '',
    platform_id: p.platformId ?? '',
    platform_name: p.platformName ?? '',
    account_id: p.accountId ?? null,
    character_name: p.characterName ?? '',
    online_status: p.onlineStatus ?? '',
    server_id: p.serverId ?? '',
    partition_id: p.partitionId ?? null,
    map: p.map ?? '',
    world_map: p.worldMap ?? '',
    last_seen: p.lastSeen ?? '',
    reconnect_grace_end: playerStateColumns.has('reconnect_grace_period_end') ? (p.reconnectGraceEnd ?? '') : '',
    transfer_count: playerStateColumns.has('transfer_count') ? (p.transferCount ?? null) : null,
    life_state: playerStateColumns.has('life_state') ? (p.lifeState ?? '') : '',
    x: position?.x ?? null,
    y: position?.y ?? null,
    z: position?.z ?? null,
    death_x: death?.x ?? null,
    death_y: death?.y ?? null,
    death_z: death?.z ?? null,
  };
}
