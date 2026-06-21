import fs from 'node:fs';
import path from 'node:path';

export interface RconConfig {
  host: string;
  port: number;
  password: string;
  timeoutMs: number;
}

export interface BridgeConfig {
  registrationToken: string;
  identityToken: string;
  serverName: string;
  takaroWsUrl: string;
  rcon: RconConfig;
  databasePath: string | null;
  itemCatalogPath: string | null;
  httpPort: number;
  pollIntervalMs: number;
  enableLogEvents: boolean;
  logFiles: string[];
}

function parseBoolean(value: string | undefined, fallback: boolean): boolean {
  if (value == null || value === '') return fallback;
  return ['1', 'true', 'yes', 'on'].includes(value.trim().toLowerCase());
}

function parseNumber(value: string | undefined, fallback: number): number {
  if (value == null || value === '') return fallback;
  const parsed = Number.parseInt(value, 10);
  return Number.isFinite(parsed) ? parsed : fallback;
}

function parseKeyValues(raw: string): Record<string, string> {
  const values: Record<string, string> = {};
  for (const line of raw.split(/\r?\n/)) {
    const trimmed = line.trim();
    if (!trimmed || trimmed.startsWith('#')) continue;
    const eq = trimmed.indexOf('=');
    if (eq < 0) continue;
    values[trimmed.slice(0, eq).trim()] = trimmed.slice(eq + 1).trim();
  }
  return values;
}

function requireValue(values: Record<string, string>, key: string): string {
  const value = values[key];
  if (!value) throw new Error(`Missing required config: ${key}`);
  return value;
}

export function loadConfig(configPath = process.env.BRIDGE_CONFIG || 'TakaroConfig.txt'): BridgeConfig {
  if (!fs.existsSync(configPath)) {
    throw new Error(`Config file not found at ${path.resolve(configPath)}`);
  }

  const values = parseKeyValues(fs.readFileSync(configPath, 'utf8'));
  const serverName = requireValue(values, 'serverName');

  return {
    registrationToken: requireValue(values, 'registrationToken'),
    identityToken: values.identityToken || serverName,
    serverName,
    takaroWsUrl: values.takaroWsUrl || 'wss://connect.takaro.io/',
    rcon: {
      host: requireValue(values, 'rconHost'),
      port: parseNumber(requireValue(values, 'rconPort'), 25575),
      password: requireValue(values, 'rconPassword'),
      timeoutMs: parseNumber(values.rconTimeoutMs, 5000),
    },
    databasePath: values.databasePath || values.conanDbPath || null,
    itemCatalogPath: values.itemCatalogPath || null,
    httpPort: parseNumber(values.httpPort, 3010),
    pollIntervalMs: parseNumber(values.pollIntervalMs, 10000),
    enableLogEvents: parseBoolean(values.enableLogEvents, true),
    logFiles: (values.logFiles || '')
      .split(',')
      .map((entry) => entry.trim())
      .filter(Boolean),
  };
}
