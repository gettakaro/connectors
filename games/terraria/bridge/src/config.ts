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
  logExcludePatterns: string[];
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

/**
 * The values the shipped TakaroConfig.txt carries. Upgrades compare against these: a key the
 * freshly unpacked file still holds at its shipped value is one the operator never touched,
 * so the saved copy's value for it wins (configFiles.ts). The template test keeps this map
 * and bridge/TakaroConfig.txt in step.
 */
export const SHIPPED_VALUES: Readonly<Record<string, string>> = {
  configFormat: '2',
  registrationToken: '',
  identityToken: '',
  serverName: 'Terraria Server',
  serverChatName: 'Takaro',
  takaroWsUrl: 'wss://connect.takaro.io/',
  tshockBaseUrl: 'http://127.0.0.1:7878',
  tshockToken: '',
  httpPort: '3020',
  pollIntervalMs: '10000',
  logFiles: 'tshock/logs',
  logExcludePatterns: 'takaro-rest executed:,RestManager:',
  commandAllowlistExact: 'help,/help',
  commandAllowlistPrefixes: 'say,time',
  enableShutdown: 'false',
};

/**
 * Builds the settings from one set of file values plus the environment. An empty registration
 * token or missing TShock credentials are not errors: the bridge stays up, says what is
 * missing (missingSettings) and picks the values up when the file is saved.
 */
export function buildConfig(values: Record<string, string>, env: NodeJS.ProcessEnv = process.env): BridgeConfig {
  const serverName = values.serverName || SHIPPED_VALUES.serverName;
  return {
    registrationToken: env.TAKARO_REGISTRATION_TOKEN || values.registrationToken || '',
    identityToken: values.identityToken || serverName,
    serverName,
    serverChatName: values.serverChatName || serverName,
    takaroWsUrl: values.takaroWsUrl || SHIPPED_VALUES.takaroWsUrl,
    tshock: {
      baseUrl: (values.tshockBaseUrl || SHIPPED_VALUES.tshockBaseUrl).replace(/\/+$/, ''),
      token: env.TSHOCK_TOKEN || values.tshockToken || undefined,
      username: env.TSHOCK_USERNAME || values.tshockUsername || undefined,
      password: env.TSHOCK_PASSWORD || values.tshockPassword || undefined,
      timeoutMs: parseNumber(values.tshockTimeoutMs, 5000),
    },
    httpPort: parseNumber(values.httpPort, 3020),
    pollIntervalMs: parseNumber(values.pollIntervalMs, 10000),
    logFiles: resolveLogFiles(parseCsv(values.logFiles)),
    logExcludePatterns: resolveLogExcludePatterns(values.logExcludePatterns),
    commandAllowlistExact: parseCsv(values.commandAllowlistExact),
    commandAllowlistPrefixes: parseCsv(values.commandAllowlistPrefixes),
    enableShutdown: parseBoolean(values.enableShutdown, false),
  };
}

/** Reads one file as it stands, with no saved copy and no identity generation. */
export function loadConfig(configPath: string, env: NodeJS.ProcessEnv = process.env): BridgeConfig {
  if (!fs.existsSync(configPath)) {
    throw new Error(`Config file not found at ${path.resolve(configPath)}`);
  }
  return buildConfig(parseKeyValues(fs.readFileSync(configPath, 'utf8')), env);
}

export function hasTshockCredentials(config: BridgeConfig): boolean {
  return Boolean(config.tshock.token || (config.tshock.username && config.tshock.password));
}

function parseCsv(value: string | undefined): string[] {
  return (value || '')
    .split(',')
    .map((entry) => entry.trim())
    .filter(Boolean);
}

/**
 * Directory and glob entries are passed through verbatim so the tailer can re-resolve the
 * newest log on every poll. Resolving to a single file here would pin the bridge to the log
 * that happened to be newest at startup, and TShock opens a new one on every restart.
 */
function resolveLogFiles(entries: string[]): string[] {
  return entries;
}

/**
 * Lines matching these are never shipped to Takaro as `log` events.
 *
 * TShock logs every REST call, including the ones this bridge itself makes, so an unfiltered
 * tailer feeds its own traffic back to Takaro. On a live server that was 58% of all log lines
 * and it tripped Takaro's rate limiter (sustained 50 per 30s), which drops real gameplay
 * events — silent data loss behind a green health check.
 *
 * Match on the REST call signature rather than the logger name: TShock 6.1 emits these under
 * `Utils:` while other builds use `RestManager:`, so keying on the logger alone silently stops
 * filtering after a server upgrade. `takaro-rest executed:` is the bridge's own REST user and
 * is the stable part of the line.
 *
 * Set `logExcludePatterns=` (empty) to disable, or supply a comma-separated replacement list.
 */
const DEFAULT_LOG_EXCLUDE_PATTERNS = ['takaro-rest executed:', 'RestManager:'];

function resolveLogExcludePatterns(value: string | undefined): string[] {
  if (value == null) return [...DEFAULT_LOG_EXCLUDE_PATTERNS];
  return parseCsv(value);
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
