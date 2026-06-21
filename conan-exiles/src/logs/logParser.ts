import type { EmittedGameEvent } from '../events/playerPoller.js';

export interface LogParserOptions {
  now?: () => Date;
}

export function parseLogLine(line: string, options: LogParserOptions = {}): EmittedGameEvent[] {
  const trimmed = line.trim();
  if (!trimmed) return [];

  const timestamp = parseConanTimestamp(trimmed) ?? (options.now?.() ?? new Date()).toISOString();
  const events: EmittedGameEvent[] = [];
  const killEvent = parseKillEvent(trimmed, timestamp);
  if (killEvent) events.push(killEvent);

  const chat = parseChat(trimmed, timestamp);
  if (chat) events.push(chat);

  events.push({
    type: 'log',
    data: {
      msg: trimmed,
      timestamp,
    },
  });

  return events;
}

function parseKillEvent(line: string, timestamp: string): EmittedGameEvent | null {
  const match = line.match(
    /KillCharacterWithRagdoll_Implementation\.\s+KillerNameInput:\s*(.*?)\s+CauseOfDeath:\s*(.*?)\.\s+IsThrall:\s*(\d+)\s+Name:\s*(\S+)\s+CharacterName:\s*(.+)$/i,
  );
  if (!match) return null;

  const killerName = match[1]!.trim();
  const causeOfDeath = match[2]!.trim();
  const isThrall = match[3] === '1';
  const actorName = match[4]!.trim();
  const characterName = match[5]!.trim();

  if (actorName.startsWith('BasePlayerChar_')) {
    return {
      type: 'player-death',
      data: {
        player: {
          gameId: characterName,
          name: characterName,
        },
        msg: causeOfDeath,
        timestamp,
        ...(killerName && isPlayerKillerName(killerName) && normalizeIdentifier(killerName) !== normalizeIdentifier(characterName)
          ? { attacker: { gameId: killerName, name: killerName } }
          : {}),
      },
    };
  }

  if (!killerName || !isPlayerKillerName(killerName)) return null;

  return {
    type: 'entity-killed',
    data: {
      player: {
        gameId: killerName,
        name: killerName,
      },
      entity: characterName || actorType(actorName),
      weapon: causeOfDeath === 'None' ? 'unknown' : causeOfDeath,
      timestamp,
      msg: `${killerName} killed ${characterName || actorType(actorName)}${isThrall ? ' (thrall)' : ''}`,
    },
  };
}

function parseChat(line: string, timestamp: string): EmittedGameEvent | null {
  const liveChat = line.match(
    /ChatWindow:\s+Character\s+(.+?)\s+\(uid\s+\d+,\s+player\s+(7656\d{13})\)\s+said:\s+(.+)$/i,
  );
  if (liveChat) {
    const name = liveChat[1]!.trim();
    const steamId = liveChat[2]!.trim();
    return {
      type: 'chat-message',
      data: {
        msg: liveChat[3]!.trim(),
        channel: 'global',
        timestamp,
        player: {
          gameId: steamId,
          name,
          steamId,
          platformId: `steam:${steamId}`,
        },
      },
    };
  }

  const pippiChat = line.match(/ChatWindow:\s+Character\s+(.+?)\s+said:\s+(.+)$/i);
  if (pippiChat) {
    const name = pippiChat[1]!.trim();
    return {
      type: 'chat-message',
      data: {
        msg: pippiChat[2]!.trim(),
        channel: 'global',
        timestamp,
        player: { gameId: name, name },
      },
    };
  }

  if (/\[Pippi\]PippiChat:/i.test(line)) return null;

  const patterns = [
    /^\[Chat\]\s+(.+?):\s+(.+)$/,
    /ChatWindow:\s+(.+?):\s+(.+)$/i,
    /Chat:\s+(.+?):\s+(.+)$/i,
  ];

  for (const pattern of patterns) {
    const match = line.match(pattern);
    if (!match) continue;
    return {
      type: 'chat-message',
      data: {
        msg: match[2]!.trim(),
        channel: 'global',
        timestamp,
        player: { name: match[1]!.trim() },
      },
    };
  }

  return null;
}

function actorType(actorName: string): string {
  return actorName.replace(/_\d+$/, '');
}

function isPlayerKillerName(value: string): boolean {
  const normalized = value.trim().toLowerCase();
  if (!normalized || normalized === 'yourself') return false;
  return !/^(npc_|npcprefix_|npc-prefix|bp_npc|humanoidnpc|wildlife_|baseplayerchar_)/i.test(value);
}

function normalizeIdentifier(value: string): string {
  return value.trim().toLowerCase();
}

function parseConanTimestamp(line: string): string | null {
  const match = line.match(/^\[(\d{4})\.(\d{2})\.(\d{2})-(\d{2})\.(\d{2})\.(\d{2})(?::(\d{3}))?\]/);
  if (!match) return null;
  const [, year, month, day, hour, minute, second, millis = '000'] = match;
  return `${year}-${month}-${day}T${hour}:${minute}:${second}.${millis}Z`;
}
