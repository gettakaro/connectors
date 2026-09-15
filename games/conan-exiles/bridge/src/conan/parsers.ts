export interface ConanPlayer {
  gameId: string;
  name: string;
  characterName?: string;
  rconId?: string;
  steamId?: string;
  platformId?: string;
  online: boolean;
}

export interface ConanBan {
  gameId: string;
  steamId?: string;
  platformId?: string;
  reason?: string;
}

export function parseListPlayers(raw: string): ConanPlayer[] {
  return raw
    .split(/\r?\n/)
    .map((line) => parsePlayerLine(line.trim()))
    .filter((player): player is ConanPlayer => player !== null);
}

export function parseListBans(raw: string): ConanBan[] {
  return raw
    .split(/\r?\n/)
    .map((line) => parseBanLine(line.trim()))
    .filter((ban): ban is ConanBan => ban !== null);
}

function parsePlayerLine(line: string): ConanPlayer | null {
  if (!line || /^(players connected|no players|[-\s]+)$/i.test(line)) return null;

  const tablePlayer = parseTablePlayerLine(line);
  if (tablePlayer) return tablePlayer;

  const kv = parseKeyValues(line);
  if (kv.name && (kv.userid || kv.userId || kv.platformid || kv.platformId)) {
    const userId = kv.userid || kv.userId;
    const platformId = normalizePlatformId(kv.platformid || kv.platformId);
    const steamId = extractSteamId(platformId || line);
    return {
      gameId: steamId || platformId || userId || kv.name,
      name: kv.name,
      ...(steamId ? { steamId } : {}),
      ...(platformId ? { platformId } : {}),
      online: true,
    };
  }

  const pipeMatch = line.match(/^\s*\d+[\).:-]?\s*(.+?)\s*(?:\||-|\()\s*(?:Steam:)?(\d{5,})\)?/i);
  if (pipeMatch) {
    const name = pipeMatch[1]!.trim();
    const steamId = pipeMatch[2]!;
    return {
      gameId: steamId,
      name,
      steamId,
      platformId: `steam:${steamId}`,
      online: true,
    };
  }

  return null;
}

function parseTablePlayerLine(line: string): ConanPlayer | null {
  const parts = line.split('|').map((part) => part.trim());
  if (parts.length < 6 || !/^\d+$/.test(parts[0]!)) return null;

  const charName = parts[1]!;
  const playerName = parts[2]!;
  const userId = parts[3]!;
  const rawPlatformId = parts[4]!;
  const platformName = parts[5]!;
  if (!userId && !rawPlatformId) return null;

  const platformId = normalizePlatformId(
    platformName && rawPlatformId ? `${platformName}:${rawPlatformId}` : rawPlatformId,
  );
  const steamId = extractSteamId(platformId || rawPlatformId);

  return {
    gameId: steamId || platformId || userId || playerName || charName,
    name: playerName || charName || userId || rawPlatformId,
    rconId: parts[0]!,
    ...(charName && charName !== playerName ? { characterName: charName } : {}),
    ...(steamId ? { steamId } : {}),
    ...(platformId ? { platformId } : {}),
    online: true,
  };
}

function parseBanLine(line: string): ConanBan | null {
  if (!line || /^(bans|no bans|[-\s]+)$/i.test(line)) return null;

  const kv = parseKeyValues(line);
  const platformId = normalizePlatformId(kv.platformid || kv.platformId);
  const steamId = extractSteamId(platformId || line);
  const id = kv.userid || kv.userId || steamId || platformId;
  if (!id) return null;

  return {
    gameId: id,
    ...(steamId ? { steamId } : {}),
    ...(platformId ? { platformId } : steamId ? { platformId: `steam:${steamId}` } : {}),
    ...(kv.reason ? { reason: kv.reason } : inferBanReason(line, id)),
  };
}

function parseKeyValues(line: string): Record<string, string> {
  const values: Record<string, string> = {};
  const regex = /([A-Za-z][A-Za-z0-9_]*)(?:=|:)\s*("[^"]+"|\S+)/g;
  let match: RegExpExecArray | null;
  while ((match = regex.exec(line)) !== null) {
    values[match[1]!] = match[2]!.replace(/^"|"$/g, '');
  }
  return values;
}

function normalizePlatformId(value: string | undefined): string | undefined {
  if (!value) return undefined;
  const trimmed = value.trim();
  const match = trimmed.match(/^([A-Za-z]+)[:|](.+)$/);
  if (!match) return trimmed;
  return `${match[1]!.toLowerCase()}:${match[2]}`;
}

function extractSteamId(value: string | undefined): string | undefined {
  return value?.match(/\b(7656\d{13})\b/)?.[1];
}

function inferBanReason(line: string, id: string): { reason?: string } {
  const escaped = id.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
  const stripped = line.replace(new RegExp(`^\\s*\\d+[\\).:-]?\\s*${escaped}\\s*`), '').trim();
  return stripped ? { reason: stripped } : {};
}
