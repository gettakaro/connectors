import { type BridgeConfig, hasTshockCredentials } from './config.js';
import { locateConfig } from './configFiles.js';
import { ConfigSource, type LoadedConfig } from './configSource.js';
import { normalizeGameEvent } from './events/normalizeEvent.js';
import { PlayerPoller } from './events/playerPoller.js';
import { PresenceGate } from './events/presenceGate.js';
import { HealthServer } from './health/server.js';
import { logger } from './logger.js';
import { LogTailer } from './logs/logTailer.js';
import { TakaroWsClient } from './takaro/client.js';
import { logConnectionState } from './takaro/connectionLog.js';
import { identifyErrorSummary, identifyErrorText } from './takaro/identifyError.js';
import type { GameEventType, RequestPayload, WsMessage } from './takaro/protocol.js';
import { TerrariaAdapter } from './terraria/adapter.js';
import { TShockClient } from './tshock/client.js';

const CONFIG_POLL_MS = 5000;

async function main(): Promise<void> {
  const source = new ConfigSource(locateConfig());
  const configPath = source.location.userPath;
  logger.info(`Terraria bridge config: ${configPath}`);
  let stopping = false;
  let stop: () => Promise<void> = async () => {
    stopping = true;
    process.exit(0);
  };
  process.on('SIGINT', () => void stop());
  process.on('SIGTERM', () => void stop());

  // A missing or empty file at startup is waited for, not fatal.
  let loaded: LoadedConfig | null = await source.poll();
  while (!loaded && !stopping) {
    await new Promise((resolve) => setTimeout(resolve, CONFIG_POLL_MS));
    loaded = await source.poll();
  }
  if (!loaded) return;
  let config = loaded.config;
  logIdentity(loaded);
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

  // Reachability from the startup probe only. It seeds /health for the window before the
  // first poll completes; once the poller has an outcome, the poller is authoritative.
  let startupReachable = false;
  const presence = new PresenceGate();
  const emit = (type: GameEventType, data: unknown): void => {
    const event = normalizeGameEvent(type, data);
    if (!presence.accept(event)) return;
    const sent = takaro.sendGameEvent(event.type, event.data);
    if (!sent) logger.warn(`Takaro event dropped: ${type}`);
  };
  const poller = new PlayerPoller(
    () => adapter.getPlayers(),
    (event) => emit(event.type, event.data),
    config.pollIntervalMs,
  );
  const tailers = config.logFiles.map(
    (file) => new LogTailer(file, (event) => emit(event.type, event.data), undefined, undefined, config.logExcludePatterns),
  );
  // The poller runs every pollIntervalMs and is the only component that continuously
  // exercises the game server, so its latest outcome is the freshest reachability truth
  // available. Before it has produced one (startup, or right after a Takaro reconnect
  // resets it) fall back to the startup probe so a healthy server is not reported down.
  const isTshockReachable = (): boolean => poller.lastPollOk ?? startupReachable;
  const health = new HealthServer(config.httpPort, () => {
    const tshockReachable = isTshockReachable();
    return {
      // A health endpoint that returns ok during a total game-server outage is worse than
      // no endpoint at all, because it is trusted. ok tracks reachability.
      ok: tshockReachable,
      takaroIdentified: takaro.identified(),
      gameServerId: takaro.getGameServerId(),
      tshockReachable,
      lastPollAt: poller.lastPollAt,
    };
  });

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
    // Only the summary: Takaro's error object can carry its own request, auth header included.
    logger.error(`Takaro identify error: ${identifyErrorSummary(payload)}`);
    banner([
      `Takaro rejected identify: ${identifyErrorText(payload)}`,
      ...(process.env.TAKARO_REGISTRATION_TOKEN
        ? ['Check TAKARO_REGISTRATION_TOKEN in the environment; it overrides the config file.']
        : [
          `Check registrationToken in ${configPath}`,
          'and save it. The bridge reconnects within a few seconds, no restart needed.',
        ]),
    ]);
  });
  // The connection state goes to the log through one place: `takaro-maint verify` reads
  // those two lines out of this bridge's log to prove the handshake and the reconnect.
  logConnectionState(takaro);
  takaro.on('identified', () => {
    presence.reset();
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
  startupReachable = Boolean((reachability as { connectable?: boolean }).connectable);
  logger.info(`Terraria bridge health: http://127.0.0.1:${health.port()}/health`);
  announceMissing(config, configPath, undefined, Boolean(loaded.identityNotSaved));
  takaro.connect();

  const apply = (next: LoadedConfig): void => {
    const previous = config;
    config = next.config;
    logIdentity(next);
    tshock.updateConfig(config.tshock);
    if (JSON.stringify(previous.tshock) !== JSON.stringify(config.tshock)) {
      // /health falls back to the startup probe until the poller runs, so probe again.
      void adapter.handleAction('testReachability', {}).then((result) => {
        startupReachable = Boolean((result as { connectable?: boolean }).connectable);
      });
    }
    adapter.updateOptions({
      commandAllowlistExact: config.commandAllowlistExact,
      commandAllowlistPrefixes: config.commandAllowlistPrefixes,
      enableShutdown: config.enableShutdown,
      serverChatName: config.serverChatName,
    });
    const needsRestart = restartOnlyChanges(previous, config);
    if (needsRestart.length) {
      logger.warn(`${needsRestart.join(', ')} changed in ${configPath}; restart the bridge to apply ${needsRestart.length > 1 ? 'them' : 'it'}.`);
    }
    const reconnecting = takaro.reconfigure(config.takaroWsUrl, {
      identityToken: config.identityToken,
      registrationToken: config.registrationToken,
      name: config.serverName,
    });
    if (reconnecting && config.registrationToken) {
      logger.info(`Takaro connection settings changed in ${configPath}; connecting now`);
    }
    announceMissing(config, configPath, previous, Boolean(next.identityNotSaved));
  };

  let polling = false;
  const watcher = setInterval(() => {
    if (polling || stopping) return;
    polling = true;
    source.poll()
      .then((next) => {
        if (next && !stopping) apply(next);
      })
      .catch((err) => logger.error(`Config reload failed: ${err instanceof Error ? err.message : String(err)}`))
      .finally(() => {
        polling = false;
      });
  }, CONFIG_POLL_MS);

  stop = async (): Promise<void> => {
    if (stopping) return;
    stopping = true;
    logger.info('Shutting down Terraria Takaro bridge');
    clearInterval(watcher);
    poller.stop();
    for (const tailer of tailers) tailer.stop();
    takaro.shutdown();
    await health.stop();
    setTimeout(() => process.exit(0), 100);
  };
}

function logIdentity(loaded: LoadedConfig): void {
  if (loaded.identityNotSaved) {
    banner([
      'identityToken could not be saved, the server is not connected to Takaro.',
      loaded.identityNotSaved,
      'and save it. The bridge connects within a few seconds, no restart needed.',
    ]);
  }
  if (loaded.identitySource === 'serverName') {
    logger.info(`identityToken is not set; using serverName "${loaded.config.identityToken}" as before and saving it as identityToken`);
  }
}

/** Settings read once at startup. */
function restartOnlyChanges(previous: BridgeConfig, next: BridgeConfig): string[] {
  const changed: string[] = [];
  if (previous.httpPort !== next.httpPort) changed.push('httpPort');
  if (previous.pollIntervalMs !== next.pollIntervalMs) changed.push('pollIntervalMs');
  if (previous.logFiles.join(',') !== next.logFiles.join(',')) changed.push('logFiles');
  if (previous.logExcludePatterns.join(',') !== next.logExcludePatterns.join(',')) changed.push('logExcludePatterns');
  return changed;
}

/** The banners for what keeps the bridge from working, each once per time it goes missing. */
function announceMissing(config: BridgeConfig, configPath: string, previous?: BridgeConfig, blocked = false): void {
  if (!blocked && !config.registrationToken && (!previous || previous.registrationToken)) {
    banner([
      'RegistrationToken not set, the server is not connected to Takaro.',
      `Paste the registration token from Takaro into registrationToken= in ${configPath}`,
      'and save it. The bridge connects within a few seconds, no restart needed.',
    ]);
  }
  if (!hasTshockCredentials(config) && (!previous || hasTshockCredentials(previous))) {
    banner([
      'tshockToken not set, the bridge cannot reach the TShock REST API.',
      `Paste a TShock application REST token into tshockToken= in ${configPath}`,
      'and save it. The bridge picks it up within a few seconds, no restart needed.',
    ]);
  }
}

function banner(lines: string[]): void {
  const rule = '*'.repeat(73);
  logger.warn([rule, ...lines.map((line) => `  ${line}`), rule].join('\n'));
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

// Defense in depth: a transient game-server outage must never kill the bridge, but the
// rejection is still logged loudly (with a stack) so real bugs stay visible instead of
// being silently swallowed.
process.on('unhandledRejection', (reason) => {
  logger.error(`Unhandled promise rejection: ${reason instanceof Error ? reason.stack || reason.message : String(reason)}`);
});

main().catch((err) => {
  logger.error(`Fatal startup error: ${err instanceof Error ? err.stack || err.message : String(err)}`);
  process.exit(1);
});
