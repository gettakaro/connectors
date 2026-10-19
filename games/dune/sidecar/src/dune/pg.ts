import { logger } from '../logger.js';
import { num, str } from './identity.js';
import type { DuneInventoryRow, DunePlayerRow, DuneReachability, Position, TakaroLocation } from './types.js';

/**
 * The only thing `pg.ts` needs from a driver. Tests inject `testing/mockBattlegroup`'s fake instead of `pg.Pool`, and
 * the production path wraps a real pool in `createPgPool()` — so no test ever needs a live Postgres.
 */
export interface Queryable {
  query<T = Record<string, unknown>>(sql: string, params?: unknown[]): Promise<{ rows: T[] }>;
  end?(): Promise<void>;
}

export interface DunePgOptions {
  schema?: string;
  /** Inventory types read by `getInventory` (0 = backpack, plus the configured gear containers). */
  inventoryTypes?: number[];
  /** Column of `player_state` that holds the pawn actor id. */
  pawnColumn?: string;
}

/**
 * Optional columns the connector uses when the build has them and simply omits when it does not. Probing
 * `information_schema` once at start is what lets the same sidecar talk to `dune` and `dune_sb_1_4_0_0` without a
 * per-build fork: every query below is assembled from the columns that actually exist.
 */
export const OPTIONAL_COLUMNS = {
  player_state: [
    'life_state',
    'death_location',
    'character_name',
    'online_status',
    'server_id',
    'last_avatar_activity',
    'last_login_time',
    'player_pawn_id',
    'account_id',
    'reconnect_grace_period_end',
    'transfer_count',
  ],
  accounts: ['funcom_id', 'platform_id', 'platform_name', 'user'],
  encrypted_accounts: ['user'],
  actors: ['transform', 'map', 'partition_id', 'dimension_index', 'class'],
  world_partition: ['partition_id', 'server_id', 'map', 'label', 'blocked', 'dimension_index'],
  farm_state: ['server_id', 'alive', 'ready', 'connected_players', 'map'],
  inventories: ['inventory_type', 'actor_id'],
  items: ['template_id', 'stack_size', 'quality_level', 'position_index', 'inventory_id'],
  /**
   * The shipped `markers` table is `(marker_hash_id, dimension_index, marker_type, position Vector, payload jsonb,
   * map_name_id)` — it has **no name and no transform**, only a dev-style `marker_type` and a numeric hash id. The
   * legacy `name`/`transform` probe below therefore comes back false on a stock build and `listLocations` answers
   * with the world partitions alone, which is the honest outcome: a marker we cannot give a player-facing name is
   * not something to ship (memory: catalogue-human-names).
   */
  markers: ['id', 'name', 'transform', 'map', 'marker_type', 'position', 'map_name_id'],
} as const;

export type TableName = keyof typeof OPTIONAL_COLUMNS;

const IDENT_RE = /^[a-z_][a-z0-9_]*$/;

/** `dune_sb_1_5_15_0` -> [1, 5, 15, 0]; null for any other name. */
function branchVersion(name: string): number[] | null {
  const m = /^dune_sb_(\d+(?:_\d+)*)$/.exec(name);
  return m ? m[1].split('_').map(Number) : null;
}

/**
 * The battlegroup names its database after the server's code branch (`dune_sb_1_5_3_0`, then `dune_sb_1_5_15_0` from
 * Steam build 25936522) and an update moves the game to the new name. A `DUNE_PG_URL` left on the old name still
 * connects and reads a frozen roster, so the newest branch database other than the current one is reported.
 */
export function newerBranchDatabase(current: string, names: string[]): string | null {
  const own = branchVersion(current);
  if (!own) return null;
  let best: { name: string; v: number[] } | null = null;
  for (const name of names) {
    const v = branchVersion(name);
    if (!v || name === current) continue;
    const cmp = (a: number[], b: number[]) => {
      for (let i = 0; i < Math.max(a.length, b.length); i++) if ((a[i] ?? 0) !== (b[i] ?? 0)) return (a[i] ?? 0) - (b[i] ?? 0);
      return 0;
    };
    if (cmp(v, own) > 0 && (!best || cmp(v, best.v) > 0)) best = { name, v };
  }
  return best?.name ?? null;
}


/** Postgres identifiers are never parameterisable, so every one we interpolate must first be proven to be an identifier. */
export function quoteIdent(name: string): string {
  if (!IDENT_RE.test(name)) throw new Error(`Refusing to interpolate '${name}' as a SQL identifier`);
  return `"${name}"`;
}

/**
 * Read-only access to the battlegroup's Postgres.
 *
 * v1 performs NO writes at all. The game overwrites rows for online players from its own in-memory state, so a DB
 * write is either ignored or fights the server; every mutation in this connector goes through a GM command instead
 * (campaign plan, "Online DB writes get overwritten").
 */
export class DunePg {
  private readonly schema: string;
  private readonly inventoryTypes: number[];
  private columns = new Map<string, Set<string>>();
  private probed = false;

  constructor(
    private readonly db: Queryable,
    private readonly options: DunePgOptions = {},
  ) {
    this.schema = options.schema ?? 'dune';
    if (!IDENT_RE.test(this.schema)) throw new Error(`DUNE_PG_SCHEMA '${this.schema}' is not a plain identifier`);
    // See `REPORTED_INVENTORY_TYPES` in config.ts for what each type holds and why only these three are reported.
    this.inventoryTypes = options.inventoryTypes ?? [0, 1, 15];
  }

  private t(table: string): string {
    return `${quoteIdent(this.schema)}.${quoteIdent(table)}`;
  }

  /** Columns this build actually has, as probed. Empty before `probe()` — every caller degrades, none throws. */
  has(table: TableName | string, column: string): boolean {
    return this.columns.get(table)?.has(column) ?? false;
  }

  probedColumns(): Record<string, string[]> {
    return Object.fromEntries([...this.columns].map(([t, c]) => [t, [...c].sort()]));
  }

  isProbed(): boolean {
    return this.probed;
  }

  /**
   * One information_schema round trip that records which optional columns exist. Called at start and again after a
   * reconnect; a failure leaves the connector in "nothing probed" state, where every optional field is simply absent.
   */
  async probe(): Promise<void> {
    const tables = Object.keys(OPTIONAL_COLUMNS);
    const { rows } = await this.db.query<{ table_name: string; column_name: string }>(
      `SELECT table_name, column_name
         FROM information_schema.columns
        WHERE table_schema = $1 AND table_name = ANY($2::text[])`,
      [this.schema, tables],
    );
    const next = new Map<string, Set<string>>();
    for (const row of rows) {
      const set = next.get(row.table_name) ?? new Set<string>();
      set.add(row.column_name);
      next.set(row.table_name, set);
    }
    this.columns = next;
    this.probed = true;
    logger.debug(`Postgres schema probe: ${JSON.stringify(this.probedColumns())}`);
    await this.checkDatabaseBranch();
  }

  /** Warns when a newer branch database than ours exists. Advisory: a failed lookup never fails the probe. */
  private async checkDatabaseBranch(): Promise<void> {
    try {
      const { rows } = await this.db.query<{ current: string; names: string[] | null }>(
        `SELECT current_database() AS current,
                (SELECT array_agg(datname::text) FROM pg_database WHERE datname LIKE 'dune\\_sb\\_%') AS names`,
      );
      const current = rows[0]?.current;
      const newer = current ? newerBranchDatabase(current, rows[0]?.names ?? []) : null;
      if (newer) {
        logger.warn(
          `Postgres: connected to '${current}', but the newer branch database '${newer}' exists. After a game update ` +
            `the battlegroup uses the new one, so this connector reads stale data until DUNE_PG_URL names '${newer}'.`,
        );
      }
    } catch (err) {
      logger.debug(`Postgres: branch database check skipped (${(err as Error).message})`);
    }
  }

  /**
   * `testReachability`: the game's own readiness signal. A partition row is usable when its farm is both `alive` and
   * `ready`; "the query succeeded but nothing is ready" is a real, reportable not-connectable state, not an error.
   */
  async reachability(): Promise<DuneReachability> {
    if (!this.has('farm_state', 'server_id') || !this.has('world_partition', 'server_id')) {
      // Without the readiness tables all we can honestly claim is that the database answers.
      await this.db.query('SELECT 1');
      return { connectable: true, reason: 'farm_state/world_partition not present in this schema; reported DB reachability only' };
    }
    const ready = this.has('farm_state', 'ready') ? 'COALESCE(fs.ready, false)' : 'true';
    const alive = this.has('farm_state', 'alive') ? 'COALESCE(fs.alive, false)' : 'true';
    const blocked = this.has('world_partition', 'blocked') ? 'COALESCE(wp.blocked, false)' : 'false';
    const { rows } = await this.db.query<{ expected: string | number; ready_alive: string | number }>(
      `SELECT count(*)::int8 AS expected,
              count(*) FILTER (WHERE ${ready} AND ${alive} AND NOT ${blocked})::int8 AS ready_alive
         FROM ${this.t('world_partition')} wp
         LEFT JOIN ${this.t('farm_state')} fs ON fs.server_id = wp.server_id`,
    );
    const expected = num(rows[0]?.expected) ?? 0;
    const readyAlive = num(rows[0]?.ready_alive) ?? 0;
    return {
      connectable: readyAlive > 0,
      reason: readyAlive > 0 ? null : `no battlegroup partition is alive and ready (${readyAlive}/${expected})`,
      readyPartitions: readyAlive,
      expectedPartitions: expected,
    };
  }

  /**
   * The roster. `player_state` is the spine; the account tables carry identity, the pawn actor carries the
   * last-SAVED position, and `world_partition` names the map.
   *
   * **Which account table is canonical for the FLS id** (settled from the shipped schema, `01_Dune.sql`):
   * `encrypted_accounts` is the base TABLE and holds `"user" TEXT NOT NULL UNIQUE`; `accounts` is a VIEW over it —
   * `select "id", "user", decrypt_user_data(encrypted_funcom_id) as funcom_id, …` — so `"user"` is literally the
   * same column seen through the view, and the view is the only place `funcom_id` / `platform_id` exist in the
   * clear. The game's own code reads it through the view (`is_player_offline`, `flag_player_as_cheater`,
   * `get_players_info` all say `accounts."user"`), so **`accounts."user"` is preferred** and
   * `encrypted_accounts."user"` is the fallback for a build that ever drops the view. Both are joined and COALESCEd
   * in that order, and every reference is table-qualified because `"user"` exists on both.
   *
   * Note the view also filters: `player_state` selects `from encrypted_player_state where character_state = 'Active'`,
   * so deleted characters never reach the roster.
   */
  async roster(options: { onlineOnly?: boolean; flsId?: string; limit?: number } = {}): Promise<DunePlayerRow[]> {
    const hasEnc = this.has('encrypted_accounts', 'user');
    const hasAccUser = this.has('accounts', 'user');
    if (!hasEnc && !hasAccUser) {
      throw new Error(`Neither ${this.schema}.encrypted_accounts."user" nor ${this.schema}.accounts."user" exists; cannot resolve FLS ids`);
    }
    // `accounts` first: it is the view the game's own stored procedures read the FLS id through.
    const flsExpr = [hasAccUser ? `NULLIF(acct."user"::text, '')` : null, hasEnc ? `NULLIF(enc."user"::text, '')` : null]
      .filter(Boolean)
      .join(', ');

    const select: string[] = [`COALESCE(${flsExpr}, '') AS fls_id`];
    select.push(this.has('accounts', 'funcom_id') ? `COALESCE(acct.funcom_id::text, '') AS funcom_id` : `'' AS funcom_id`);
    select.push(this.has('accounts', 'platform_id') ? `COALESCE(acct.platform_id::text, '') AS platform_id` : `'' AS platform_id`);
    select.push(this.has('accounts', 'platform_name') ? `COALESCE(acct.platform_name::text, '') AS platform_name` : `'' AS platform_name`);
    select.push(this.has('player_state', 'account_id') ? `ps.account_id::int8 AS account_id` : `NULL::int8 AS account_id`);
    select.push(this.has('player_state', 'character_name') ? `COALESCE(ps.character_name, '') AS character_name` : `'' AS character_name`);
    select.push(this.has('player_state', 'online_status') ? `COALESCE(ps.online_status::text, '') AS online_status` : `'' AS online_status`);
    select.push(this.has('player_state', 'life_state') ? `COALESCE(ps.life_state::text, '') AS life_state` : `'' AS life_state`);
    // `server_id` is TEXT in the shipped schema (`Survival_1`), NOT an integer: casting it to int8 made the whole
    // roster query fail on the real database.
    select.push(this.has('player_state', 'server_id') ? `COALESCE(ps.server_id::text, '') AS server_id` : `'' AS server_id`);
    select.push(
      this.has('player_state', 'reconnect_grace_period_end')
        ? `COALESCE(to_char(ps.reconnect_grace_period_end, 'YYYY-MM-DD"T"HH24:MI:SS"Z"'), '') AS reconnect_grace_end`
        : `'' AS reconnect_grace_end`,
    );
    select.push(this.has('player_state', 'transfer_count') ? `ps.transfer_count::int8 AS transfer_count` : `NULL::int8 AS transfer_count`);
    if (this.has('actors', 'transform')) {
      select.push(
        `((a.transform).location).x::float8 AS x`,
        `((a.transform).location).y::float8 AS y`,
        `((a.transform).location).z::float8 AS z`,
      );
    } else {
      select.push(`NULL::float8 AS x`, `NULL::float8 AS y`, `NULL::float8 AS z`);
    }
    if (this.has('player_state', 'death_location')) {
      select.push(
        `((ps.death_location).location).x::float8 AS death_x`,
        `((ps.death_location).location).y::float8 AS death_y`,
        `((ps.death_location).location).z::float8 AS death_z`,
      );
    } else {
      select.push(`NULL::float8 AS death_x`, `NULL::float8 AS death_y`, `NULL::float8 AS death_z`);
    }
    select.push(this.has('actors', 'partition_id') ? `a.partition_id::int8 AS partition_id` : `NULL::int8 AS partition_id`);
    const mapParts = [this.has('actors', 'map') ? `NULLIF(a.map, '')` : null, this.has('world_partition', 'label') ? `NULLIF(wp.label, '')` : null, this.has('world_partition', 'map') ? `NULLIF(wp.map, '')` : null].filter(Boolean);
    select.push(mapParts.length ? `COALESCE(${mapParts.join(', ')}, '') AS map` : `'' AS map`);
    select.push(this.has('world_partition', 'map') && this.has('actors', 'partition_id') ? `COALESCE(wp.map, '') AS world_map` : `'' AS world_map`);
    const seenCol = this.has('player_state', 'last_avatar_activity')
      ? 'ps.last_avatar_activity'
      : this.has('player_state', 'last_login_time')
        ? 'ps.last_login_time'
        : null;
    select.push(seenCol ? `COALESCE(to_char(${seenCol} AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS"Z"'), '') AS last_seen` : `'' AS last_seen`);

    const joins = [
      hasAccUser || this.has('accounts', 'funcom_id') || this.has('accounts', 'platform_id')
        ? `LEFT JOIN ${this.t('accounts')} acct ON acct.id = ps.account_id`
        : '',
      hasEnc ? `LEFT JOIN ${this.t('encrypted_accounts')} enc ON enc.id = ps.account_id` : '',
      this.has('actors', 'transform') || this.has('actors', 'map')
        ? `LEFT JOIN ${this.t('actors')} a ON a.id = ps.${this.options.pawnColumn ?? 'player_pawn_id'}`
        : '',
      this.has('world_partition', 'partition_id') && this.has('actors', 'partition_id')
        ? `LEFT JOIN ${this.t('world_partition')} wp ON wp.partition_id = a.partition_id`
        : '',
    ].filter(Boolean);

    const where: string[] = [];
    const params: unknown[] = [];
    // `PlayerConnectionStatus` has three values; only `Online` counts. `LoggingOut` is a player on the way out and is
    // deliberately excluded here so that presence sees them disappear exactly once.
    if (options.onlineOnly && this.has('player_state', 'online_status')) where.push(`ps.online_status::text = 'Online'`);
    if (options.flsId) {
      params.push(options.flsId);
      const idx = `$${params.length}`;
      const candidates = [
        hasAccUser ? `lower(COALESCE(acct."user"::text, ''))` : null,
        hasEnc ? `lower(COALESCE(enc."user"::text, ''))` : null,
        this.has('accounts', 'funcom_id') ? `lower(COALESCE(acct.funcom_id::text, ''))` : null,
        this.has('accounts', 'platform_id') ? `lower(COALESCE(acct.platform_id::text, ''))` : null,
        this.has('player_state', 'character_name') ? `lower(COALESCE(ps.character_name, ''))` : null,
      ].filter(Boolean);
      where.push(`(${candidates.map((c) => `${c} = lower(${idx})`).join(' OR ')})`);
    }
    params.push(Math.max(1, Math.min(options.limit ?? 500, 5000)));

    const sql =
      `SELECT ${select.join(',\n       ')}\n` +
      `  FROM ${this.t('player_state')} ps\n` +
      (joins.length ? `  ${joins.join('\n  ')}\n` : '') +
      (where.length ? ` WHERE ${where.join(' AND ')}\n` : '') +
      ` ORDER BY ${this.has('player_state', 'character_name') ? 'ps.character_name NULLS LAST, ' : ''}ps.account_id\n` +
      ` LIMIT $${params.length}`;

    const { rows } = await this.db.query<Record<string, unknown>>(sql, params);
    return rows.map(toPlayerRow).filter((r) => r.flsId || r.funcomId || r.characterName);
  }

  async onlinePlayers(): Promise<DunePlayerRow[]> {
    return this.roster({ onlineOnly: true });
  }

  async findPlayer(id: string): Promise<DunePlayerRow | undefined> {
    const rows = await this.roster({ flsId: id, limit: 1 });
    return rows[0];
  }

  /**
   * Last SAVED pawn position. It is NOT the live position — the audit of every DB source (persisted transform, travel
   * return info, respawn locators, overmap, actor_state) found none that tracks a moving player — so every caller has
   * to label it stale. The live position comes from the plugin, or from the last `chat.intercept` origin.
   */
  async savedLocation(flsId: string): Promise<Position | null> {
    const row = await this.findPlayer(flsId);
    return row?.position ?? null;
  }

  /**
   * The player's items. `inventories.inventory_type = 0` is the backpack; the other configured types are the gear
   * and equipment containers, which is where the rest of what a player "has" lives.
   *
   * Caveat carried into `giveItem`'s read-back: these rows are written by the server's periodic `SavePlayer`, so an
   * item granted seconds ago may not be here yet.
   */
  async inventory(flsId: string, opts: { allTypes?: boolean } = {}): Promise<DuneInventoryRow[]> {
    const player = await this.findPlayer(flsId);
    if (!player || player.accountId == null) return [];
    if (!this.has('items', 'template_id') || !this.has('inventories', 'inventory_type')) return [];
    const pawnCol = this.options.pawnColumn ?? 'player_pawn_id';
    // `allTypes` drops the display filter. `getPlayerInventory` shows the containers a player would call "mine",
    // but a GRANT can land anywhere the game likes — a contract item goes to `inventory_type = 29`, which is not a
    // display container — so a read-back that only looks at the display set reports `verified:false` for a grant
    // that plainly worked. Verification therefore scans every inventory owned by the pawn.
    const typeFilter = opts.allTypes ? '' : 'AND inv.inventory_type = ANY($2::int8[])';
    const { rows } = await this.db.query<Record<string, unknown>>(
      `SELECT i.template_id::text AS template_id,
              COALESCE(i.stack_size, 1)::int8 AS stack_size,
              ${this.has('items', 'quality_level') ? 'i.quality_level::int8' : 'NULL::int8'} AS quality_level,
              ${this.has('items', 'position_index') ? 'i.position_index::int8' : 'NULL::int8'} AS position_index,
              inv.inventory_type::int8 AS inventory_type
         FROM ${this.t('player_state')} ps
         JOIN ${this.t('inventories')} inv ON inv.actor_id = ps.${quoteIdent(pawnCol)}
         JOIN ${this.t('items')} i ON i.inventory_id = inv.id
        WHERE ps.account_id = $1::int8
          ${typeFilter}
        ORDER BY inv.inventory_type, ${this.has('items', 'position_index') ? 'i.position_index' : 'i.id'}`,
      opts.allTypes ? [player.accountId] : [player.accountId, this.inventoryTypes],
    );
    return rows.map((r) => ({
      templateId: String(r.template_id ?? ''),
      stackSize: num(r.stack_size) ?? 1,
      qualityLevel: num(r.quality_level),
      positionIndex: num(r.position_index),
      inventoryType: num(r.inventory_type),
    })).filter((r) => r.templateId);
  }

  /**
   * `listLocations`: the running map partitions plus the world's static markers. Takaro never calls this action in
   * practice, so it is a best-effort read that answers `[]` rather than failing when neither table is present.
   */
  async locations(): Promise<TakaroLocation[]> {
    const out: TakaroLocation[] = [];
    if (this.has('world_partition', 'partition_id')) {
      const label = this.has('world_partition', 'label') ? `NULLIF(wp.label, '')` : 'NULL';
      const map = this.has('world_partition', 'map') ? `NULLIF(wp.map, '')` : 'NULL';
      const { rows } = await this.db.query<Record<string, unknown>>(
        `SELECT wp.partition_id::int8 AS partition_id, COALESCE(${label}, ${map}, 'partition') AS name, COALESCE(${map}, '') AS map
           FROM ${this.t('world_partition')} wp ORDER BY wp.partition_id LIMIT 200`,
      );
      for (const r of rows) {
        const id = num(r.partition_id);
        if (id === null) continue;
        out.push({ code: `partition:${id}`, name: `${str(r.name) ?? 'partition'} (${str(r.map) ?? 'map'})`, position: { x: 0, y: 0, z: 0 } });
      }
    }
    if (this.has('markers', 'transform') && this.has('markers', 'name')) {
      const { rows } = await this.db.query<Record<string, unknown>>(
        `SELECT m.id::int8 AS id, COALESCE(m.name, '') AS name,
                ((m.transform).location).x::float8 AS x,
                ((m.transform).location).y::float8 AS y,
                ((m.transform).location).z::float8 AS z
           FROM ${this.t('markers')} m WHERE m.transform IS NOT NULL ORDER BY m.id LIMIT 500`,
      );
      for (const r of rows) {
        const x = num(r.x);
        const y = num(r.y);
        const z = num(r.z);
        if (x === null || y === null || z === null) continue;
        const name = str(r.name);
        out.push({ code: `marker:${num(r.id) ?? name ?? out.length}`, name: name ?? `Marker ${num(r.id) ?? ''}`.trim(), position: { x, y, z } });
      }
    }
    return out;
  }

  async close(): Promise<void> {
    await this.db.end?.();
  }
}

function toPlayerRow(r: Record<string, unknown>): DunePlayerRow {
  const x = num(r.x);
  const y = num(r.y);
  const z = num(r.z);
  const dx = num(r.death_x);
  const dy = num(r.death_y);
  const dz = num(r.death_z);
  return {
    flsId: str(r.fls_id) ?? '',
    funcomId: str(r.funcom_id),
    platformId: str(r.platform_id),
    platformName: str(r.platform_name),
    accountId: num(r.account_id),
    characterName: str(r.character_name),
    onlineStatus: str(r.online_status),
    lifeState: str(r.life_state),
    position: x !== null && y !== null && z !== null ? { x, y, z } : null,
    deathLocation: dx !== null && dy !== null && dz !== null ? { x: dx, y: dy, z: dz } : null,
    serverId: str(r.server_id),
    partitionId: num(r.partition_id),
    reconnectGraceEnd: str(r.reconnect_grace_end),
    transferCount: num(r.transfer_count),
    map: str(r.map),
    worldMap: str(r.world_map),
    lastSeen: str(r.last_seen),
  };
}

/**
 * `player_state.online_status` is `PlayerConnectionStatus = Offline | LoggingOut | Online` — three states, not two
 * (verified against `01_Dune.sql` on the rig image). Everything downstream goes through these two helpers so the
 * middle state can never be mistaken for either end.
 */
export const ONLINE_STATES = new Set(['online']);
/** States that mean "on the way out". They are NOT online, and reaching `Offline` afterwards is not a second leave. */
export const LEAVING_STATES = new Set(['loggingout', 'logging_out']);

/** True when `online_status` says the player is on a map right now. `LoggingOut` is deliberately false. */
export function isOnline(row: Pick<DunePlayerRow, 'onlineStatus'>): boolean {
  return ONLINE_STATES.has((row.onlineStatus ?? '').trim().toLowerCase());
}

/** True for `LoggingOut`: present in the table, already gone as far as Takaro is concerned. */
export function isLeaving(row: Pick<DunePlayerRow, 'onlineStatus'>): boolean {
  return LEAVING_STATES.has((row.onlineStatus ?? '').trim().toLowerCase());
}

/** Dune's dead life states. `Dead` is a plain death; the other two name their killer. */
export const DEAD_STATES = new Set(['dead', 'deadbycoriolis', 'deadbysandworm']);

export function isDead(row: Pick<DunePlayerRow, 'lifeState'>): boolean {
  return DEAD_STATES.has((row.lifeState ?? '').toLowerCase());
}
