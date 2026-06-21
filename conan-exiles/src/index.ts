import { ConanAdapter } from './conan/adapter.js';
import { loadConfig } from './config.js';
import { enrichLogEvent } from './events/chatEnricher.js';
import { PlayerPoller } from './events/playerPoller.js';
import { HealthServer } from './health/server.js';
import { logger } from './logger.js';
import { LogTailer } from './logs/logTailer.js';
import { ModCommandBridge } from './mod/commandBridge.js';
import { RconCommandQueue } from './rcon/commandQueue.js';
import { sendRconCommand } from './rcon/client.js';
import { loadConanItemCatalog } from './conan/itemCatalog.js';
import { ConanSaveDbReader } from './conan/saveDb.js';
import { TakaroWsClient } from './takaro/client.js';
import type { GameEventType, GameServerAction, RequestPayload, WsMessage } from './takaro/protocol.js';

async function main(): Promise<void> {
  const config = loadConfig();

  logger.info(`Starting Conan Exiles Takaro bridge for serverName='${config.serverName}'`);
  logger.info(`Takaro WS: ${config.takaroWsUrl}`);
  logger.info(`Conan RCON: ${config.rcon.host}:${config.rcon.port}`);
  logger.info(`Conan save DB: ${config.databasePath ? 'configured' : 'not configured'}`);
  logger.info(`Conan item catalog: ${config.itemCatalogPath ? 'configured' : 'built-in seed only'}`);
  logger.info(`Health: http://127.0.0.1:${config.httpPort}/health`);

  const takaro = new TakaroWsClient(config.takaroWsUrl, {
    identityToken: config.identityToken,
    registrationToken: config.registrationToken,
    name: config.serverName,
  });

  const rconQueue = new RconCommandQueue((command) =>
    sendRconCommand({
      host: config.rcon.host,
      port: config.rcon.port,
      password: config.rcon.password,
      command,
      timeoutMs: config.rcon.timeoutMs,
    }),
    300,
  );

  const emit = (type: GameEventType, data: unknown): void => {
    logger.info(`Emitting Takaro game event type=${type}`);
    takaro.sendGameEvent(type, data);
  };

  const modBridge = new ModCommandBridge({
    emitGameEvent: (type, data) => emit(type, data),
  });
  const itemCatalog = loadConanItemCatalog(config.itemCatalogPath);
  const saveDb = new ConanSaveDbReader(config.databasePath, itemCatalog);
  const adapter = new ConanAdapter((command) => rconQueue.run(command), modBridge, saveDb, itemCatalog);

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
    ok: true,
    takaroIdentified: takaro.identified(),
    gameServerId: takaro.getGameServerId(),
    rconConfigured: Boolean(config.rcon.host && config.rcon.port && config.rcon.password),
    logTailers: logTailers.length,
    modBridge: modBridge.status(),
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

  const stop = async (): Promise<void> => {
    logger.info('Shutting down Conan Exiles Takaro bridge');
    playerPoller.stop();
    for (const tailer of logTailers) tailer.stop();
    takaro.shutdown();
    await health.stop();
    setTimeout(() => process.exit(0), 100);
  };

  process.on('SIGINT', () => void stop());
  process.on('SIGTERM', () => void stop());
  process.on('uncaughtException', (err) => {
    logger.error(`Uncaught exception: ${err.stack || err.message}`);
  });
  process.on('unhandledRejection', (reason) => {
    logger.error(`Unhandled rejection: ${String(reason)}`);
  });
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

main().catch((err) => {
  logger.error(`Fatal startup error: ${err.stack || err.message}`);
  process.exit(1);
});
