import { ALL_GAME_EVENT_TYPES, type GameEvent, type GameEventType } from '../takaro/protocol.js';

/** TShock writes `<timestamp> - <source>: <LEVEL>: <message>`; the prefix is optional on relayed lines. */
const TSHOCK_PREFIX = /^(?:(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}) - )?[^\s:][^:]*: (?:INFO|WARN|ERROR|DEBUG|FATAL): (.+)$/i;

const CHAT = /^Broadcast: <([^>]+)> (.+)$/;
/** Real TShock join lines carry trailing detail, e.g. `Name has joined. IP: 1.2.3.4`. */
const JOINED = /^(?:\[Server API] )?(.+?) has joined\.(?:\s.*)?$/i;
/** TShock emits both `Name left.` and `Name has left.` depending on the disconnect path. */
const LEFT = /^(?:\[Server API] )?(.+?) (?:has )?left\.(?:\s.*)?$/i;

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

  // `Broadcast: Name (N/A) has joined.` is a chat-channel echo of the authoritative
  // `Name has joined.` line; treating both as joins would emit the event twice.
  if (message.startsWith('Broadcast: ')) return null;

  const joined = message.match(JOINED);
  if (joined) {
    return { type: 'player-connected', data: { player: logPlayer(joined[1]) } };
  }

  const left = message.match(LEFT);
  if (left) {
    return { type: 'player-disconnected', data: { player: logPlayer(left[1]) } };
  }

  return null;
}

function parseTakaroEventMarker(line: string): GameEvent | null {
  const marker = 'TAKARO_EVENT ';
  const markerIndex = line.indexOf(marker);
  if (markerIndex < 0) return null;

  const payload = line.slice(markerIndex + marker.length).trim();
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
  return { gameId: clean, name: clean, platformId: `terraria:${clean}` };
}
