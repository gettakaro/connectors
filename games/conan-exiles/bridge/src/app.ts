import { ConanAdapter } from './conan/adapter.js';
import type { BridgeConfig } from './config.js';
import { enrichLogEvent } from './events/chatEnricher.js';
import { PlayerPoller } from './events/playerPoller.js';
import { HealthServer } from './health/server.js';
import { logger } from './logger.js';
import { LogTailer } from './logs/logTailer.js';
import { ModCommandBridge } from './mod/commandBridge.js';
import { validateStrictModEvent } from './mod/strictEventValidation.js';
import { RconCommandQueue } from './rcon/commandQueue.js';
import { PersistentRconClient } from './rcon/client.js';
import { loadConanItemCatalog } from './conan/itemCatalog.js';
import { ConanSaveDbReader } from './conan/saveDb.js';
import { TakaroWsClient } from './takaro/client.js';
import type { TargetStamp } from './targetStamp.js';
import { describeStamp } from './targetStamp.js';
import type { GameEventType, GameServerAction, RequestPayload, WsMessage } from './takaro/protocol.js';

export interface StartBridgeOptions {
  /** The catalog identity of this package, logged once and served on `/health`. */
  stamp?: TargetStamp | null;
  /** Reconnect backoff, overridden only by tests that cannot wait three seconds. */
  reconnectMs?: { base: number; max: number };
}

export interface RunningBridge {
  stop(): Promise<void>;
  healthPort(): number;
}

/**
 * Wire a configured bridge up and start it, returning the handle that stops it again.
 *
 * `index.ts` is the process around this: it loads the config, reads the stamp, installs
 * the signal handlers and exits. Everything a test needs to drive the bridge end to end —
 * a fake RCON server, a fake Takaro, an ephemeral health port — is reachable from here
 * without a child process, which is what makes the contract check portable.
 */
export async function startBridge(config: BridgeConfig, options: StartBridgeOptions = {}): Promise<RunningBridge> {
  logger.info(`Starting Conan Exiles Takaro bridge for serverName='${config.serverName}'`);
  logger.info(describeStamp(options.stamp ?? null));
  logger.info(`Takaro WS: ${config.takaroWsUrl}`);
  logger.info(`Conan RCON: ${config.rcon.host}:${config.rcon.port}`);
  logger.info(`Conan save DB: ${config.databasePath ? 'configured' : 'not configured'}`);
  logger.info(`Conan item catalog: ${config.itemCatalogPath ? 'configured' : 'built-in seed only'}`);
  logger.info(`Health: http://127.0.0.1:${config.httpPort}/health`);
  logger.info(`Mod source attribution required: ${config.requireModSourceAttribution}`);

  const identifyPayload = {
    identityToken: config.identityToken ?? config.serverName,
    registrationToken: config.registrationToken,
    name: config.serverName,
  };
  const takaro = new TakaroWsClient(
    config.takaroWsUrl,
    identifyPayload,
    options.reconnectMs?.base,
    options.reconnectMs?.max,
  );

  const rcon = new PersistentRconClient({
    host: config.rcon.host,
    port: config.rcon.port,
    password: config.rcon.password,
    timeoutMs: config.rcon.timeoutMs,
  });
  const rconQueue = new RconCommandQueue((command) => rcon.run(command), config.rcon.commandGapMs);

  const emit = (type: GameEventType, data: unknown): void => {
    logger.info(`Emitting Takaro game event type=${type}`);
    takaro.sendGameEvent(type, data);
  };

  let adapter: ConanAdapter;
  const modBridge = new ModCommandBridge({
    requireSourceAttribution: config.requireModSourceAttribution,
    validateGameEvent: config.requireModSourceAttribution
      ? async (type, data) => validateStrictModEvent(type, data, () => adapter.getKnownPlayersForEvents())
      : undefined,
    emitGameEvent: (type, data) => emit(type, data),
  });
  const itemCatalog = loadConanItemCatalog(config.itemCatalogPath);
  const saveDb = new ConanSaveDbReader(config.databasePath, itemCatalog);
  adapter = new ConanAdapter((command) => rconQueue.run(command), modBridge, saveDb, itemCatalog);

  const playerPoller = new PlayerPoller(
    () => adapter.getPlayers(),
    (event) => emit(event.type, event.data),
    config.pollIntervalMs,
  );

  const emitLogEvent = async (event: { type: GameEventType; data: unknown }): Promise<void> => {
    try {
      const enriched = await enrichLogEvent(event, () => adapter.getKnownPlayersForEvents());
      emit(enriched.type, enriched.data);
    } catch (err) {
      logger.warn(`Failed to enrich log event: ${err instanceof Error ? err.message : String(err)}`);
      emit(event.type, event.data);
    }
  };

  const logTailers = config.enableLogEvents
    ? config.logFiles.map((file) => new LogTailer(file, (event) => void emitLogEvent(event)))
    : [];

  const health = new HealthServer(config.httpPort, () => ({
    ok: takaro.identified() && !takaro.getLastIdentifyError(),
    takaroIdentified: takaro.identified(),
    gameServerId: takaro.getGameServerId(),
    takaroIdentifyError: takaro.getLastIdentifyError(),
    rconConfigured: Boolean(config.rcon.host && config.rcon.port && config.rcon.password),
    logTailers: logTailers.length,
    modBridge: modBridge.status(),
    target: options.stamp ?? null,
  }), (req, res) => modBridge.handleHttpRequest(req, res));

  takaro.on('request', (message: WsMessage) => {
    void handleTakaroRequest(message, adapter, takaro);
  });
  takaro.on('identified', () => {
    playerPoller.reset();
    playerPoller.start();
  });
  takaro.on('disconnected', () => {
    playerPoller.stop();
    playerPoller.reset();
  });

  await health.start();
  takaro.connect();
  for (const tailer of logTailers) tailer.start();

  return {
    async stop(): Promise<void> {
      logger.info('Shutting down Conan Exiles Takaro bridge');
      playerPoller.stop();
      for (const tailer of logTailers) tailer.stop();
      takaro.shutdown();
      rcon.close();
      await health.stop();
    },
    healthPort(): number {
      return health.port();
    },
  };
}

async function handleTakaroRequest(message: WsMessage, adapter: ConanAdapter, takaro: TakaroWsClient): Promise<void> {
  const requestId = message.requestId;
  if (!requestId) {
    logger.warn(`Ignoring Takaro request without requestId: ${JSON.stringify(message)}`);
    return;
  }

  try {
    const payload = message.payload as RequestPayload | undefined;
    if (!payload?.action) {
      takaro.sendError(requestId, 'Missing request action');
      return;
    }

    logger.info(`Accepted Takaro request requestId=${requestId} action=${payload.action}`);
    const result = await adapter.handleAction(payload.action as GameServerAction, payload.args);
    takaro.sendResponse(requestId, result);
  } catch (err) {
    const messageText = err instanceof Error ? err.message : String(err);
    logger.error(`Failed Takaro request ${requestId}: ${messageText}`);
    takaro.sendError(requestId, messageText);
  }
}
