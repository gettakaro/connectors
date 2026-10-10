import { randomUUID } from 'node:crypto';
import { type BridgeConfig, buildConfig, parseKeyValues } from './config.js';
import {
  type ConfigLocation,
  type IdentitySource,
  mergeSaved,
  readIfExists,
  renderSaved,
  resolveFiles,
  setKeys,
  writeFileAtomic,
} from './configFiles.js';
import { logger } from './logger.js';

export interface LoadedConfig {
  config: BridgeConfig;
  identitySource: IdentitySource;
  /** Set when the bridge must not connect: a generated identity could not be stored anywhere. */
  identityNotSaved?: string;
}

type Log = Pick<typeof logger, 'info' | 'warn' | 'error'>;

/**
 * Reads the config files, keeps the user file and the saved copy up to date, and tells the
 * caller when the text of the user file changed. It never throws for a missing, empty or
 * half-written file: those keep the settings already in use.
 */
export class ConfigSource {
  private lastUserText: string | null = null;
  private previousLoadedText: string | null = null;
  // One UUID per process: a user file that cannot be written must not get a fresh identity
  // on every read.
  private generatedIdentity: string | null = null;
  private readonly warned = new Set<string>();

  constructor(
    readonly location: ConfigLocation,
    private readonly env: NodeJS.ProcessEnv = process.env,
    private readonly log: Log = logger,
    private readonly newIdentity: () => string = randomUUID,
    private readonly settleMs = 300,
  ) {}

  /**
   * The settings when the user file changed since the last call (or on the first call),
   * else null. Also null while the file is missing, empty or still being written.
   */
  async poll(): Promise<LoadedConfig | null> {
    const first = this.readUser();
    if (first === null || first === this.lastUserText) return null;
    // An editor that truncates and rewrites can be caught in between: only a text that
    // reads the same twice is taken.
    if (this.settleMs > 0) await new Promise((resolve) => setTimeout(resolve, this.settleMs));
    const text = this.readUser();
    if (text !== first) return null;
    this.lastUserText = text;
    return this.load(text);
  }

  private readUser(): string | null {
    let text: string | null;
    try {
      text = readIfExists(this.location.userPath);
    } catch (err) {
      this.warnOnce('read', `Could not read ${this.location.userPath}: ${message(err)}`);
      return null;
    }
    if (text === null) {
      this.warnOnce('missing', `Config file not found at ${this.location.userPath}; waiting for it.`);
      return null;
    }
    this.warned.delete('missing');
    if (!text.trim()) return null;
    return text;
  }

  private load(userText: string): LoadedConfig {
    let savedText: string | null = null;
    try {
      savedText = readIfExists(this.location.savedPath);
    } catch (err) {
      this.warnOnce('read-saved', `Could not read ${this.location.savedPath}: ${message(err)}`);
    }
    const resolution = resolveFiles(
      userText,
      savedText,
      () => (this.generatedIdentity ??= this.newIdentity()),
      this.previousLoadedText,
    );
    const { values, identitySource, userWrites, restored } = resolution;

    if (restored.length) {
      this.log.info(`Restored ${restored.join(', ')} from ${this.location.savedPath} into ${this.location.userPath}`);
    }
    if (identitySource === 'generated') {
      this.log.info(`Generated a new identityToken for this server: ${values.identityToken}`);
    }
    let identityStored = identitySource !== 'generated';
    if (Object.keys(userWrites).length) {
      try {
        const updated = setKeys(userText, userWrites);
        writeFileAtomic(this.location.userPath, updated);
        this.lastUserText = updated;
        identityStored = true;
        this.warned.delete('write-user');
      } catch (err) {
        this.warnOnce('write-user', `Could not write ${Object.keys(userWrites).join(', ')} into ${this.location.userPath}: ${message(err)}`);
      }
    }
    if (this.location.savedPath) {
      const desired = renderSaved(mergeSaved(savedText == null ? null : parseKeyValues(savedText), values));
      if (desired === savedText) identityStored = true;
      else {
        try {
          writeFileAtomic(this.location.savedPath, desired, 0o600);
          identityStored = true;
          this.warned.delete('write-saved');
        } catch (err) {
          this.warnOnce(
            'write-saved',
            `Could not save a copy of the settings to ${this.location.savedPath} (${message(err)}); `
              + 'back up TakaroConfig.txt before replacing the bridge folder.',
          );
        }
      }
    }
    this.previousLoadedText = userText;
    const config = buildConfig(values, this.env);
    if (!identityStored) {
      // Connecting would register a server whose identity is gone on the next start.
      return {
        config: { ...config, registrationToken: '' },
        identitySource,
        identityNotSaved: `Set identityToken=${values.identityToken} in ${this.location.userPath}`,
      };
    }
    return { config, identitySource };
  }

  private warnOnce(key: string, text: string): void {
    if (this.warned.has(key)) return;
    this.warned.add(key);
    this.log.warn(text);
  }
}

function message(err: unknown): string {
  return err instanceof Error ? err.message : String(err);
}
