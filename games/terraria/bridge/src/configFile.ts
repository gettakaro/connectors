import { randomUUID } from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { type BridgeConfig, buildConfig, defaultServerName, parseKeyValues } from './config.js';
import { logger } from './logger.js';

export const CONFIG_FILE_NAME = 'TakaroConfig.txt';

/**
 * The one config file: `BRIDGE_CONFIG` when set, else `TakaroConfig.txt` beside the bridge
 * folder, which an upgrade replaces as a whole and so never holds the config.
 */
export function locateConfig(env: NodeJS.ProcessEnv = process.env, cwd = process.cwd(), root = bridgeRoot()): string {
  if (env.BRIDGE_CONFIG) return path.resolve(cwd, env.BRIDGE_CONFIG);
  return path.join(path.dirname(root), CONFIG_FILE_NAME);
}

/** The bridge folder: the one holding dist/ and package.json. */
function bridgeRoot(): string {
  return path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
}

/** The file the bridge creates when there is none. The README shows it with both values empty. */
export function configTemplate(identityToken: string, serverName: string): string {
  return `# Takaro Terraria bridge settings. The bridge reads this file every few seconds:
# paste the registration token and save, no restart needed.
# identityToken is this server's identity in Takaro; the bridge fills it in when empty.
# An empty serverName means "Terraria Server (<first 8 characters of identityToken>)".
# Relative paths (logFiles, mapDir) start at the folder holding this file: the server folder.

# Takaro connection
registrationToken=
identityToken=${identityToken}
serverName=${serverName}
serverChatName=Takaro
takaroWsUrl=wss://connect.takaro.io/

# TShock REST API (RestApiEnabled in tshock/config.json). Instead of a token you may set
# tshockUsername= and tshockPassword=.
tshockBaseUrl=http://127.0.0.1:7878
tshockToken=

# Local status server (GET /health, GET /coverage) and player poll interval. Restart to apply.
httpPort=3020
pollIntervalMs=10000

# The TShock log DIRECTORY to tail (a directory follows TShock's log rotation), and the
# lines never sent to Takaro (the bridge's own REST calls). Restart to apply.
logFiles=tshock/logs
logExcludePatterns=takaro-rest executed:,RestManager:

# Console commands Takaro may run, by exact match and by prefix. Shutdown is off by default.
commandAllowlistExact=help,/help
commandAllowlistPrefixes=say,time
enableShutdown=false

# The world map: the plugin renders the loaded world into mapDir (<TShock folder>/takaro) and
# the bridge shows it on Takaro's map. protocolVersion=1 speaks Takaro protocol 1 when Takaro
# offers it (the map, events resent after a reconnect); 0 stays on version 0. Restart to apply.
# The plugin renders the map at world load, every TAKARO_MAP_INTERVAL_MINUTES (environment of
# the TShock server: default 30, 0 = off) and when an admin runs /takaromap.
mapDir=tshock/takaro
protocolVersion=1
`;
}

export interface LoadedConfig {
  config: BridgeConfig;
  /** Set when the bridge must not connect: a generated identity could not be written to the file. */
  identityNotSaved?: string;
}

type Log = Pick<typeof logger, 'info' | 'warn'>;

/**
 * Reads the config file and tells the caller when its text changed. Creates the file when it
 * is missing and writes a generated identity into it when it has none; it writes nothing
 * else. A file that is empty or still being written keeps the settings in use.
 */
export class ConfigSource {
  private lastText: string | null = null;
  // One UUID per process: a file that cannot be written must not get a fresh one on every read.
  private generated: string | null = null;
  private readonly warned = new Set<string>();

  constructor(
    readonly path: string,
    private readonly env: NodeJS.ProcessEnv = process.env,
    private readonly log: Log = logger,
    private readonly newIdentity: () => string = randomUUID,
    private readonly settleMs = 300,
  ) {}

  /** The settings when the file changed since the last call (or on the first call), else null. */
  async poll(): Promise<LoadedConfig | null> {
    const first = this.read();
    if (first === null || first === this.lastText) return null;
    // An editor that truncates and rewrites can be caught in between: only a text that
    // reads the same twice is taken.
    if (this.settleMs > 0) await new Promise((resolve) => setTimeout(resolve, this.settleMs));
    const text = this.read();
    if (text !== first) return null;
    this.lastText = text;
    return this.load(text);
  }

  private identity(): string {
    return (this.generated ??= this.newIdentity());
  }

  private read(): string | null {
    let text: string;
    try {
      text = fs.readFileSync(this.path, 'utf8');
    } catch (err) {
      if ((err as NodeJS.ErrnoException).code !== 'ENOENT') {
        this.warnOnce('read', `Could not read ${this.path}: ${message(err)}`);
        return null;
      }
      // Only before the first load: a file that disappears while running (deleted, or mid
      // replace) is waited for, so the identity in use is never replaced by a new one.
      if (this.lastText === null) return this.create();
      this.warnOnce('missing', `Config file ${this.path} is gone; keeping the settings in use until it is back.`);
      return null;
    }
    this.warned.delete('missing');
    this.warned.delete('read');
    return text.trim() ? text : null;
  }

  private create(): string | null {
    const identity = this.identity();
    const text = configTemplate(identity, defaultServerName(identity));
    try {
      // 'wx': a file someone else created in the meantime is never replaced.
      fs.writeFileSync(this.path, text, { mode: 0o600, flag: 'wx' });
    } catch (err) {
      this.warnOnce('create', `Could not create ${this.path} (${message(err)}); create it as the README shows.`);
      return null;
    }
    this.log.info(`Created ${this.path} with identityToken=${identity}`);
    return text;
  }

  private load(text: string): LoadedConfig {
    const values = parseKeyValues(text);
    let identityNotSaved: string | undefined;
    if (!values.identityToken) {
      values.identityToken = this.identity();
      try {
        const updated = setKeys(text, { identityToken: values.identityToken });
        writeFileAtomic(this.path, updated);
        this.lastText = updated;
        this.log.info(`Generated identityToken=${values.identityToken} and saved it in ${this.path}`);
      } catch (err) {
        // Connecting would register a server whose identity is gone on the next start.
        this.log.warn(`Could not write identityToken into ${this.path}: ${message(err)}`);
        identityNotSaved = `Set identityToken=${values.identityToken} in ${this.path}`;
      }
    }
    const config = buildConfig(values, this.env, path.dirname(path.resolve(this.path)));
    return identityNotSaved ? { config: { ...config, registrationToken: '' }, identityNotSaved } : { config };
  }

  private warnOnce(key: string, text: string): void {
    if (this.warned.has(key)) return;
    this.warned.add(key);
    this.log.warn(text);
  }
}

/** Sets keys in a key=value text, keeping its comments and order; a missing key is appended. */
export function setKeys(text: string, writes: Record<string, string>): string {
  const newline = text.includes('\r\n') ? '\r\n' : '\n';
  const lines = text.split(/\r?\n/);
  const pending = new Map(Object.entries(writes));
  // From the end: parseKeyValues lets the last line for a key win.
  for (let i = lines.length - 1; i >= 0 && pending.size; i--) {
    const trimmed = lines[i].trim();
    const eq = trimmed.indexOf('=');
    if (trimmed.startsWith('#') || eq < 0) continue;
    const key = trimmed.slice(0, eq).trim();
    if (!pending.has(key)) continue;
    lines[i] = `${key}=${pending.get(key)}`;
    pending.delete(key);
  }
  if (pending.size) {
    while (lines.length && lines[lines.length - 1] === '') lines.pop();
    for (const [key, value] of pending) lines.push(`${key}=${value}`);
    lines.push('');
  }
  return lines.join(newline);
}

/**
 * Replaces a file, keeping its owner and mode, so a reader never sees it half-written. A file
 * that cannot be renamed over (a single-file docker bind mount answers EBUSY) is written in place.
 */
export function writeFileAtomic(file: string, text: string): void {
  const stat = fs.statSync(file);
  const tmp = `${file}.tmp-${process.pid}`;
  try {
    fs.writeFileSync(tmp, text, { mode: stat.mode & 0o777 });
    fs.chmodSync(tmp, stat.mode & 0o777);
    try {
      fs.chownSync(tmp, stat.uid, stat.gid);
    } catch {
      // Only root may give a file away; a file the bridge's own user owns needs nothing.
    }
    fs.renameSync(tmp, file);
  } catch (err) {
    fs.rmSync(tmp, { force: true });
    const code = (err as NodeJS.ErrnoException).code;
    if (code !== 'EBUSY' && code !== 'EXDEV' && code !== 'EPERM') throw err;
    fs.writeFileSync(file, text);
  }
}

function message(err: unknown): string {
  return err instanceof Error ? err.message : String(err);
}
