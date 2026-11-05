import { ALL_GAME_EVENT_TYPES, type GameEvent, type GameEventType } from '../takaro/protocol.js';
import { terrariaIdentity } from '../terraria/identity.js';
import { TAKARO_CHAT_MARKER } from '../terraria/serverChat.js';

/** TShock writes `<timestamp> - <source>: <LEVEL>: <message>`; the prefix is optional on relayed lines. */
const TSHOCK_PREFIX = /^(?:(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}) - )?[^\s:][^:]*: (?:INFO|WARN|ERROR|DEBUG|FATAL): (.+)$/i;

const CHAT = /^Broadcast: <([^>]+)> (.+)$/;
/** Real TShock join lines carry trailing detail, e.g. `Name has joined. IP: 1.2.3.4`. */
const JOINED = /^(?:\[Server API] )?(.+?) has joined\.(?:\s.*)?$/i;
/** TShock emits both `Name left.` and `Name has left.` depending on the disconnect path. */
const LEFT = /^(?:\[Server API] )?(.+?) (?:has )?left\.(?:\s.*)?$/i;
/**
 * TShock 6.2 logs a leave only as `Broadcast: Name has left.` (skipped below, like the join echo)
 * plus `Name disconnected.`, so the second line is the one that reports a leave at once.
 */
const DISCONNECTED = /^(.+?) disconnected\.$/;
/**
 * Lines that repeat chat Takaro sent: TShock's `<user> executed: /takarosay …` record of the
 * bridge's own command and the plugin's `TAKARO_CHAT …` line. Their text is Takaro's message,
 * so one that reads `Bob has joined.` must not become a join.
 */
const TAKARO_CHAT_ECHO = new RegExp(`^(?:${TAKARO_CHAT_MARKER} |\\S.* executed: [/.])`);

interface PrefixedLine {
  message: string;
  timestamp?: string;
}

export function parseLogLine(line: string): GameEvent | null {
  const trimmed = line.trim();
  if (!trimmed) return null;

  const takaroEvent = parseTakaroEventMarker(trimmed);
  if (takaroEvent) return takaroEvent;

  const structured = trimmed.match(/^(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}) \[([^\]]+)] (.+)$/);
  if (structured) {
    return {
      type: 'log',
      data: { message: structured[3], level: structured[2], timestamp: structured[1] },
    };
  }

  const prefixed = stripTshockPrefix(trimmed);
  const event = parsePlayerEvent(prefixed);
  if (event) return event;

  return { type: 'log', data: { message: trimmed } };
}

/**
 * Removes the `<timestamp> - <source>: INFO: ` wrapper so join/leave/chat matchers all see the bare
 * message. Lines without the wrapper (plain `Guide has left.`) pass through untouched.
 */
function stripTshockPrefix(line: string): PrefixedLine {
  const match = line.match(TSHOCK_PREFIX);
  if (!match) return { message: line };
  return { message: match[2].trim(), timestamp: match[1] };
}

function parsePlayerEvent({ message, timestamp }: PrefixedLine): GameEvent | null {
  const chat = message.match(CHAT);
  if (chat) {
    return {
      type: 'chat-message',
      data: { player: logPlayer(chat[1]), message: chat[2], timestamp },
    };
  }

  if (TAKARO_CHAT_ECHO.test(message)) return null;

  // `Broadcast: Name (N/A) has joined.` is a chat-channel echo of the authoritative
  // `Name has joined.` line; treating both as joins would emit the event twice.
  if (message.startsWith('Broadcast: ')) return null;

  const joined = message.match(JOINED);
  if (joined) {
    return { type: 'player-connected', data: { player: logPlayer(joined[1]) } };
  }

  const left = message.match(LEFT) ?? message.match(DISCONNECTED);
  if (left) {
    return { type: 'player-disconnected', data: { player: logPlayer(left[1]) } };
  }

  return null;
}

/**
 * The plugin's `TAKARO_EVENT {json}` line. The marker must open the logged message: chat text
 * (a player's, or Takaro's own as TShock logs the command that sent it) is logged too, and
 * a marker inside it must not become an event.
 */
function parseTakaroEventMarker(line: string): GameEvent | null {
  const marker = 'TAKARO_EVENT ';
  const message = stripTshockPrefix(line).message.replace(/^\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2} \[[^\]]+] /, '');
  if (!message.startsWith(marker)) return null;

  const payload = message.slice(marker.length).trim();
  try {
    const parsed = JSON.parse(payload) as { type?: unknown; data?: unknown };
    if (!isGameEventType(parsed.type)) return null;
    return { type: parsed.type, data: parsed.data ?? {} };
  } catch {
    return null;
  }
}

function isGameEventType(value: unknown): value is GameEventType {
  return typeof value === 'string' && (ALL_GAME_EVENT_TYPES as readonly string[]).includes(value);
}

function logPlayer(name: string): { gameId: string; name: string; platformId: string } {
  const clean = name.trim();
  return { ...terrariaIdentity(clean), name: clean };
}
