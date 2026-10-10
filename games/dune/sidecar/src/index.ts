import { Bridge, type EventSource } from './bridge.js';
import { DuneAdapter } from './dune/adapter.js';
import { FileBanStore } from './dune/banStore.js';
import { BanManager } from './dune/bans.js';
import { Catalogue } from './dune/catalogue.js';
import { inventoryTypesOf, loadConfig } from './dune/config.js';
import { FileCursorStore } from './dune/cursorStore.js';
import { EventPoller } from './dune/eventPoller.js';
import {
  AmqpGmPublisher,
  ExecGmPublisher,
  GmClient,
  dockerExecArgv,
  kubectlExecArgv,
  type GmPublisher,
} from './dune/gm.js';
import { FileKnownPlayerStore } from './dune/knownStore.js';
import { LogTailer, makeLogForwarder } from './dune/logTail.js';
import { mapChannel } from './dune/mapping.js';
import { FileOnlineStore } from './dune/onlineStore.js';
import { DunePg, type Queryable } from './dune/pg.js';
import { DunePluginClient } from './dune/pluginClient.js';
import { PluginJoin } from './dune/pluginJoin.js';
import { DeathCoalescer } from './dune/deathCoalescer.js';
import { mapPluginEvent } from './dune/mapping.js';
import { PresencePoller } from './dune/presence.js';
import { DuneRmq, type AmqpConnection, type AmqpConnector } from './dune/rmq.js';
import { HealthServer, type AdminRoute } from './healthServer.js';
import { logger } from './logger.js';
import { TakaroWsClient } from './takaro/client.js';
import { resolveConfigFile, TakaroSettingsSource } from './takaro/settings.js';
import type { WsMessage } from './takaro/protocol.js';

/** amqplib is loaded lazily so the unit tests (which inject a fake connector) never need the native-ish dependency. */
const amqpConnect: AmqpConnector = async (url, options) => {
  const amqplib = await import('amqplib');
  return (await amqplib.connect(url, options)) as unknown as AmqpConnection;
};

async function createPgPool(url: string, statementTimeoutMs: number, max: number): Promise<Queryable> {
  const { Pool } = await import('pg');
  const pool = new Pool({ connectionString: url, max, statement_timeout: statementTimeoutMs, application_name: 'takaro-dune-sidecar' });
  return {
    query: async <T>(sql: string, params?: unknown[]) => (await pool.query(sql, params as unknown[])) as unknown as { rows: T[] },
    end: () => pool.end(),
  };
}

async function main(): Promise<void> {
  const config = loadConfig();
  if (!config.pgUrl) throw new Error('DUNE_PG_URL (or DUNE_PG_HOST + DUNE_PG_DATABASE) is required');

  const pg = new DunePg(await createPgPool(config.pgUrl, config.pgStatementTimeoutMs, config.pgPoolMax), {
    schema: config.pgSchema,
    inventoryTypes: inventoryTypesOf(),
  });
  await pg.probe().catch((err: Error) => logger.warn(`Schema probe failed (optional columns will be treated as absent): ${err.message}`));

  const catalogue = new Catalogue({ itemsFile: config.itemsFile, entitiesFile: config.entitiesFile });
  catalogue.load();

  // Resolved before any event source starts writing to the data dir, so a fresh install is told apart from an existing
  // one by the state files it already has.
  const settings = new TakaroSettingsSource({
    env: process.env,
    configFile: resolveConfigFile(process.env),
    dataDir: config.dataDir,
    legacyStateFiles: [config.cursorFile, config.onlineFile, config.banFile, config.knownPlayersFile],
    log: logger,
  });
  const initial = settings.current();
  const configHint = (): { file: string; hasFile: boolean } => ({
    file: process.env.TAKARO_CONFIG_DISPLAY_PATH?.trim() || settings.configFile,
    hasFile: settings.hasConfigFile(),
  });
  logger.info(
    settings.hasConfigFile()
      ? `Takaro settings: environment, then ${configHint().file} (re-read every ${config.configPollMs / 1000}s, saving it applies without a restart)`
      : `Takaro settings: environment only (${configHint().file} not found; it is picked up if it appears)`,
  );
  config.serverName = initial.serverName;
  const takaro = new TakaroWsClient(
    initial.wsUrl,
    { identityToken: initial.identityToken, registrationToken: initial.registrationToken, serverName: initial.serverName },
    { baseReconnectMs: config.reconnectBaseMs, maxReconnectMs: config.reconnectMaxMs },
  );
  takaro.setConfigHint(configHint());

  let rmq: DuneRmq | null = null;
  const onChat = (message: { msg: string; senderFlsId?: string; senderFuncomId?: string; channelType?: string }): void => {
    void (async () => {
      const id = message.senderFlsId ?? message.senderFuncomId;
      const player = id ? await adapter.getPlayer(id).catch(() => undefined) : undefined;
      bridge.emit('chat-message', {
        msg: message.msg,
        channel: mapChannel(message.channelType),
        ...(player ? { player } : {}),
      });
    })();
  };
  if (config.rmqUrl) {
    rmq = new DuneRmq({
      url: config.rmqUrl,
      tlsInsecure: config.rmqTlsInsecure,
      connect: amqpConnect,
      chatQueue: config.chatQueue,
      interceptExchange: config.chatInterceptExchange,
      interceptRoutingKey: config.chatInterceptRoutingKey,
      whisperExchange: config.whisperExchange,
      mapExchange: config.mapChatExchange,
      mapRoutingKeys: config.mapChatRoutingKeys,
      ...(config.rmqManagementUrl ? { managementUrl: config.rmqManagementUrl } : {}),
      wire: config.chatWire,
      senderName: config.senderName,
      announcerFuncomId: config.announcerFuncomId,
      onChat,
    });
  } else {
    logger.warn('DUNE_RMQ_URL is not set: chat in/out and the AMQP GM publisher are disabled');
  }

  const publisher = buildPublisher(config, rmq);
  const gm = new GmClient({
    authToken: config.gmAuthToken,
    publisher,
    exchange: config.gmExchange,
    routingKey: config.gmRoutingKey,
    userId: config.gmUserId,
    appId: config.gmAppId,
  });

  const plugin = new DunePluginClient({ baseUrl: config.pluginBaseUrl, token: config.pluginToken, timeoutMs: config.pluginTimeoutMs });
  // The plugin knows Postgres row ids, never the FLS id Takaro uses as `gameId`; this is the join.
  const pluginJoin: PluginJoin | null = plugin.enabled()
    ? new PluginJoin({
        players: () => plugin.getPlayers(),
        roster: () => pg.roster({ limit: 500 }),
        ttlMs: config.pluginJoinTtlMs,
        onError: (err) => logger.debug(`Plugin join refresh failed: ${err.message}`),
      })
    : null;
  const bans: BanManager = new BanManager({
    store: new FileBanStore(config.banFile),
    kick: async (gameId: string, reason: string): Promise<boolean> => {
      const result = await adapter.kickPlayer({ gameId, reason });
      return result.verified === true;
    },
    kickCooldownMs: config.banKickCooldownMs,
  });

  const adapter: DuneAdapter = new DuneAdapter({
    pg,
    gm,
    rmq,
    plugin,
    pluginJoin,
    catalogue,
    bans,
    knownStore: new FileKnownPlayerStore(config.knownPlayersFile),
    senderName: config.senderName,
    serverName: config.serverName,
    gmPlayerIdKind: config.gmPlayerIdKind,
    globalMessageMode: config.globalMessageMode,
    verifyWindowMs: config.verifyWindowMs,
    teleportVerifyWindowMs: config.teleportVerifyWindowMs,
    takaroRequestTimeoutMs: config.takaroRequestTimeoutMs,
    takaroRequestMarginMs: config.takaroRequestMarginMs,
    verifyIntervalMs: config.verifyIntervalMs,
    shutdownCmd: config.shutdownCmd,
    shutdownNoticeSeconds: config.shutdownNoticeSeconds,
    execTimeoutMs: config.execTimeoutMs,
  });

  const onlineStore = new FileOnlineStore(config.onlineFile);
  // Exactly ONE player-death per death: the Postgres life_state edge is held briefly so the plugin's
  // attributed death (killer, weapon, position) can take its place. Without the plugin the hold simply
  // expires and the edge is emitted, which is the documented plugin-absent behaviour.
  const deaths = new DeathCoalescer({
    emit: (type, data) => void bridge.emit(type, data),
    windowMs: config.deathCoalesceMs,
  });
  const presence = new PresencePoller({
    roster: () => pg.onlinePlayers(),
    emit: (type, data) => void bridge.emit(type, data),
    initialOnline: onlineStore.load(),
    // No `save` here on purpose. The store means "who Takaro has been TOLD is online", and the bridge is its only
    // writer (it saves when an event actually reached Takaro). During a Takaro outage the presence poller's own set
    // tracks the live roster and therefore diverges from Takaro's knowledge — which is exactly what makes the
    // reconciliation on reconnect possible. If presence also wrote the file, the two would be conflated and the
    // reconciler would conclude Takaro already knows about everybody.
    intervalMs: config.presenceIntervalMs,
    transferGraceMs: config.transferGraceMs,
    emitDeath: (gameId, data) => deaths.fromLifeState(gameId, data),
    onError: (err) => logger.warn(`Presence poll failed: ${err.message}`),
  });

  const forwardLog = makeLogForwarder(config.logEvents, (msg) => void bridge.emit('log', { msg }));
  const logTailer = config.logFile
    ? new LogTailer({
        file: config.logFile,
        intervalMs: config.pollIntervalMs,
        onLine: forwardLog,
        onTick: forwardLog.flush,
        onError: (err) => logger.debug(`Log tail: ${err.message}`),
      })
    : null;

  /**
   * Plugin events the sidecar refused or re-labelled, by reason. Visible on `/health` because a guard that fires in
   * silence looks exactly like a plugin that sent nothing — which is how a `BP_DunePlayerCharacter_C` "kill" reached
   * Takaro. A rising `entityKilledUnnamed` means the entity catalogue needs a row; a rising
   * `entityKilledPlayerCharacter` means the plugin is still misclassifying player deaths (L2c).
   */
  const pluginDrops: Record<string, number> = {};
  const notePluginDrop = (reason: string): void => {
    pluginDrops[reason] = (pluginDrops[reason] ?? 0) + 1;
  };

  const pluginEvents: EventPoller | undefined = plugin.enabled()
    ? new EventPoller({
        getEvents: (since) => plugin.getEvents(since),
        // The plugin's payloads carry Postgres row ids, so the mapper is given the join and the entity
        // catalogue. An event whose subject cannot be joined is dropped by the mapper rather than
        // forwarded with a character name standing in for a `gameId`.
        mapEvent: (event) =>
          mapPluginEvent(event, {
            resolvePlayer: (source) => {
              const ref = typeof source.ref === 'string' ? source.ref : null;
              const joined = ref ? pluginJoin?.flsIdForCached(ref) ?? null : null;
              if (!joined) return null;
              // The FULL IGamePlayer, not just gameId+name: Takaro matches a player on `steamId`/`platformId` too, and
              // a plugin kill that arrived with only the two fields was the odd one out among our events. The known
              // cache is fed by every roster read, so it has the platform ids the plugin cannot know.
              const cached = adapter.lastKnown(joined.flsId);
              if (cached) return { ...cached, name: joined.characterName ?? cached.name };
              return { gameId: joined.flsId, name: joined.characterName ?? joined.flsId };
            },
            entityName: (code) => (code ? catalogue.entityName(code) : undefined),
            // LANE L2d. The plugin's `weaponItemCode` is an ITEM TEMPLATE ID, so the weapon name comes out
            // of the same catalogue index the inventory mapper uses — `ScrapMetalKnife` -> "Scrap Metal Knife".
            itemName: (code) => (code ? catalogue.displayName(code) : undefined),
            onDrop: notePluginDrop,
          }),
        emit: (type, data, seq): boolean | 'queued' => {
          // A plugin death is the ATTRIBUTED one and wins over the life_state edge; everything else
          // goes straight through with its own seq so the cursor logic is unchanged.
          if (type === 'player-death') {
            const gameId = (data as { player?: { gameId?: string } }).player?.gameId;
            if (gameId) {
              deaths.fromPlugin(gameId, data as Record<string, unknown>);
              return true;
            }
          }
          // The plugin's connect/disconnect and the Postgres presence differ are TWO sources for ONE edge, and
          // whichever is slower used to emit a second event for a join Takaro already knows about (measured
          // 2026-09-21: presence at 19:20:06, the plugin's `announceDelayMs` hint at 19:20:34 — two
          // `player-connected` rows for one join). The presence set is the record of what Takaro has been told,
          // so the edge is deduped against it: the first source through wins and tells presence, the second is
          // dropped.
          if (type === 'player-connected' || type === 'player-disconnected') {
            const player = (data as { player?: { gameId?: string; name?: string } }).player;
            const gameId = player?.gameId;
            if (gameId) {
              const known = presence.onlinePlayers().some((p) => p.gameId === gameId);
              if ((type === 'player-connected') === known) {
                notePluginDrop(`${type}Duplicate`);
                return true;
              }
              if (type === 'player-connected') {
                presence.note({ gameId, name: player?.name ?? gameId }, { flsId: gameId, characterName: player?.name ?? null });
              } else {
                presence.forget(gameId);
              }
            }
          }
          return bridge.emit(type, data, seq);
        },
        store: new FileCursorStore(config.cursorFile),
        onError: (err) => logger.debug(`Plugin event poll failed: ${err.message}`),
        intervalMs: config.pollIntervalMs,
      })
    : undefined;

  const sources: EventSource[] = [
    { name: 'presence', start: () => presence.start(), stop: () => presence.stop() },
    ...(rmq ? [{ name: 'chat', start: () => rmq!.start(), stop: () => void rmq!.close() }] : []),
    ...(logTailer ? [{ name: 'log', start: () => logTailer.start(), stop: () => logTailer.stop() }] : []),
  ];

  const bridge: Bridge = new Bridge({
    adapter,
    takaro,
    onlineStore,
    pluginEvents,
    sources,
    currentOnline: () => presence.onlinePlayers(),
    outageChatMaxAgeMs: config.outageChatMaxAgeMs,
    healthCheckIntervalMs: config.healthCheckIntervalMs,
    exitAfterDependencyLossMs: config.exitAfterDependencyLossMs,
    // "Dependency ok" is deliberately weaker than testReachability: the database ANSWERING is what the sidecar needs
    // to keep running. A battlegroup with no ready partition is a game problem, not a reason to restart ourselves.
    dependenciesOk: async () => {
      await pg.reachability();
      return true;
    },
  });

  /**
   * Ban enforcement rides its own timer over the presence poller's online set: every online player is checked against
   * the store and kicked on sight, and `enforce()` sweeps expired timed bans first so they are lifted by us (Takaro
   * never sends `unbanPlayer` for an expiry).
   *
   * This timer and the presence poller both live for the whole process, INDEPENDENT of Takaro. Enforcement is a
   * promise to the server owner about who may be on their server; it is not Takaro's to switch off. During the
   * 2026-09-21 registration-token outage the presence poller was stopped with the Takaro socket, enforcement went
   * quiet with it, and a banned player rejoined and stayed.
   */
  const banSweep = setInterval(() => void bans.enforce(presence.onlinePlayers()), Math.max(5000, config.presenceIntervalMs));
  banSweep.unref?.();

  const health = new HealthServer(
    config.healthPort,
    config.healthHost,
    () => ({
      /**
       * `ok` is about the GAME side only — Postgres answering. It is deliberately NOT ANDed with
       * `takaroIdentified`: the connector's game-side duties (presence, ban enforcement, chat, the plugin drain) keep
       * running through a Takaro outage, so reporting 503 for one would be a lie, and it is the same signal the
       * self-exit watchdog uses. Takaro's state is reported in full under `takaro`, never hidden.
       */
      ok: bridge.dependencies().ok,
      takaroIdentified: takaro.identified(),
      gameServerId: takaro.getGameServerId(),
      // Truth per dependency, so an operator can see WHICH leg is down instead of one collapsed boolean.
      deps: {
        takaro: {
          identified: takaro.identified(),
          gameServerId: takaro.getGameServerId(),
          lastConfirmedSendId: takaro.lastConfirmedId(),
          rejectedEvents: bridge.takaroRejections().count,
          lastRejectReason: bridge.takaroRejections().lastReason,
          lastRejectAt: bridge.takaroRejections().lastAt,
        },
        pg: { ok: bridge.dependencies().ok, error: bridge.dependencies().error, probed: pg.isProbed() },
        rmq: rmq
          ? { enabled: true, connected: rmq.connected(), chatConsumerBound: rmq.chatConsumerBound(), error: rmq.error() }
          : { enabled: false, connected: false, chatConsumerBound: false, error: 'DUNE_RMQ_URL is not set' },
        plugin: { enabled: plugin.enabled(), baseUrl: config.pluginBaseUrl || null, join: pluginJoin?.status() ?? null },
        // Proof that the game-side lifecycle is independent of Takaro: these stay true through an outage.
        sources: { running: bridge.isActive(), names: sources.map((s) => s.name), pluginEvents: Boolean(pluginEvents) },
      },
      takaroRejectedEvents: bridge.takaroRejections().count,
      capabilities: adapter.capabilities(),
      dependencies: bridge.dependencies(),
      online: presence.onlinePlayers().length,
      takaroKnowsOnline: bridge.onlinePlayers().length,
      pendingLeaves: presence.pendingLeaves(),
      presenceError: presence.error(),
      rmqError: rmq?.error() ?? null,
      pendingEvents: bridge.pending().length,
      unconfirmedEvents: bridge.unconfirmed().length,
      lastConfirmedSendId: takaro.lastConfirmedId(),
      droppedEvents: bridge.dropped(),
      // Per-reason counts for events the plugin guards refused or re-labelled (see `notePluginDrop`).
      droppedPluginEvents: { ...pluginDrops },
      outage: bridge.outage(),
      bans: bans.all().length,
      banEnforcement: bans.status(),
    // Lane L2 observability: was the plugin's attributed death used, or did the life_state edge answer?
    // A non-zero `suppressedDuplicate` means DUNE_DEATH_COALESCE_MS is too short for this rig.
    deaths: deaths.status(),
    pluginJoin: pluginJoin?.status() ?? null,
      playerLocation: adapter.locationSources(),
      schema: pg.probedColumns(),
    }),
    {
      adminToken: config.adminToken,
      routes: adminRoutes({ bans, adapter, presence, bridge }),
    },
  );

  takaro.on('request', (message: WsMessage) => void bridge.handleRequest(message));
  takaro.on('rejected', (reason: string) => bridge.noteTakaroError(reason));
  // Takaro coming and going now only affects DELIVERY. The event sources are started once, below, and stopped only on
  // shutdown — see `Bridge.startSources`.
  takaro.on('identified', () => void bridge.onTakaroUp());
  takaro.on('disconnected', () => bridge.onTakaroDown());

  await health.start();
  logger.info(
    `Sidecar health on http://${config.healthHost}:${config.healthPort}/health; GM publisher ${publisher.kind}; ` +
      `local admin routes ${config.adminToken ? 'enabled' : 'DISABLED (no SIDECAR_ADMIN_TOKEN / DUNE_PLUGIN_TOKEN)'}`,
  );
  bridge.startHealthWatch();
  // Game-side duties start NOW, before Takaro is even contacted, and keep running whatever Takaro does.
  await bridge.startSources();
  takaro.connect();

  const settingsWatch = setInterval(() => {
    const next = settings.poll();
    takaro.setConfigHint(configHint());
    if (!next) return;
    takaro.reconfigure(next.wsUrl, { identityToken: next.identityToken, registrationToken: next.registrationToken, serverName: next.serverName });
  }, config.configPollMs);
  settingsWatch.unref?.();

  const stop = async (): Promise<void> => {
    logger.info('Shutting down the Dune Takaro sidecar');
    clearInterval(settingsWatch);
    clearInterval(banSweep);
    bridge.stopSources();
    bridge.stopHealthWatch();
    // A death still waiting for the plugin's attribution is emitted rather than dropped.
    deaths.flush();
    deaths.stop();
    takaro.shutdown();
    await rmq?.close();
    await pg.close().catch(() => undefined);
    await health.stop();
    setTimeout(() => process.exit(0), 100);
  };
  process.on('SIGINT', () => void stop());
  process.on('SIGTERM', () => void stop());
}

/**
 * The local admin routes.
 *
 * Takaro is normally the connector's only control plane, which means that during a Takaro outage an operator has no
 * way to ban somebody, lift a ban, or warn the server — exactly when they need it most, and exactly the situation in
 * which this lane had to prove enforcement works. These routes are that escape hatch and nothing more: four verbs,
 * bearer-token protected, bound to `SIDECAR_HEALTH_HOST` (the compose network; the rig publishes no port for it).
 *
 * They go through the SAME BanManager and the SAME GM publisher as a Takaro request, so what they prove is what
 * Takaro's own path does.
 */
function adminRoutes(deps: {
  bans: BanManager;
  adapter: DuneAdapter;
  presence: PresencePoller;
  bridge: Bridge;
}): AdminRoute[] {
  const gameIdOf = (body: Record<string, unknown>): string => {
    const id = typeof body.gameId === 'string' ? body.gameId.trim() : '';
    if (!id) throw new Error("'gameId' is required");
    return id;
  };
  return [
    {
      method: 'GET',
      path: '/admin/bans',
      doc: 'The current ban store, after expiring timed bans (same view as Takaro listBans).',
      handle: () => ({ bans: deps.bans.list() }),
    },
    {
      method: 'POST',
      path: '/admin/ban',
      doc: 'Body {gameId, reason?, expiresAt?}. Adds a ban and enforces it immediately (kick-on-sight).',
      handle: async (body) => {
        const gameId = gameIdOf(body);
        const reason = typeof body.reason === 'string' ? body.reason : null;
        const expiresAt = typeof body.expiresAt === 'string' ? body.expiresAt : null;
        const known = deps.adapter.lastKnown(gameId);
        const record = deps.bans.add(gameId, reason, expiresAt, known);
        const kicked = await deps.bans.enforce(deps.presence.onlinePlayers());
        return { ban: record, kicked, online: deps.presence.onlinePlayers().map((p) => p.gameId) };
      },
    },
    {
      method: 'POST',
      path: '/admin/unban',
      doc: 'Body {gameId}. Removes the ban so the player may stay connected.',
      handle: (body) => ({ removed: deps.bans.remove(gameIdOf(body)), bans: deps.bans.all().length }),
    },
    {
      method: 'POST',
      path: '/admin/enforce',
      doc: 'Runs one ban-enforcement sweep now and reports who was kicked.',
      handle: async () => ({ kicked: await deps.bans.enforce(deps.presence.onlinePlayers()) }),
    },
    {
      method: 'POST',
      path: '/admin/message',
      doc: 'Body {message, gameId?}. Sends a server message (GM ServiceBroadcast + chat) or a whisper.',
      handle: async (body) => {
        const message = typeof body.message === 'string' ? body.message.trim() : '';
        if (!message) throw new Error("'message' is required");
        const gameId = typeof body.gameId === 'string' && body.gameId.trim() ? body.gameId.trim() : null;
        return deps.adapter.sendMessage(gameId ? { message, opts: { recipient: { gameId } } } : { message });
      },
    },
    {
      method: 'GET',
      path: '/admin/outage',
      doc: 'Outage queue state: pending, dropped, collapsed presence, stale chat, Takaro rejections.',
      handle: () => ({ outage: deps.bridge.outage(), rejections: deps.bridge.takaroRejections() }),
    },
  ];
}

function buildPublisher(config: ReturnType<typeof loadConfig>, rmq: DuneRmq | null): GmPublisher {
  switch (config.gmPublisher) {
    case 'amqp':
      if (!rmq) throw new Error('DUNE_GM_PUBLISHER=amqp needs DUNE_RMQ_URL');
      return new AmqpGmPublisher(() => rmq.ensureChannel());
    case 'docker-exec':
      return new ExecGmPublisher({
        kind: 'docker-exec',
        execArgv: dockerExecArgv(config.dockerBin, config.rmqContainer),
        timeoutMs: config.execTimeoutMs,
      });
    case 'kubectl-exec':
      return new ExecGmPublisher({
        kind: 'kubectl-exec',
        execArgv: kubectlExecArgv(config.kubectlBin, config.k8sNamespace, config.k8sPod),
        timeoutMs: config.execTimeoutMs,
      });
  }
}

main().catch((err) => {
  logger.error(`Fatal startup error: ${err instanceof Error ? err.stack || err.message : String(err)}`);
  process.exit(1);
});
