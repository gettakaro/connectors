import type { ConanPlayer } from '../conan/parsers.js';

export async function validateStrictModEvent(
  type: string,
  data: unknown,
  getKnownPlayers: () => Promise<ConanPlayer[]>,
): Promise<void> {
  if (type !== 'chat-message') return;
  const record = data && typeof data === 'object' && !Array.isArray(data) ? data as Record<string, unknown> : null;
  const player = record?.player && typeof record.player === 'object' && !Array.isArray(record.player)
    ? record.player as Record<string, unknown>
    : null;
  const identifiers = [
    stringValue(player?.gameId),
    stringValue(player?.steamId),
    stringValue(player?.platformId),
  ].filter((value): value is string => Boolean(value));

  if (identifiers.length === 0) {
    throw new Error('Strict mod event validation requires player gameId, steamId, or platformId');
  }

  const knownPlayers = await getKnownPlayers();
  if (knownPlayers.length === 0) {
    throw new Error('Strict mod event validation has no known Conan players');
  }

  const matched = knownPlayers.some((known) => identifiers.some((identifier) => playerMatchesIdentifier(known, identifier)));
  if (!matched) {
    throw new Error('Strict mod event validation rejected unknown player identity');
  }
}

function playerMatchesIdentifier(player: ConanPlayer, identifier: string): boolean {
  const normalized = identifier.toLowerCase();
  return [
    player.gameId,
    player.steamId,
    player.platformId,
    player.name,
  ]
    .filter((value): value is string => Boolean(value))
    .some((value) => value.toLowerCase() === normalized);
}

function stringValue(value: unknown): string | null {
  return typeof value === 'string' && value ? value : null;
}
