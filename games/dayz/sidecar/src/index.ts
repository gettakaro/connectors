import { loadConfig } from './config.js';
import { logger } from './logger.js';
import { TakaroWsClient } from './ws/client.js';
import { LocalHttpServer } from './local/server.js';

async function main(): Promise<void> {
  const config = loadConfig(process.env.BRIDGE_CONFIG || 'TakaroConfig.txt');
  logger.info(`Starting DayZ ↔ Takaro bridge serverName='${config.serverName}'`);
  logger.info(`Takaro WS: ${config.takaroWsUrl}`);
  logger.info(`Mod HTTP: 127.0.0.1:${config.localPort}`);

  const ws = new TakaroWsClient(config.takaroWsUrl, {
    identityToken: config.identityToken,
    registrationToken: config.registrationToken,
    name: config.serverName,
  });

  const local = new LocalHttpServer(
    config.localPort,
    ws,
    config.serverName,
    config.identityToken,
  );

  local.start();
  ws.connect();

  const stop = (): void => {
    logger.info('Shutting down');
    local.stop();
    ws.shutdown();
    setTimeout(() => process.exit(0), 250);
  };
  process.on('SIGINT', stop);
  process.on('SIGTERM', stop);
  process.on('uncaughtException', (err) => {
    logger.error(`Uncaught: ${err.stack || err.message}`);
  });
  process.on('unhandledRejection', (reason) => {
    logger.error(`Unhandled rejection: ${String(reason)}`);
  });
}

main().catch((err) => {
  logger.error(`Fatal startup: ${err}`);
  process.exit(1);
});
