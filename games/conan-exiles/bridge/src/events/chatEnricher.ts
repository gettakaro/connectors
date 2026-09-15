import type { ConanPlayer } from '../conan/parsers.js';
import type { EmittedGameEvent } from './playerPoller.js';

type PlayerLoader = () => Promise<ConanPlayer[]>;

interface ChatMessageData {
  msg?: unknown;
  channel?: unknown;
  timestamp?: unknown;
  player?: {
    gameId?: unknown;
    name?: unknown;
  };
}

export async function enrichChatMessageEvent(event: EmittedGameEvent, loadPlayers: PlayerLoader): Promise<EmittedGameEvent> {
  if (event.type !== 'chat-message') return event;
  return enrichPlayerField(event, loadPlayers);
}

export async function enrichLogEvent(event: EmittedGameEvent, loadPlayers: PlayerLoader): Promise<EmittedGameEvent> {
  if (event.type === 'chat-message' || event.type === 'player-death' || event.type === 'entity-killed') {
    return enrichPlayerField(event, loadPlayers);
  }
  return event;
}

async function enrichPlayerField(event: EmittedGameEvent, loadPlayers: PlayerLoader): Promise<EmittedGameEvent> {
  const data = event.data as ChatMessageData;
  const identifier = firstString(data.player?.gameId, data.player?.name);
  if (!identifier) return event;

  const player = findPlayer(await loadPlayers(), identifier);
  if (!player) return event;

  return {
    type: event.type,
    data: {
      ...data,
      player: eventPlayer(player),
    },
  };
}

function findPlayer(players: ConanPlayer[], identifier: string): ConanPlayer | null {
  const normalized = normalize(identifier);
  return (
    players.find((player) =>
      [player.gameId, player.name, player.characterName, player.steamId, player.platformId]
        .filter((value): value is string => Boolean(value))
        .some((value) => normalize(value) === normalized || normalize(stripPlatformPrefix(value)) === normalized),
    ) ?? null
  );
}

function eventPlayer(player: ConanPlayer): Omit<ConanPlayer, 'online' | 'characterName' | 'rconId'> {
  const { online: _online, characterName: _characterName, rconId: _rconId, ...rest } = player;
  return rest;
}

function firstString(...values: unknown[]): string | null {
  for (const value of values) {
    if (typeof value === 'string' && value.trim()) return value.trim();
  }
  return null;
}

function normalize(value: string): string {
  return stripPlatformPrefix(value).toLowerCase();
}

function stripPlatformPrefix(value: string): string {
  const parts = value.split(':');
  return parts.length > 1 ? parts.slice(1).join(':') : value;
}
