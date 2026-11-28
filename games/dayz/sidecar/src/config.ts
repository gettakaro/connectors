import fs from 'node:fs';
import path from 'node:path';

export interface BridgeConfig {
  registrationToken: string;
  serverName: string;
  identityToken: string;
  takaroWsUrl: string;
  localPort: number;
  /** Bind address for the mod-facing HTTP server. 127.0.0.1 in the shared-netns rig. */
  bindAddress: string;
  debug: boolean;
}

const DEFAULT_WS_URL = 'wss://connect.takaro.io/';

function readConfigFile(configPath: string): Record<string, string> {
  const kv: Record<string, string> = {};
  if (!fs.existsSync(configPath)) return kv;

  const raw = fs.readFileSync(configPath, 'utf8');
  for (const line of raw.split(/\r?\n/)) {
    const trimmed = line.trim();
    if (!trimmed || trimmed.startsWith('#')) continue;
    const eq = trimmed.indexOf('=');
    if (eq < 0) continue;
    kv[trimmed.slice(0, eq).trim()] = trimmed.slice(eq + 1).trim();
  }
  return kv;
}

function envOr(name: string, fallback: string | undefined): string | undefined {
  const v = process.env[name];
  if (v !== undefined && v !== '') return v;
  return fallback;
}

/**
 * Loads bridge configuration.
 *
 * Precedence: environment variables OVERRIDE TakaroConfig.txt. The config file
 * is optional as long as the environment supplies everything required
 * (TAKARO_REGISTRATION_TOKEN + a server name / identity token), which is how the
 * containerised dev rig runs it.
 */
export function loadConfig(configPath = 'TakaroConfig.txt'): BridgeConfig {
  const resolved = path.resolve(configPath);
  const fileExists = fs.existsSync(resolved);
  const kv = readConfigFile(resolved);

  const registrationToken = envOr('TAKARO_REGISTRATION_TOKEN', kv.registrationToken);
  const serverName = envOr('TAKARO_SERVER_NAME', kv.serverName);
  const identityToken = envOr('TAKARO_IDENTITY_TOKEN', kv.identityToken) || serverName;
  const takaroWsUrl = envOr('TAKARO_WS_URL', kv.takaroWsUrl) || DEFAULT_WS_URL;
  const localPortRaw = envOr('BRIDGE_LOCAL_PORT', kv.localPort) || '8088';

  const missing: string[] = [];
  if (!registrationToken) missing.push('registrationToken / TAKARO_REGISTRATION_TOKEN');
  if (!serverName) missing.push('serverName / TAKARO_SERVER_NAME');

  if (missing.length > 0) {
    const where = fileExists
      ? `Config file ${resolved} is missing: ${missing.join(', ')}.`
      : `No config file at ${resolved} and the environment is incomplete. Missing: ${missing.join(', ')}.`;
    throw new Error(
      `${where} Set the environment variables (TAKARO_REGISTRATION_TOKEN, TAKARO_SERVER_NAME, optionally TAKARO_IDENTITY_TOKEN/TAKARO_WS_URL/BRIDGE_LOCAL_PORT) or copy TakaroConfig.example.txt and fill it in.`,
    );
  }

  const localPort = parseInt(localPortRaw, 10);
  if (!Number.isFinite(localPort) || localPort <= 0 || localPort > 65535) {
    throw new Error(`Invalid local port '${localPortRaw}' (BRIDGE_LOCAL_PORT / localPort)`);
  }

  return {
    registrationToken: registrationToken!,
    serverName: serverName!,
    identityToken: identityToken!,
    takaroWsUrl,
    localPort,
    bindAddress: envOr('BRIDGE_BIND_ADDR', kv.bindAddress) || '127.0.0.1',
    debug: /^(1|true|yes|on)$/i.test(process.env.DEBUG || ''),
  };
}
