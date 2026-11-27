import { loadConfig } from './config.js';
import { DEBUG, logger } from './logger.js';
import { TakaroWsClient } from './ws/client.js';
import { LocalHttpServer } from './local/server.js';
import { rptTailFromEnv } from './local/rptTail.js';

async function main(): Promise<void> {
  const config = loadConfig(process.env.BRIDGE_CONFIG || 'TakaroConfig.txt');
  logger.info(`Starting DayZ ↔ Takaro bridge serverName='${config.serverName}'`);
  logger.info(`Takaro WS: ${config.takaroWsUrl}`);
  logger.info(`Mod HTTP: ${config.bindAddress}:${config.localPort}`);
  logger.info(`Identity token: ${config.identityToken}`);
  if (DEBUG) logger.info('DEBUG=1 — logging every WS frame and local HTTP request');

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
    config.bindAddress,
  );

  // F9: forward DayZ .RPT / script log lines to Takaro as `log` events when the
  // profiles directory is mounted (DAYZ_LOG_DIR). No-op when it is not set.
  const rptTail = rptTailFromEnv((ev) => {
    ws.sendGameEvent('log', { timestamp: ev.timestamp, msg: ev.msg });
  });
  if (rptTail) {
    local.setLogCounter(() => rptTail.forwarded());
    rptTail.start();
  } else {
    logger.info('DAYZ_LOG_DIR not set — no `log` events will be produced');
  }

  local.start();
  ws.connect();

  // M3 resilience fix (Docker sidecar only, opt-in): the sidecar joins the game
  // container's network namespace (network_mode: service:dayz). When the game
  // container restarts (e.g. after Takaro `shutdown`) it gets a NEW netns and the
  // sidecar keeps listening in the dead one, so every mod poll fails with curl
  // code 7 and Takaro sees the server as unreachable forever. If the mod has
  // polled before and then goes silent for BRIDGE_POLL_WATCHDOG_SEC, exit(1) so
  // the restart policy re-attaches the sidecar to the live namespace.
  const watchdogSec = parseInt(process.env.BRIDGE_POLL_WATCHDOG_SEC || '0', 10);
  if (Number.isFinite(watchdogSec) && watchdogSec > 0) {
    logger.info(`Poll watchdog enabled: exit if the mod stops polling for ${watchdogSec}s`);
    const t = setInterval(() => {
      const last = local.lastPollAtMs();
      if (last !== null && Date.now() - last > watchdogSec * 1000) {
        logger.warn(`Poll watchdog: no mod poll for ${Math.round((Date.now() - last) / 1000)}s — exiting so the container restarts into the game's current network namespace`);
        ws.shutdown();
        setTimeout(() => process.exit(1), 250);
      }
    }, 5000);
    t.unref?.();
  }

  const stop = (): void => {
    logger.info('Shutting down');
    rptTail?.stop();
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
