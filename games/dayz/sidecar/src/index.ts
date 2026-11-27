import { loadConfig } from './config.js';
import { DEBUG, logger } from './logger.js';
import { TakaroWsClient } from './ws/client.js';
import { LocalHttpServer } from './local/server.js';
import { rptTailFromEnv } from './local/rptTail.js';
import { mapServiceFromEnv } from './map/service.js';
import { buildManifest } from './ws/capabilities.js';
import { PollWatchdog } from './watchdog.js';

async function main(): Promise<void> {
  const config = loadConfig(process.env.BRIDGE_CONFIG || 'TakaroConfig.txt');
  logger.info(`Starting DayZ ↔ Takaro bridge serverName='${config.serverName}'`);
  logger.info(`Takaro WS: ${config.takaroWsUrl}`);
  logger.info(`Mod HTTP: ${config.bindAddress}:${config.localPort}`);
  logger.info(`Identity token: ${config.identityToken}`);
  if (DEBUG) logger.info('DEBUG=1 — logging every WS frame and local HTTP request');

  // Map tiles come from the mod's world survey under the profiles directory.
  const map = mapServiceFromEnv();

  // Protocol v1 (map Dimensions, acknowledged events) is offered by default; Takaro picks the
  // version. TAKARO_PROTOCOL=0 sends the old identify.
  const offerV1 = (process.env.TAKARO_PROTOCOL ?? '1').trim() !== '0';
  logger.info(offerV1 ? 'Takaro protocol: offering versions 0 and 1' : 'Takaro protocol: version 0 only (TAKARO_PROTOCOL=0)');
  const ws = new TakaroWsClient(
    config.takaroWsUrl,
    {
      identityToken: config.identityToken,
      registrationToken: config.registrationToken,
      name: config.serverName,
    },
    { offerV1, manifest: () => buildManifest(map?.dimension() ?? null) },
  );
  if (offerV1 && map) {
    // The survey can finish (or be redone) after identify: declare the new map then.
    const refresh = setInterval(() => ws.refreshCapabilities(), 30_000);
    refresh.unref?.();
  }
  logger.info(map ? 'Map tiles: serving getMapInfo/getMapTile from the world survey' : 'Map tiles: off (no DAYZ_LOG_DIR/DAYZ_MAP_DIR)');
  const local = new LocalHttpServer(
    config.localPort,
    ws,
    config.serverName,
    config.identityToken,
    config.bindAddress,
    map,
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

  // Docker sidecar only, opt-in: exit (so the restart policy re-attaches the sidecar) when the
  // game container restarted into a new network namespace. See watchdog.ts for why mod silence
  // alone is not enough.
  const watchdogSec = parseInt(process.env.BRIDGE_POLL_WATCHDOG_SEC || '0', 10);
  if (Number.isFinite(watchdogSec) && watchdogSec > 0) {
    logger.info(`Poll watchdog enabled: exit if only loopback is left in this network namespace for ${watchdogSec}s`);
    const tickMs = 5000;
    const watchdog = new PollWatchdog({ thresholdMs: watchdogSec * 1000, tickMs });
    let warnedSilent = false;
    let exiting = false;
    const t = setInterval(() => {
      if (exiting) return;
      const r = watchdog.tick(Date.now(), local.lastPollAtMs());
      if (r.stalledMs > 0) logger.warn(`Poll watchdog: bridge event loop stalled for ${Math.round(r.stalledMs / 1000)}s; not counted as mod silence`);
      if (r.verdict === 'exit') {
        exiting = true;
        logger.warn(`Poll watchdog: only loopback left in this network namespace for ${watchdogSec}s (no mod poll for ${Math.round(r.silentMs / 1000)}s) — exiting so the container restarts into the game's current one`);
        ws.shutdown();
        setTimeout(() => process.exit(1), 250);
      } else if (r.verdict === 'silent') {
        if (!warnedSilent) logger.warn(`Poll watchdog: no mod poll for ${Math.round(r.silentMs / 1000)}s, but the network namespace is alive (game hung or busy) — staying up`);
        warnedSilent = true;
      } else {
        warnedSilent = false;
      }
    }, tickMs);
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
