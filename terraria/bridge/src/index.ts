import { loadConfig } from './config.js';
import { normalizeGameEvent } from './events/normalizeEvent.js';
import { PlayerPoller } from './events/playerPoller.js';
import { HealthServer } from './health/server.js';
import { logger } from './logger.js';
import { LogTailer } from './logs/logTailer.js';
import { TakaroWsClient } from './takaro/client.js';
import type { GameEventType, RequestPayload, WsMessage } from './takaro/protocol.js';
import { TerrariaAdapter } from './terraria/adapter.js';
import { TShockClient } from './tshock/client.js';

async function main(): Promise<void> {
  const config = loadConfig();
  const tshock = new TShockClient(config.tshock);
  const adapter = new TerrariaAdapter(tshock, {
    commandAllowlistExact: config.commandAllowlistExact,
    commandAllowlistPrefixes: config.commandAllowlistPrefixes,
    enableShutdown: config.enableShutdown,
    serverChatName: config.serverChatName,
  });
  const takaro = new TakaroWsClient(config.takaroWsUrl, {
    identityToken: config.identityToken,
    registrationToken: config.registrationToken,
    name: config.serverName,
  });

  let tshockReachable = false;
  const emit = (type: GameEventType, data: unknown): void => {
    const event = normalizeGameEvent(type, data);
    const sent = takaro.sendGameEvent(event.type, event.data);
    if (!sent) logger.warn(`Takaro event dropped: ${type}`);
  };
  const poller = new PlayerPoller(
    () => adapter.getPlayers(),
    (event) => emit(event.type, event.data),
    config.pollIntervalMs,
  );
  const tailers = config.logFiles.map((file) => new LogTailer(file, (event) => emit(event.type, event.data)));
  const health = new HealthServer(config.httpPort, () => ({
    ok: true,
    takaroIdentified: takaro.identified(),
    gameServerId: takaro.getGameServerId(),
    tshockReachable,
    lastPollAt: poller.lastPollAt,
  }));

  takaro.on('request', (message: WsMessage & { payload?: RequestPayload }) => {
    void handleTakaroRequest(message, adapter, takaro);
  });
  takaro.on('clientError', (err) => {
    logger.error(`Takaro client error: ${err instanceof Error ? err.message : JSON.stringify(err)}`);
  });
  takaro.on('serverError', (payload) => {
    logger.error(`Takaro server error: ${JSON.stringify(payload)}`);
  });
  takaro.on('identifyError', (payload) => {
    logger.error(`Takaro identify error: ${JSON.stringify(payload)}`);
  });
  takaro.on('identified', () => {
    poller.reset();
    poller.start();
    for (const tailer of tailers) tailer.start();
  });
  takaro.on('disconnected', () => {
    poller.stop();
    poller.reset();
    for (const tailer of tailers) tailer.stop();
  });

  await health.start();
  const reachability = await adapter.handleAction('testReachability', {});
  tshockReachable = Boolean((reachability as { connectable?: boolean }).connectable);
  logger.info(`Terraria bridge health: http://127.0.0.1:${health.port()}/health`);
  takaro.connect();

  const stop = async (): Promise<void> => {
    logger.info('Shutting down Terraria Takaro bridge');
    poller.stop();
    for (const tailer of tailers) tailer.stop();
    takaro.shutdown();
    await health.stop();
    setTimeout(() => process.exit(0), 100);
  };

  process.on('SIGINT', () => void stop());
  process.on('SIGTERM', () => void stop());
}

async function handleTakaroRequest(
  message: WsMessage & { payload?: RequestPayload },
  adapter: TerrariaAdapter,
  takaro: TakaroWsClient,
): Promise<void> {
  const requestId = message.requestId;
  if (!requestId) return;
  try {
    const action = message.payload?.action;
    if (action !== 'getPlayerLocation') {
      logger.info(`Accepted Takaro request requestId=${requestId} action=${action ?? '<missing>'}`);
    }
    const result = await adapter.handleAction(action, message.payload?.args);
    takaro.sendResponse(requestId, result);
  } catch (err) {
    const messageText = err instanceof Error ? err.message : String(err);
    takaro.sendError(requestId, messageText);
  }
}

main().catch((err) => {
  logger.error(`Fatal startup error: ${err instanceof Error ? err.stack || err.message : String(err)}`);
  process.exit(1);
});
