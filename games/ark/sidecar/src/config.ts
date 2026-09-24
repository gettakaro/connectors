import path from 'node:path';

export interface Config {
  takaroWsUrl: string;
  identityToken: string;
  registrationToken: string;
  serverName: string;
  nativeUrl: string;
  nativeToken: string;
  cursorFile: string;
  banMetadataFile: string;
  pollIntervalMs: number;
  timeoutMs: number;
  healthHost: string;
  healthPort: number;
}

export function loadConfig(env: NodeJS.ProcessEnv = process.env): Config {
  const nativeToken = env.ARK_NATIVE_TOKEN?.trim();
  if (!nativeToken) throw new Error('Missing required ARK_NATIVE_TOKEN');
  const nativeUrl = (env.ARK_NATIVE_URL || 'http://127.0.0.1:18891').replace(/\/+$/, '');
  const url = new URL(nativeUrl);
  if (url.protocol !== 'http:' || url.hostname !== '127.0.0.1' || url.pathname !== '/' ||
      url.username || url.password || url.search || url.hash) {
    throw new Error('ARK_NATIVE_URL must be a plain http://127.0.0.1 loopback origin');
  }
  const int = (name: string, fallback: number): number => {
    const parsed = Number(env[name]);
    return Number.isSafeInteger(parsed) && parsed > 0 ? parsed : fallback;
  };
  return {
    takaroWsUrl: env.TAKARO_WS_URL || 'wss://connect.takaro.io/',
    identityToken: env.TAKARO_IDENTITY_TOKEN || 'ark',
    registrationToken: env.TAKARO_REGISTRATION_TOKEN || '',
    serverName: env.TAKARO_SERVER_NAME || 'ARK native',
    nativeUrl,
    nativeToken,
    cursorFile: env.TAKARO_CURSOR_FILE || './data/event-cursor.json',
    banMetadataFile: env.TAKARO_BAN_METADATA_FILE ||
      path.join(path.dirname(env.TAKARO_CURSOR_FILE || './data/event-cursor.json'), 'ban-metadata.json'),
    pollIntervalMs: int('ARK_POLL_INTERVAL_MS', 1000),
    timeoutMs: int('ARK_NATIVE_TIMEOUT_MS', 10000),
    healthHost: env.SIDECAR_HEALTH_HOST || '127.0.0.1',
    healthPort: int('SIDECAR_HEALTH_PORT', 18892),
  };
}
