import path from 'node:path';
export interface SidecarConfig {
  takaroWsUrl: string;
  identityToken: string;
  registrationToken: string;
  serverName: string;
  pluginBaseUrl: string;
  pluginToken: string;
  pluginTimeoutMs: number;
  pollIntervalMs: number;
  cursorFile: string;
  onlineFile: string;
  logFile: string;
  logTailMode: 'auto' | 'always' | 'never';
  logEvents: 'all' | 'filtered' | 'none';
  healthPort: number;
  healthHost: string;
  reconnectBaseMs: number;
  reconnectMaxMs: number;
}

type Env = Record<string, string | undefined>;

export function loadConfig(env: Env = process.env): SidecarConfig {
  const registrationToken = env.TAKARO_REGISTRATION_TOKEN?.trim() ?? '';
  const pluginToken = env.TAKARO_PLUGIN_TOKEN?.trim() ?? '';
  if (!pluginToken) throw new Error('Missing required env TAKARO_PLUGIN_TOKEN');

  const mode = (env.ENSHROUDED_LOG_TAIL || 'auto').toLowerCase();
  if (mode !== 'auto' && mode !== 'always' && mode !== 'never') {
    throw new Error(`ENSHROUDED_LOG_TAIL must be auto|always|never, got '${mode}'`);
  }

  const logEvents = (env.ENSHROUDED_LOG_EVENTS || 'filtered').toLowerCase();
  if (logEvents !== 'all' && logEvents !== 'filtered' && logEvents !== 'none') {
    throw new Error(`ENSHROUDED_LOG_EVENTS must be all|filtered|none, got '${logEvents}'`);
  }

  return {
    takaroWsUrl: env.TAKARO_WS_URL || 'wss://connect.takaro.io/',
    identityToken: env.TAKARO_IDENTITY_TOKEN || 'takaro-dev-enshrouded',
    registrationToken,
    serverName: env.TAKARO_SERVER_NAME || 'Takaro Dev Enshrouded',
    pluginBaseUrl: (env.TAKARO_PLUGIN_URL || 'http://127.0.0.1:18890').replace(/\/+$/, ''),
    pluginToken,
    pluginTimeoutMs: int(env.TAKARO_PLUGIN_TIMEOUT_MS, 10000),
    pollIntervalMs: int(env.TAKARO_POLL_INTERVAL_MS, 1000),
    cursorFile: env.TAKARO_CURSOR_FILE || './data/event-cursor.json',
    onlineFile: env.TAKARO_ONLINE_FILE || path.join(path.dirname(env.TAKARO_CURSOR_FILE || './data/event-cursor.json'), 'online-players.json'),
    logFile: env.ENSHROUDED_LOG_FILE || '/opt/enshrouded/server/logs/enshrouded_server.log',
    logTailMode: mode,
    logEvents,
    healthPort: int(env.SIDECAR_HEALTH_PORT, 18891),
    healthHost: env.SIDECAR_HEALTH_HOST || '127.0.0.1',
    reconnectBaseMs: int(env.TAKARO_RECONNECT_BASE_MS, 2000),
    reconnectMaxMs: int(env.TAKARO_RECONNECT_MAX_MS, 60000),
  };
}

function int(value: string | undefined, fallback: number): number {
  if (value == null || value.trim() === '') return fallback;
  const parsed = Number.parseInt(value, 10);
  return Number.isFinite(parsed) && parsed >= 0 ? parsed : fallback;
}
