export type CapabilityState = 'ok' | 'degraded' | 'unimplemented' | 'absent';

export interface Position {
  x: number;
  y: number;
  z: number;
  dimension?: string;
}

/**
 * One row of the battlegroup roster, as `pg.ts` reads it out of `dune.player_state` joined to the account tables and
 * the pawn actor. Everything except `flsId` is optional because the schema differs between builds (the DB name itself
 * is not fixed — `dune`, `dune_sb_1_4_0_0`, …) and `pg.ts` probes for optional columns at runtime.
 */
export interface DunePlayerRow {
  /** `encrypted_accounts."user"` (16 hex) — the id GM commands and chat routing use. Takaro's `gameId`. */
  flsId: string;
  /** `accounts.funcom_id`, e.g. `PLAYER#12345`. Chat `m_FuncomIdFrom` uses this. */
  funcomId?: string | null;
  /** `accounts.platform_id`; a SteamID64 when `platform_name` is steam. */
  platformId?: string | null;
  platformName?: string | null;
  accountId?: number | null;
  characterName?: string | null;
  /**
   * `player_state.online_status::text`. The real enum is `PlayerConnectionStatus = Offline | LoggingOut | Online`
   * (verified in `01_Dune.sql`), so this is THREE-state: `LoggingOut` is a player on their way out and must never
   * be reported as online (and must not produce a second `player-disconnected` once they reach `Offline`).
   */
  onlineStatus?: string | null;
  /** `player_state.life_state::text` — 'Alive' / 'Dead' / 'DeadByCoriolis' / 'DeadBySandworm' / … */
  lifeState?: string | null;
  /** Last-SAVED pawn position (`(actors.transform).location`), never the live one. */
  position?: Position | null;
  /** True when `player_state.death_location` is populated. */
  deathLocation?: Position | null;
  /**
   * `player_state.server_id` — **TEXT**, not a number (`encrypted_player_state.server_id TEXT`, and
   * `farm_state.server_id TEXT PRIMARY KEY` which `world_partition.server_id` references). It names the map process
   * (`Survival_1`), and `is_player_offline()` treats a player whose `server_id` is not in `active_server_ids` as
   * offline — which is how the game survives a map crash that never wrote `online_status = 'Offline'`.
   */
  serverId?: string | null;
  partitionId?: number | null;
  /**
   * `player_state.reconnect_grace_period_end` (TIMESTAMP, UTC, nullable) — the game's OWN reconnect window. When it
   * is present it beats the connector's fixed `DUNE_TRANSFER_GRACE_SECONDS` guess.
   */
  reconnectGraceEnd?: string | null;
  /** `player_state.transfer_count` — bumped on a partition hand-off; a change across two polls means a map transfer. */
  transferCount?: number | null;
  /** `world_partition.label` or `actors.map` — which map process the player is on. */
  map?: string | null;
  /** `world_partition.map` of the pawn's partition (`Survival_1`, `DeepDesert_1`, …): the map Dimension. */
  worldMap?: string | null;
  lastSeen?: string | null;
  level?: number | null;
}

export interface DuneInventoryRow {
  /** `items.template_id` — the item FName; also what `AddItemToInventory.ItemName` takes. */
  templateId: string;
  stackSize: number;
  qualityLevel?: number | null;
  positionIndex?: number | null;
  inventoryType?: number | null;
}

export interface DuneReachability {
  connectable: boolean;
  reason: string | null;
  /** Partition rows that are alive AND ready (the game's own readiness signal). */
  readyPartitions?: number;
  expectedPartitions?: number;
}

/** Takaro IGamePlayer. For Dune `gameId` = FLS id, `steamId` = platform_id when steam, `platformId` = `steam:<id>`. */
export interface TakaroPlayer {
  gameId: string;
  name: string;
  /** Only set on a last-known record answered for a player who is not online (see adapter.getPlayer). */
  online?: boolean;
  steamId?: string;
  platformId?: string;
  /** Dune exposes neither the client IP nor a ping through Postgres or RabbitMQ; both stay absent. */
  ip?: string;
  ping?: number;
}

export interface TakaroItem {
  code: string;
  name: string;
  description?: string;
  amount?: number;
  quality?: string;
}

export type TakaroEntityType = 'hostile' | 'friendly' | 'neutral';

export interface TakaroEntity {
  code: string;
  name: string;
  description?: string;
  type: TakaroEntityType;
}

export interface TakaroLocation {
  code: string;
  name: string;
  position: Position;
  radius?: number;
  sizeX?: number;
  sizeY?: number;
  sizeZ?: number;
}

export interface TakaroBan {
  player: TakaroPlayer;
  reason: string;
  expiresAt: string | null;
}

/** One inbound chat message, as `rmq.ts` decodes it off `chat.intercept`. */
export interface DuneChatMessage {
  /** Raw text (`m_Message.m_UnlocalizedMessage`). */
  msg: string;
  /** AMQP `user_id` of the publisher = the sender's FLS id. Absent on builds that do not set it. */
  senderFlsId?: string;
  /** `m_FuncomIdFrom`. */
  senderFuncomId?: string;
  /** `m_ChannelType`, normalised to the short form (`Map`, `Whispers`, `Proximity`, `Guild`, `Party`, …). */
  channelType?: string;
  /** Live position at the moment of the message (`m_OriginLocation`) — the only live location out of process. */
  originLocation?: Position | null;
  /** Message id from the inner payload / AMQP `message_id`; used by the noEcho filter. */
  messageId?: string;
  /** The AMQP routing key the message arrived on (diagnostics). */
  routingKey?: string;
}

/** Optional native plugin (`libtakaro-dune.so`) health body. */
export interface PluginHealth {
  status: string;
  version?: string;
  gameBuild?: string;
  engineVersion?: string;
  capabilities?: Record<string, CapabilityState | string>;
  diagnostics?: Record<string, unknown>;
}

export interface PluginEvent {
  seq: number;
  type: string;
  data: unknown;
  ts?: string | number;
}

export interface PluginEventsResponse {
  /** Per-process id of the game server; changes when the map process restarts. */
  bootId?: string;
  seq: number;
  truncated?: boolean;
  events: PluginEvent[];
}

/**
 * One entry of the plugin's `/players` — the players the PLUGIN has seen through `PostLogin`.
 *
 * ⚠️ This is not the roster and it carries no FLS id. Lane L2 established that the map process holds
 * no account-id *string*: what it does hold, on `DunePlayerControllerPersistenceComponent`, are the
 * Postgres row ids — `accountId` → `encrypted_accounts.id`, and the three actor ids that
 * `encrypted_player_state.{player_state_id, player_controller_id, player_pawn_id}` reference. The
 * sidecar joins those to `accounts."user"` (the FLS id, and Takaro's `gameId`) itself; see
 * `pluginJoin.ts`. A field the plugin could not read is **null**, never 0 — a zero would invite a
 * join against actor row 0.
 */
export interface PluginPlayer {
  /** The plugin's own handle: `acct:<accountId>`, `ps:<playerStateId>` or `name:<characterName>`. */
  ref: string;
  characterName?: string | null;
  accountId?: number | string | null;
  playerStateId?: number | string | null;
  playerControllerId?: number | string | null;
  playerPawnId?: number | string | null;
  position?: Position | null;
  /** `pawn` (the live transform), `playerState` (a fallback) or `unknown`. Always present. */
  positionSource?: string;
  online?: boolean;
  /** Which properties produced the ids, verbatim from the plugin — the audit trail for the join. */
  identityHow?: string;
}

export interface PluginPlayersResponse {
  players: PluginPlayer[];
  generation?: number;
  snapshotAgeMs?: number;
  snapshotFresh?: boolean;
}

/** `/players/{ref}/location`. `source` is always present; a silently stale answer would be worse than none. */
export interface PluginLocation extends Position {
  pitch?: number;
  yaw?: number;
  source: string;
  ageMs?: number;
  snapshotAgeMs?: number;
  at?: string;
}
