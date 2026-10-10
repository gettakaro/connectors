import path from 'node:path';
import type { PlayerIdKind } from './identity.js';

export type GmPublisherKind = 'amqp' | 'docker-exec' | 'kubectl-exec';
export type GlobalMessageMode = 'chat' | 'broadcast' | 'both';

/**
 * Wire variants the live probe has to settle. Every one of these was observed to be build-sensitive in the community
 * tooling, so each is a config switch rather than a code constant: the rig can flip them and re-test without a rebuild
 * (campaign plan, "Chat payload field drift").
 */
export interface ChatWireVariant {
  /** `m_TimeStamp` (confirmed client-visible) or `m_Timestamp` (rollback knob for a future build). */
  timestampField: string;
  /** UE `%Y.%m.%d-%H.%M.%S` (confirmed) or RFC3339. */
  timestampFormat: 'ue' | 'rfc3339';
  /** Emit `m_ChannelType` short (`Whispers`) or fully qualified (`ETextChatChannelType::Whispers`). */
  channelEnumForm: 'short' | 'qualified';
  /** Outer body key for the inner chat JSON string: `Content` (envelope-style) or `content` (observed inbound). */
  contentKey: 'Content' | 'content';
  /** AMQP `content_type`. */
  contentType: string;
  /** AMQP `type`. */
  amqpType: string;
  /** AMQP `delivery_mode`: 1 = transient, as the confirmed probes used. */
  deliveryMode: 1 | 2;
  /** Outer `Type` discriminator in the body. */
  bodyType: string;
  /**
   * Field carrying the SENDER's display name. **Empty by default, and that is the measured answer**: a real
   * player-authored `chat.intercept` frame, captured live 2026-09-21, contains no such field at all, and sending
   * `m_UserNameFrom` changed nothing on screen. Kept as a switch only so a future build can be tried without a
   * rebuild. See `buildChatPayload`.
   */
  senderNameField: string;
  /**
   * `m_ChannelType` of a private line on `chat.whispers`. **`Map` by default, measured on Steam builds 25635074 and
   * 25689360 (clients 25611704 and 25690505)**: those clients render every `Whispers`-typed frame we publish as an
   * empty `[]:` line (the timestamp spelling, the recipient form, the spoof flag, the localized block and the sender
   * id were each A/B tested and none of them mattered), while the same frame typed `Map` or `Proximity` renders its
   * text. The line
   * stays private either way, because the routing key is the recipient's own `chat.whispers` binding; only the tag
   * the client files it under changes. Set `Whispers` for a build that renders it again.
   */
  whisperChannelType: string;
}

export interface SidecarConfig {
  // --- Takaro ---
  // The connection settings (URL, tokens, server name) live in `takaro/settings.ts`: they can change at runtime.
  serverName: string;
  /** How often the `.env` file is re-read for changed Takaro settings. */
  configPollMs: number;
  /** Sender name shown in game for our own chat messages (spoofed-username field). */
  senderName: string;
  reconnectBaseMs: number;
  reconnectMaxMs: number;

  // --- Postgres (read-only) ---
  pgUrl: string;
  pgStatementTimeoutMs: number;
  pgPoolMax: number;
  /** Schema the game's tables live in (`dune`); the DATABASE name is part of pgUrl and is not fixed. */
  pgSchema: string;

  // --- game RabbitMQ ---
  rmqUrl: string;
  /** Accept the broker's self-signed certificate (the shipped game-RMQ cert is not a public CA chain). */
  rmqTlsInsecure: boolean;
  chatQueue: string;
  chatInterceptExchange: string;
  chatInterceptRoutingKey: string;
  whisperExchange: string;
  mapChatExchange: string;
  /** Fixed `chat.map` routing keys (comma-separated). Empty = discover the live bindings from the broker. */
  mapChatRoutingKeys: string[];
  /** Broker management API base URL; empty = `http://<amqp host>:15672`. */
  rmqManagementUrl: string;
  chatWire: ChatWireVariant;
  /** Funcom id our outbound messages claim as sender (`m_FuncomIdFrom`), e.g. `ADMIN#00001`. */
  announcerFuncomId: string;
  globalMessageMode: GlobalMessageMode;

  // --- GM commands ---
  gmAuthToken: string;
  gmPublisher: GmPublisherKind;
  gmExchange: string;
  gmRoutingKey: string;
  gmAmqpUser: string;
  gmAmqpPassword: string;
  gmUserId: string;
  gmAppId: string;
  gmPlayerIdKind: PlayerIdKind;
  rmqContainer: string;
  dockerBin: string;
  kubectlBin: string;
  k8sNamespace: string;
  k8sPod: string;
  execTimeoutMs: number;

  // --- optional native plugin ---
  pluginBaseUrl: string;
  pluginToken: string;
  pluginTimeoutMs: number;
  /**
   * How long the Postgres `life_state` death edge waits for the plugin's ATTRIBUTED death before it is
   * emitted on its own, so a death reaches Takaro exactly once. See `deathCoalescer.ts`.
   */
  deathCoalesceMs: number;
  /** How long the plugin-ref -> FLS id join is cached before /players and the roster are re-read. */
  pluginJoinTtlMs: number;

  // --- polling / timing ---
  presenceIntervalMs: number;
  pollIntervalMs: number;
  healthCheckIntervalMs: number;
  /**
   * A map transfer (Hagga Basin → Deep Desert) takes the player offline in `player_state` for a few seconds. Within
   * this window a disappearance is NOT reported as a disconnect.
   */
  transferGraceMs: number;
  /** How long a mutation read-back may poll before the action answers `verified:false`. */
  verifyWindowMs: number;
  /** Read-back window for teleports; the GM command takes ~20 s to run on a live rig. */
  teleportVerifyWindowMs: number;
  verifyIntervalMs: number;
  banKickCooldownMs: number;
  /**
   * How long Takaro waits for our `response` frame before it abandons the request (`ws.requestTimeoutMs` in
   * app-connector, default 10 s, env `T_WEBSOCKET_REQUEST_TIMEOUT_MS`). Every read-back window is capped below this,
   * because a verification that outlives the request proves nothing to anybody: Takaro has already errored.
   */
  takaroRequestTimeoutMs: number;
  /** Safety margin subtracted from `takaroRequestTimeoutMs` to leave room for the reply to travel. */
  takaroRequestMarginMs: number;
  /** Max age of a `chat-message` queued during a Takaro outage; older lines are dropped instead of replayed. */
  outageChatMaxAgeMs: number;

  // --- state ---
  dataDir: string;
  cursorFile: string;
  onlineFile: string;
  banFile: string;
  knownPlayersFile: string;
  itemsFile: string;
  entitiesFile: string;

  // --- log forwarding ---
  logFile: string;
  logEvents: 'all' | 'filtered' | 'none';

  // --- shutdown ---
  /** Argv (JSON array or shell-free space-separated) run to stop the battlegroup after the shutdown broadcast. */
  shutdownCmd: string;
  shutdownNoticeSeconds: number;

  // --- health endpoint / watchdog ---
  healthPort: number;
  healthHost: string;
  exitAfterDependencyLossMs: number;
  /**
   * Bearer token for the local admin routes on the health server. Defaults to `DUNE_PLUGIN_TOKEN` so the rig needs no
   * new secret; empty disables the routes entirely.
   */
  adminToken: string;
}

type Env = Record<string, string | undefined>;

export function loadConfig(env: Env = process.env): SidecarConfig {
  const dataDir = (env.TAKARO_DATA_DIR || './data').replace(/\/+$/, '') || '.';
  const cursorFile = env.TAKARO_CURSOR_FILE || path.join(dataDir, 'event-cursor.json');

  const publisher = (env.DUNE_GM_PUBLISHER || 'amqp').toLowerCase();
  if (publisher !== 'amqp' && publisher !== 'docker-exec' && publisher !== 'kubectl-exec') {
    throw new Error(`DUNE_GM_PUBLISHER must be amqp|docker-exec|kubectl-exec, got '${publisher}'`);
  }
  /**
   * What `PlayerId` means in a GM server command. The switch stays, but the default is no longer a coin flip:
   *
   * The schema enumerates all five flavours (`PlayerIdentifierType = None | ByCharacterName | ByFlsId | ByPlayerId |
   * ByFuncomId | ByPlatformId`) and `get_players_info` spells out what each one matches — crucially
   * `ByPlayerId` resolves against `player_state.player_controller_id::text`, i.e. a numeric actor row id, which is
   * not something an admin tool ever types. Meanwhile Funcom's own admin-tool functions call the **FLS id** the
   * player id: `admin_get_character_details` returns `accounts."user" AS player_id`, and
   * `admin_move_offline_player`, `is_player_offline` and `admin_get_character_ids` all key on `accounts."user"`.
   *
   * So `fls` is the default. `funcom` and `name` remain available for a build that disagrees, and the live
   * `KickPlayer` probe settles it for good.
   */
  const idKind = (env.DUNE_GM_PLAYER_ID_KIND || 'fls').toLowerCase();
  if (idKind !== 'fls' && idKind !== 'funcom' && idKind !== 'name') {
    throw new Error(`DUNE_GM_PLAYER_ID_KIND must be fls|funcom|name, got '${idKind}'`);
  }
  /**
   * `both` is the default because only the broadcast variant is PROVEN to render: the map server acknowledges
   * `ServiceBroadcast` (`LogDuneServerCommands: Now running ServerCommand 'ServiceBroadcast'`) and it was seen on
   * screen, while the `chat.map` variant is proven routable at the broker but its in-game rendering is unconfirmed.
   * Sending both means a Takaro `sendMessage` is visible to the player on every build, and the response body says
   * which legs carried it, so nothing is claimed that was not published.
   */
  const globalMode = (env.DUNE_GLOBAL_MESSAGE_MODE || 'both').toLowerCase();
  if (globalMode !== 'chat' && globalMode !== 'broadcast' && globalMode !== 'both') {
    throw new Error(`DUNE_GLOBAL_MESSAGE_MODE must be chat|broadcast|both, got '${globalMode}'`);
  }
  const logEvents = (env.DUNE_LOG_EVENTS || 'filtered').toLowerCase();
  if (logEvents !== 'all' && logEvents !== 'filtered' && logEvents !== 'none') {
    throw new Error(`DUNE_LOG_EVENTS must be all|filtered|none, got '${logEvents}'`);
  }
  const tsFormat = (env.DUNE_CHAT_TIMESTAMP_FORMAT || 'ue').toLowerCase();
  if (tsFormat !== 'ue' && tsFormat !== 'rfc3339') {
    throw new Error(`DUNE_CHAT_TIMESTAMP_FORMAT must be ue|rfc3339, got '${tsFormat}'`);
  }
  const enumForm = (env.DUNE_CHAT_CHANNEL_ENUM_FORM || 'short').toLowerCase();
  if (enumForm !== 'short' && enumForm !== 'qualified') {
    throw new Error(`DUNE_CHAT_CHANNEL_ENUM_FORM must be short|qualified, got '${enumForm}'`);
  }
  const contentKey = env.DUNE_CHAT_CONTENT_KEY || 'Content';
  if (contentKey !== 'Content' && contentKey !== 'content') {
    throw new Error(`DUNE_CHAT_CONTENT_KEY must be Content|content, got '${contentKey}'`);
  }
  const deliveryMode = int(env.DUNE_CHAT_DELIVERY_MODE, 1);
  if (deliveryMode !== 1 && deliveryMode !== 2) throw new Error(`DUNE_CHAT_DELIVERY_MODE must be 1 or 2`);

  return {
    // Replaced at start by the effective name from `takaro/settings.ts`.
    serverName: env.TAKARO_SERVER_NAME || '',
    configPollMs: Math.max(1000, int(env.TAKARO_CONFIG_POLL_MS, 5000)),
    // `Takaro` rather than `Server`: this is what a player sees in front of every connector message, and "Server" is
    // indistinguishable from the game's own notices. Overridable with `TAKARO_SENDER_NAME`.
    senderName: (env.TAKARO_SENDER_NAME || env.TAKARO_SERVER_CHAT_NAME || 'Takaro').trim(),
    reconnectBaseMs: int(env.TAKARO_RECONNECT_BASE_MS, 2000),
    reconnectMaxMs: int(env.TAKARO_RECONNECT_MAX_MS, 60000),

    pgUrl: pgUrlOf(env),
    pgStatementTimeoutMs: int(env.DUNE_PG_STATEMENT_TIMEOUT_MS, 5000),
    pgPoolMax: int(env.DUNE_PG_POOL_MAX, 4),
    pgSchema: env.DUNE_PG_SCHEMA || 'dune',

    rmqUrl: env.DUNE_RMQ_URL || '',
    rmqTlsInsecure: bool(env.DUNE_RMQ_TLS_INSECURE, true),
    chatQueue: env.DUNE_CHAT_QUEUE || 'takaro_chat_intercept',
    chatInterceptExchange: env.DUNE_CHAT_INTERCEPT_EXCHANGE || 'chat.intercept',
    chatInterceptRoutingKey: env.DUNE_CHAT_INTERCEPT_ROUTING_KEY || '#',
    whisperExchange: env.DUNE_CHAT_WHISPER_EXCHANGE || 'chat.whispers',
    mapChatExchange: env.DUNE_CHAT_MAP_EXCHANGE || 'chat.map',
    mapChatRoutingKeys: (env.DUNE_CHAT_MAP_ROUTING_KEYS || '').split(',').map((k) => k.trim()).filter(Boolean),
    rmqManagementUrl: env.DUNE_RMQ_MANAGEMENT_URL?.trim() ?? '',
    chatWire: {
      timestampField: env.DUNE_CHAT_TIMESTAMP_FIELD || 'm_TimeStamp',
      timestampFormat: tsFormat,
      channelEnumForm: enumForm,
      contentKey,
      contentType: env.DUNE_CHAT_CONTENT_TYPE || 'Content',
      amqpType: env.DUNE_CHAT_AMQP_TYPE || 'text_chat',
      deliveryMode: deliveryMode as 1 | 2,
      bodyType: env.DUNE_CHAT_BODY_TYPE || 'TextChat',
      senderNameField: env.DUNE_CHAT_SENDER_NAME_FIELD ?? '',
      whisperChannelType: env.DUNE_CHAT_WHISPER_CHANNEL_TYPE?.trim() || 'Map',
    },
    announcerFuncomId: env.DUNE_ANNOUNCER_FUNCOM_ID || 'ADMIN#00001',
    globalMessageMode: globalMode,

    gmAuthToken: env.DUNE_GM_AUTH_TOKEN?.trim() ?? '',
    gmPublisher: publisher,
    gmExchange: env.DUNE_GM_EXCHANGE || 'heartbeats',
    gmRoutingKey: env.DUNE_GM_ROUTING_KEY || 'notifications',
    gmAmqpUser: env.DUNE_GM_AMQP_USER || 'fls',
    gmAmqpPassword: env.DUNE_GM_AMQP_PASS || env.DUNE_GM_AMQP_PASSWORD || '',
    gmUserId: env.DUNE_GM_USER_ID || 'fls',
    gmAppId: env.DUNE_GM_APP_ID || 'fls_backend',
    gmPlayerIdKind: idKind,
    rmqContainer: env.DUNE_RMQ_CONTAINER || 'dune_server-game-rmq-1',
    dockerBin: env.DUNE_DOCKER_BIN || 'docker',
    kubectlBin: env.DUNE_KUBECTL_BIN || 'kubectl',
    k8sNamespace: env.DUNE_K8S_NAMESPACE || 'default',
    k8sPod: env.DUNE_K8S_POD || '',
    execTimeoutMs: int(env.DUNE_EXEC_TIMEOUT_MS, 20000),

    pluginBaseUrl: (env.DUNE_PLUGIN_URL || '').replace(/\/+$/, ''),
    pluginToken: env.DUNE_PLUGIN_TOKEN?.trim() ?? '',
    pluginTimeoutMs: int(env.DUNE_PLUGIN_TIMEOUT_MS, 10000),
    deathCoalesceMs: int(env.DUNE_DEATH_COALESCE_MS, 5000),
    pluginJoinTtlMs: int(env.DUNE_PLUGIN_JOIN_TTL_MS, 5000),

    presenceIntervalMs: int(env.DUNE_PRESENCE_INTERVAL_MS, 5000),
    pollIntervalMs: int(env.TAKARO_POLL_INTERVAL_MS, 1000),
    healthCheckIntervalMs: int(env.TAKARO_HEALTH_INTERVAL_MS, 15000),
    transferGraceMs: int(env.DUNE_TRANSFER_GRACE_SECONDS, 45) * 1000,
    verifyWindowMs: int(env.DUNE_VERIFY_WINDOW_MS, 8000),
    teleportVerifyWindowMs: int(env.DUNE_TELEPORT_VERIFY_WINDOW_MS, 30000),
    verifyIntervalMs: int(env.DUNE_VERIFY_INTERVAL_MS, 500),
    banKickCooldownMs: int(env.DUNE_BAN_KICK_COOLDOWN_MS, 15000),
    takaroRequestTimeoutMs: int(env.TAKARO_REQUEST_TIMEOUT_MS, 10000),
    takaroRequestMarginMs: int(env.TAKARO_REQUEST_MARGIN_MS, 1500),
    outageChatMaxAgeMs: int(env.DUNE_OUTAGE_CHAT_MAX_AGE_MS, 600000),

    dataDir,
    cursorFile,
    onlineFile: env.TAKARO_ONLINE_FILE || path.join(dataDir, 'online-players.json'),
    banFile: env.TAKARO_BAN_FILE || path.join(dataDir, 'bans.json'),
    knownPlayersFile: env.TAKARO_KNOWN_PLAYERS_FILE || path.join(dataDir, 'known-players.json'),
    itemsFile: env.DUNE_ITEMS_FILE || path.join(dataDir, 'items.json'),
    entitiesFile: env.DUNE_ENTITIES_FILE || path.join(dataDir, 'entities.json'),

    logFile: env.DUNE_LOG_FILE || '',
    logEvents,

    shutdownCmd: env.DUNE_SHUTDOWN_CMD ?? '',
    shutdownNoticeSeconds: int(env.DUNE_SHUTDOWN_NOTICE_SECONDS, 60),

    healthPort: int(env.SIDECAR_HEALTH_PORT, 18891),
    healthHost: env.SIDECAR_HEALTH_HOST || '127.0.0.1',
    // A restart cannot fix a dead Postgres or broker, but it does re-identify us and re-create the netns when the
    // battlegroup was re-created underneath us; 180 s is long enough to ride out a normal map/broker restart.
    exitAfterDependencyLossMs: int(env.SIDECAR_EXIT_AFTER_DEPENDENCY_LOSS_MS, 180000),
    adminToken: (env.SIDECAR_ADMIN_TOKEN?.trim() || env.DUNE_PLUGIN_TOKEN?.trim() || '') as string,
  };
}

/**
 * `DUNE_PG_URL`, or assembled from the discrete parts. The database NAME is not fixed across builds (`dune`,
 * `dune_sb_1_4_0_0`, …) so it is always explicit — there is no safe default to guess.
 */
export function pgUrlOf(env: Env): string {
  if (env.DUNE_PG_URL?.trim()) return env.DUNE_PG_URL.trim();
  const host = env.DUNE_PG_HOST?.trim();
  const database = env.DUNE_PG_DATABASE?.trim();
  if (!host || !database) return '';
  const port = int(env.DUNE_PG_PORT, 5432);
  const user = encodeURIComponent(env.DUNE_PG_USER?.trim() || 'dune');
  const password = env.DUNE_PG_PASSWORD ? `:${encodeURIComponent(env.DUNE_PG_PASSWORD)}` : '';
  return `postgres://${user}${password}@${host}:${port}/${encodeURIComponent(database)}`;
}

/**
 * Which `inventories.inventory_type`s `getPlayerInventory` REPORTS to Takaro.
 *
 * Only physical carry inventories belong here. The rest of the pawn's containers are game bookkeeping, and reporting
 * them made Takaro's inventory screen show ten `Emote_*` "items" that no player can hold, drop or be given.
 *
 * Measured on the live rig 2026-09-21 (TakaroTest's pawn, `dune.inventories ⋈ dune.items` per type — see
 * `research/2026-09-21-inventory-types.md`). There is **no enum for this in Postgres** (checked `pg_enum`), so the
 * table is empirical plus one confirmation from Funcom's own SQL: `migrate_clamp_max_allow_solaris` carries the
 * comment `-- inventory_type 0 is backpack`.
 *
 * | type | contents observed | reported |
 * |---|---|---|
 * | 0 | backpack — Ammo, AzuriteOre, Oil, ScrapMetal, Stone (**Funcom's own comment confirms**) | ✅ |
 * | 1 | worn clothing/gear — Social_Choam_MaulaCastOffs01_*, PowerPack | ✅ |
 * | 15 | hotbar / equipped tools — Crysknife, Literjon, MiningTool_1h_Standard, ScrapMetalKnife | ✅ |
 * | 14 | emotes — Emote_IxianSecret_01, Emote_ShakeOffSand_01 | ❌ not carryable |
 * | 27 | emotes — Emote_Bow_01, Clap, Follow, No, Point, Sit, Threaten, Yes | ❌ not carryable |
 * | 29 | contract/quest items — `wy1ll` ("Electronics Delivery") | ❌ not a possession |
 * | 12, 20, 25, 30–33 | empty on this pawn; purpose unknown | ❌ not claimed |
 * | 22 | NOT the player's — belongs to `BP_LootContainerDefeatDrop_C` (a death-drop container) | n/a |
 *
 * The giveItem read-back deliberately scans **every** type (`pg.inventory(id, {allTypes:true})`): that is how the
 * type-29 contract-item grant was caught, and a verification that only looks where it expects the item proves nothing.
 */
export const REPORTED_INVENTORY_TYPES = [0, 1, 15];

export function inventoryTypesOf(env: Env = process.env): number[] {
  const raw = env.DUNE_INVENTORY_TYPES?.trim();
  if (!raw) return [...REPORTED_INVENTORY_TYPES];
  const out = raw
    .split(',')
    .map((v) => Number.parseInt(v.trim(), 10))
    .filter((v) => Number.isInteger(v) && v >= 0);
  if (!out.length) throw new Error(`DUNE_INVENTORY_TYPES has no valid integers: '${raw}'`);
  return [...new Set(out)];
}

function int(value: string | undefined, fallback: number): number {
  if (value == null || value.trim() === '') return fallback;
  const parsed = Number.parseInt(value, 10);
  return Number.isFinite(parsed) && parsed >= 0 ? parsed : fallback;
}

function bool(value: string | undefined, fallback: boolean): boolean {
  if (value == null || value.trim() === '') return fallback;
  return /^(1|true|yes|on)$/i.test(value.trim());
}
