import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { afterEach, describe, expect, it } from 'vitest';
import { EnshroudedLogParser, LogTailer, type LogEvent } from '../enshrouded/logTail.js';

const SID = '76561198000005875';
const JOIN = [
  `[I 00:03:52,650] [online] Session accepted with peer (steamid:${SID})`,
  `[I 00:03:52,650] [online] Added peer 0(1) (steamid:${SID})`,
  '[E 00:03:53,170] [online] Begin auth session with peer 0(1)',
  `[I 00:03:54,478] [online] Client '${SID}' authenticated by steam`,
  '[I 00:03:54,548] [session] Remote player added. Player handle: 0(0)',
  "[I 00:04:24,062] [server] Machine '1': Player '0(0)' logged in",
  "[I 00:04:24,063] [server] Player 'Limon' logged in with Permissions:",
  '[I 00:04:24,063] \t - CanKickBan',
];
const LEAVE = [
  "[I 00:07:46,469] [Server] Sending Character Savegame 'Limon' Size:22'899",
  "[I 00:07:46,564] [server] Remove Entity for Player 'Limon'",
  "[I 00:07:46,566] [server] Remove Player 'Limon'",
  '[I 00:07:46,704] [online] Disconnecting peer 0(1)',
  '[I 00:07:46,704] [online] Removed peer 0(1)',
];
const limon = { gameId: SID, name: 'Limon', steamId: SID, platformId: `steam:${SID}` };

const feedAll = (p: EnshroudedLogParser, lines: string[]) => lines.flatMap((l) => p.feed(l));

describe('EnshroudedLogParser (lines from research/log-events.md)', () => {
  it('correlates SteamID + persona name on join, emits one disconnect on leave', () => {
    const p = new EnshroudedLogParser();
    expect(feedAll(p, JOIN)).toEqual([{ type: 'player-connected', data: { player: limon } }]);
    expect(feedAll(p, LEAVE)).toEqual([{ type: 'player-disconnected', data: { player: limon } }]);
  });

  it('correlates two overlapping joins by machine index', () => {
    const p = new EnshroudedLogParser();
    const out = feedAll(p, [
      '[I 1] [online] Added peer 0(1) (steamid:111)',
      '[I 2] [online] Added peer 1(2) (steamid:222)',
      "[I 3] [server] Machine '2': Player '1(0)' logged in",
      "[I 3] [server] Player 'Second' logged in with Permissions:",
      "[I 4] [server] Machine '1': Player '0(0)' logged in",
      "[I 4] [server] Player 'First' logged in with Permissions:",
    ]);
    expect(out.map((e) => [e.data.player.name, e.data.player.gameId])).toEqual([['Second', '222'], ['First', '111']]);
  });

  it('peer removed without Remove Player still disconnects; unnamed peer drop is silent', () => {
    const p = new EnshroudedLogParser();
    feedAll(p, JOIN);
    expect(p.feed('[I 9] [online] Removed peer 0(1)')).toEqual([{ type: 'player-disconnected', data: { player: limon } }]);
    expect(feedAll(p, ['[I 1] [online] Added peer 0(3) (steamid:333)', '[I 2] [online] Removed peer 0(3)'])).toEqual([]);
  });

  it('ignores chat-like / savegame / unrelated lines and CRLF', () => {
    const p = new EnshroudedLogParser();
    expect(feedAll(p, ["[I 1] [Server] Sending Character Savegame 'Limon' Size:1", '[I 2] [session] Congestion x\r'])).toEqual([]);
    expect(p.feed("[I 3] [server] Player 'Ghost' logged in with Permissions:\r")).toEqual([]); // no peer seen: no SteamID
  });
});

describe('LogTailer', () => {
  let dir: string;
  afterEach(() => fs.rmSync(dir, { recursive: true, force: true }));

  it('starts at EOF, handles partial lines and rotation', () => {
    dir = fs.mkdtempSync(path.join(os.tmpdir(), 'ensh-tail-'));
    const file = path.join(dir, 'enshrouded_server.log');
    fs.writeFileSync(file, JOIN.join('\n') + '\n'); // old session must not replay
    const events: LogEvent[] = [];
    const tailer = new LogTailer({ file, onEvent: (e) => events.push(e), intervalMs: 999999 });
    tailer.start();
    expect(events).toEqual([]);

    const joinText = JOIN.join('\n') + '\n';
    fs.appendFileSync(file, joinText.slice(0, 200));
    tailer.poll();
    fs.appendFileSync(file, joinText.slice(200));
    tailer.poll();
    expect(events.map((e) => e.type)).toEqual(['player-connected']);

    // rotation: server restart truncates/replaces the file
    fs.rmSync(file);
    fs.writeFileSync(file, '[I 00:00:00,001] new boot\n');
    tailer.poll();
    fs.appendFileSync(file, JOIN.join('\n') + '\n' + LEAVE.join('\n') + '\n');
    tailer.poll();
    tailer.stop();
    expect(events.map((e) => e.type)).toEqual(['player-connected', 'player-connected', 'player-disconnected']);
  });
});
