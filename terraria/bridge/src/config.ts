import fs from 'node:fs';
import path from 'node:path';

export interface TShockConfig {
  baseUrl: string;
  token?: string;
  username?: string;
  password?: string;
  timeoutMs: number;
}

export interface BridgeConfig {
  registrationToken: string;
  identityToken: string;
  serverName: string;
  serverChatName: string;
  takaroWsUrl: string;
  tshock: TShockConfig;
  httpPort: number;
  pollIntervalMs: number;
  logFiles: string[];
  commandAllowlistExact: string[];
  commandAllowlistPrefixes: string[];
  enableShutdown: boolean;
}

export function parseKeyValues(raw: string): Record<string, string> {
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

export function loadConfig(
  configPath = process.env.BRIDGE_CONFIG || 'TakaroConfig.txt',
  env: NodeJS.ProcessEnv = process.env,
): BridgeConfig {
  if (!fs.existsSync(configPath)) {
    throw new Error(`Config file not found at ${path.resolve(configPath)}`);
  }

  const values = parseKeyValues(fs.readFileSync(configPath, 'utf8'));
  const serverName = requireValue(values, 'serverName');
  const registrationToken = env.TAKARO_REGISTRATION_TOKEN || values.registrationToken;
  if (!registrationToken) throw new Error('Missing required config: registrationToken');

  const tshockToken = env.TSHOCK_TOKEN || values.tshockToken;
  const tshockUsername = env.TSHOCK_USERNAME || values.tshockUsername;
  const tshockPassword = env.TSHOCK_PASSWORD || values.tshockPassword;
  if (!tshockToken && !(tshockUsername && tshockPassword)) {
    throw new Error('Missing required config: tshockToken or tshockUsername/tshockPassword');
  }

  return {
    registrationToken,
    identityToken: values.identityToken || serverName,
    serverName,
    serverChatName: values.serverChatName || serverName,
    takaroWsUrl: values.takaroWsUrl || 'wss://connect.takaro.io/',
    tshock: {
      baseUrl: requireValue(values, 'tshockBaseUrl').replace(/\/+$/, ''),
      token: tshockToken,
      username: tshockUsername,
      password: tshockPassword,
      timeoutMs: parseNumber(values.tshockTimeoutMs, 5000),
    },
    httpPort: parseNumber(values.httpPort, 3020),
    pollIntervalMs: parseNumber(values.pollIntervalMs, 10000),
    logFiles: resolveLogFiles(parseCsv(values.logFiles)),
    commandAllowlistExact: parseCsv(values.commandAllowlistExact),
    commandAllowlistPrefixes: parseCsv(values.commandAllowlistPrefixes),
    enableShutdown: parseBoolean(values.enableShutdown, false),
  };
}

function parseCsv(value: string | undefined): string[] {
  return (value || '')
    .split(',')
    .map((entry) => entry.trim())
    .filter(Boolean);
}

function resolveLogFiles(entries: string[]): string[] {
  return entries.flatMap((entry) => {
    if (!fs.existsSync(entry) || !fs.statSync(entry).isDirectory()) return [entry];

    const latest = fs.readdirSync(entry)
      .filter((file) => file.endsWith('.log'))
      .map((file) => path.join(entry, file))
      .filter((file) => fs.statSync(file).isFile())
      .sort((left, right) => fs.statSync(right).mtimeMs - fs.statSync(left).mtimeMs)[0];
    return latest ? [latest] : [];
  });
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

function requireValue(values: Record<string, string>, key: string): string {
  const value = values[key];
  if (!value) throw new Error(`Missing required config: ${key}`);
  return value;
}
