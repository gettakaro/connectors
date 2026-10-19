import { createHash } from 'node:crypto';
import fs from 'node:fs';
import { CONNECTOR_VERSION } from '../version.js';
import type { Capability } from './capabilities.js';
import { GAME_ICON_PNG_BASE64 } from './gameIcon.generated.js';

/** Stable identifiers: Takaro groups connector releases by `connector.name`, so neither is ever renamed. */
export const GAME_SLUG = 'dune';
export const CONNECTOR_NAME = 'takaro-dune';
export const GAME_DISPLAY_NAME = 'Dune: Awakening';

export interface ConnectorIdentity {
  name: string;
  version: string;
}

export const CONNECTOR_IDENTITY: ConnectorIdentity = { name: CONNECTOR_NAME, version: CONNECTOR_VERSION };

export interface GameIcon {
  bytes: Buffer;
  /** First 12 hex characters of the PNG's sha256: a changed icon always gets a new revision. */
  revision: string;
  size: number;
}

export function iconFromBytes(bytes: Buffer): GameIcon {
  return { bytes, revision: createHash('sha256').update(bytes).digest('hex').slice(0, 12), size: bytes.length };
}

/** The shipped 256 x 256 PNG, with its revision and size computed from the bytes, never by hand. */
export const GAME_ICON: GameIcon = iconFromBytes(Buffer.from(GAME_ICON_PNG_BASE64, 'base64'));

/** The `getGameIcon` response: always the bytes of the revision the manifest declares. */
export function gameIconPayload(icon: GameIcon = GAME_ICON): { revision: string; data: string } {
  return { revision: icon.revision, data: icon.bytes.toString('base64') };
}

/** The `game.profile` capability. `version` is the running game build, omitted while it is unknown. */
export function gameProfileCapability(gameVersion: string | null, icon: GameIcon = GAME_ICON): Capability {
  const version = gameVersion ? cleanGameVersion(gameVersion) : null;
  return {
    name: 'game.profile',
    parameters: {
      name: GAME_DISPLAY_NAME,
      ...(version ? { version } : {}),
      icon: { revision: icon.revision, size: icon.size },
    },
  };
}

/** Takaro accepts 1 to 32 printable ASCII characters; anything else is no version at all. */
export function cleanGameVersion(value: string): string | null {
  const trimmed = value.trim();
  return /^[\x20-\x7e]{1,32}$/.test(trimmed) ? trimmed : null;
}

/**
 * The map server logs its build once at boot: `LogNetVersion: Log: DuneSandbox 1.5.3.7, NetCL: 2147284`. The number
 * after `DuneSandbox` is the game build players see; the Postgres database name only carries the content version
 * (`dune_sb_1_5_3_0`), which does not change with a patch.
 */
const NET_VERSION_LINE = /LogNetVersion:.*\bDuneSandbox (\d+(?:\.\d+){1,3}), NetCL: \d+/;

export function parseGameBuild(line: string): string | null {
  return NET_VERSION_LINE.exec(line)?.[1] ?? null;
}

/** How far into the map-server log the boot lines are searched (the version line sits within the first ~30 KB). */
const SCAN_BYTES = 1024 * 1024;

/** The running game build, read from the map-server log and kept current as the log tail sees a map restart. */
export class GameBuild {
  private current: string | null = null;

  version(): string | null {
    return this.current;
  }

  /** Feeds one log line; true when it changed the known build. */
  observe(line: string): boolean {
    const found = parseGameBuild(line);
    if (!found || found === this.current) return false;
    this.current = found;
    return true;
  }

  /** Reads the boot lines of the current log, which the tail (starting at the end of the file) never sees. */
  scanFile(file: string): void {
    let fd: number | null = null;
    try {
      fd = fs.openSync(file, 'r');
      const buffer = Buffer.alloc(SCAN_BYTES);
      const read = fs.readSync(fd, buffer, 0, SCAN_BYTES, 0);
      for (const line of buffer.subarray(0, read).toString('utf8').split(/\r?\n/)) {
        if (this.observe(line)) return;
      }
    } catch {
      // No log yet: the build stays unknown until the tail sees the boot lines.
    } finally {
      if (fd !== null) fs.closeSync(fd);
    }
  }
}
