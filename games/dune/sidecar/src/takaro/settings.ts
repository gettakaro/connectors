import { randomUUID } from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

/**
 * The Takaro connection settings, and the only part of the configuration that changes while the sidecar runs.
 *
 * Sources, per key:
 *  - the environment, which wins at start exactly as before (a Docker install gets it from compose's `--env-file`);
 *  - the `.env` file the operator edits (`TAKARO_CONFIG_FILE`), re-read every few seconds. Once a key's value in the
 *    file differs from what the file said at start, the file wins for that key: Docker bakes the environment at
 *    container create, so the edited file is the newest word.
 *
 * Only these four keys are read from the file. Every other `DUNE_*` setting still comes from the environment alone and
 * needs a sidecar restart, so the file can never switch on a setting compose does not pass through.
 */
export const RELOADABLE_KEYS = ['TAKARO_WS_URL', 'TAKARO_REGISTRATION_TOKEN', 'TAKARO_IDENTITY_TOKEN', 'TAKARO_SERVER_NAME'] as const;
type ReloadableKey = (typeof RELOADABLE_KEYS)[number];

/** The identity every install used before identities were generated. Existing installs on it keep it. */
export const LEGACY_IDENTITY = 'dune';
export const IDENTITY_FILE_NAME = 'takaro-identity.json';
const DEFAULT_WS_URL = 'wss://connect.takaro.io/';
const DEFAULT_SERVER_NAME = 'Takaro Dev Dune';

type Env = Record<string, string | undefined>;

export interface TakaroSettings {
  wsUrl: string;
  registrationToken: string;
  identityToken: string;
  serverName: string;
}

/**
 * The subset of dotenv that compose's `--env-file` and a shell `set -a; . .env` agree on: `KEY=value`, optional
 * `export `, single or double quotes, and a ` #` comment after an unquoted value.
 */
export function parseDotenv(text: string): Record<string, string> {
  const out: Record<string, string> = {};
  for (const rawLine of text.split(/\r?\n/)) {
    const line = rawLine.trim();
    if (!line || line.startsWith('#')) continue;
    const match = /^(?:export\s+)?([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.*)$/.exec(line);
    if (!match) continue;
    const [, key, rest] = match;
    let value = rest;
    const quote = value[0];
    if ((quote === '"' || quote === "'") && value.indexOf(quote, 1) > 0) {
      value = value.slice(1, value.indexOf(quote, 1));
    } else {
      value = value.startsWith('#') ? '' : value.replace(/\s+#.*$/, '').trim();
    }
    out[key] = value;
  }
  return out;
}

/** `TAKARO_CONFIG_FILE`, else `.env` beside the unpacked `sidecar/` folder, where the README puts it. */
export function resolveConfigFile(env: Env, sidecarDir = defaultSidecarDir()): string {
  const explicit = env.TAKARO_CONFIG_FILE?.trim();
  if (explicit) return path.resolve(explicit);
  return path.resolve(sidecarDir, '..', '.env');
}

function defaultSidecarDir(): string {
  // dist/takaro/settings.js -> the sidecar folder
  return path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..');
}

export interface SettingsSourceOptions {
  env: Env;
  configFile: string;
  dataDir: string;
  /** Files whose presence in `dataDir` marks an install that ran before identities were generated. */
  legacyStateFiles: string[];
  readFile?: (file: string) => string;
  log?: { info: (m: string) => void; warn: (m: string) => void };
}

/**
 * Holds the effective settings and recomputes them when the config file's TEXT changes. Not shared across threads:
 * Node runs `poll()` on the event loop, so the only ordering concerns are the socket callbacks, which the client
 * handles by generation (see `TakaroWsClient.reconfigure`).
 */
export class TakaroSettingsSource {
  private readonly env: Env;
  private readonly readFile: (file: string) => string;
  private readonly log: { info: (m: string) => void; warn: (m: string) => void };
  private lastText: string | null = null;
  private lastReadError = '';
  private baseline: Partial<Record<ReloadableKey, string>> | null = null;
  private file: Partial<Record<ReloadableKey, string>> = {};
  private readonly overridden = new Set<ReloadableKey>();
  private settings: TakaroSettings;

  constructor(private readonly options: SettingsSourceOptions) {
    this.env = options.env;
    this.readFile = options.readFile ?? ((file) => fs.readFileSync(file, 'utf8'));
    this.log = options.log ?? { info: () => undefined, warn: () => undefined };
    this.readOnce();
    this.settings = this.compute();
  }

  get configFile(): string {
    return this.options.configFile;
  }

  /** Whether the config file existed on the last read. */
  hasConfigFile(): boolean {
    return this.lastText !== null;
  }

  current(): TakaroSettings {
    return { ...this.settings };
  }

  /** Re-reads the file. Returns the new settings when anything effective changed, else null. */
  poll(): TakaroSettings | null {
    if (!this.readOnce()) return null;
    const next = this.compute();
    const prev = this.settings;
    this.settings = next;
    const changed =
      next.wsUrl !== prev.wsUrl ||
      next.registrationToken !== prev.registrationToken ||
      next.identityToken !== prev.identityToken ||
      next.serverName !== prev.serverName;
    return changed ? { ...next } : null;
  }

  /** Reads the file; true when its text differs from the last successful read. A failed read keeps everything. */
  private readOnce(): boolean {
    let text: string;
    try {
      text = this.readFile(this.options.configFile);
    } catch (err) {
      const code = (err as NodeJS.ErrnoException).code;
      // A missing file is a normal install (environment only); any other error is a half-replaced file. Either way the
      // current settings stand, and the next tick tries again.
      const reason = code ?? (err as Error).message;
      // Said once per distinct problem, not every tick (an unreadable `chmod 600` file would otherwise flood the log).
      if (code !== 'ENOENT' && code !== 'ENOTDIR' && reason !== this.lastReadError) {
        this.log.warn(`Could not read ${this.options.configFile}: ${reason}; keeping the current Takaro settings`);
      }
      this.lastReadError = reason;
      return false;
    }
    this.lastReadError = '';
    if (text === this.lastText) return false;
    this.lastText = text;
    const parsed = parseDotenv(text);
    const values: Partial<Record<ReloadableKey, string>> = {};
    for (const key of RELOADABLE_KEYS) if (parsed[key] !== undefined) values[key] = parsed[key].trim();
    if (this.baseline === null) {
      this.baseline = values;
    } else {
      for (const key of RELOADABLE_KEYS) if ((values[key] ?? '') !== (this.baseline[key] ?? '')) this.overridden.add(key);
    }
    this.file = values;
    return true;
  }

  private value(key: ReloadableKey): string {
    if (this.overridden.has(key)) return this.file[key] ?? '';
    const fromEnv = this.env[key]?.trim();
    return fromEnv ? fromEnv : (this.file[key] ?? '');
  }

  private compute(): TakaroSettings {
    return {
      wsUrl: this.value('TAKARO_WS_URL') || DEFAULT_WS_URL,
      registrationToken: this.value('TAKARO_REGISTRATION_TOKEN'),
      identityToken: this.resolveIdentity(this.value('TAKARO_IDENTITY_TOKEN')),
      serverName: this.value('TAKARO_SERVER_NAME') || DEFAULT_SERVER_NAME,
    };
  }

  /**
   * Configured identity > the one saved in the data volume > `dune` for an install that already has state from before
   * identities were generated > a new UUID. Whatever is chosen is saved, so it survives upgrades and the key being
   * removed from the file later; an identity an install already uses is never replaced.
   */
  private resolveIdentity(configured: string): string {
    const idFile = path.join(this.options.dataDir, IDENTITY_FILE_NAME);
    const saved = readSavedIdentity(idFile);
    let identity: string;
    let why: string;
    if (configured) {
      identity = configured;
      why = 'configured';
    } else if (saved) {
      identity = saved;
      why = `saved in ${idFile}`;
    } else if (this.options.legacyStateFiles.some((f) => fs.existsSync(f))) {
      identity = LEGACY_IDENTITY;
      why = `existing install (state in ${this.options.dataDir}, no identity set)`;
    } else {
      identity = randomUUID();
      why = 'generated for this new install';
    }
    if (identity !== saved) {
      try {
        fs.mkdirSync(this.options.dataDir, { recursive: true });
        const tmp = `${idFile}.tmp`;
        fs.writeFileSync(tmp, `${JSON.stringify({ identityToken: identity, savedAt: new Date().toISOString() }, null, 2)}\n`);
        fs.renameSync(tmp, idFile);
      } catch (err) {
        this.log.warn(`Could not save the Takaro identity to ${idFile}: ${(err as Error).message}. Keep TAKARO_DATA_DIR writable, or set TAKARO_IDENTITY_TOKEN.`);
      }
    }
    if (identity !== this.settings?.identityToken) this.log.info(`Takaro identity: ${identity} (${why})`);
    return identity;
  }
}

function readSavedIdentity(file: string): string {
  try {
    const parsed = JSON.parse(fs.readFileSync(file, 'utf8')) as { identityToken?: unknown };
    return typeof parsed.identityToken === 'string' ? parsed.identityToken.trim() : '';
  } catch {
    return '';
  }
}
