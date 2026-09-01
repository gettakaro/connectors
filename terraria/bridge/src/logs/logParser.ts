import { ALL_GAME_EVENT_TYPES, type GameEvent, type GameEventType } from '../takaro/protocol.js';

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

  const tshockBroadcast = trimmed.match(/^(?:(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}) - )?[^:]+: INFO: Broadcast: <([^>]+)> (.+)$/);
  if (tshockBroadcast) {
    return {
      type: 'chat-message',
      data: {
        player: logPlayer(tshockBroadcast[2]),
        message: tshockBroadcast[3],
        timestamp: tshockBroadcast[1],
      },
    };
  }

  const joined = trimmed.match(/(?:\[Server API] )?(.+?) has joined\.$/i);
  if (joined) {
    return { type: 'player-connected', data: { player: logPlayer(joined[1]) } };
  }

  const left = trimmed.match(/(?:\[Server API] )?(.+?) has left\.$/i);
  if (left) {
    return { type: 'player-disconnected', data: { player: logPlayer(left[1]) } };
  }

  const chat = trimmed.match(/^([^:]{1,40}):\s+(.+)$/);
  if (chat) {
    return { type: 'chat-message', data: { player: logPlayer(chat[1]), message: chat[2] } };
  }

  return { type: 'log', data: { message: trimmed } };
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
