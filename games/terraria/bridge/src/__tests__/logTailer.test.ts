import assert from 'node:assert/strict';
import { mkdtempSync, mkdirSync, rmSync, appendFileSync, writeFileSync, utimesSync } from 'node:fs';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { test } from 'node:test';
import { LogTailer } from '../logs/logTailer.js';
import type { GameEvent } from '../takaro/protocol.js';

/** TShock names each log after the server start time and opens a new one on every restart. */
function logName(hour: number, minute: number): string {
  return `2026-09-01_${String(hour).padStart(2, '0')}-${String(minute).padStart(2, '0')}-00.log`;
}

/** mtime ordering decides which log is newest, so fixtures must stamp it explicitly. */
function stampMtime(file: string, minutesFromEpoch: number): void {
  const when = new Date(Date.UTC(2026, 8, 1, 7, minutesFromEpoch, 0));
  utimesSync(file, when, when);
}

function chatLine(message: string): string {
  return `2026-09-01 07:31:49 - TShock: INFO: Broadcast: <TestPlayer> ${message}\n`;
}

function messagesOf(events: GameEvent[]): string[] {
  return events
    .filter((event) => event.type === 'chat-message')
    .map((event) => (event.data as { message: string }).message);
}

async function withTempDir(prefix: string, run: (dir: string) => Promise<void>): Promise<void> {
  const dir = mkdtempSync(path.join(tmpdir(), prefix));
  try {
    await run(dir);
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
}

test('emits lines appended to the current file', async () => {
  await withTempDir('terraria-tail-append-', async (dir) => {
    const log = path.join(dir, logName(7, 0));
    writeFileSync(log, chatLine('before'));
    const events: GameEvent[] = [];
    const tailer = new LogTailer(dir, (event) => events.push(event), 1000);

    // First start on an existing file: history is skipped, not replayed into Takaro.
    await tailer.pollOnce();
    assert.deepEqual(messagesOf(events), []);

    appendFileSync(log, chatLine('after'));
    await tailer.pollOnce();

    assert.deepEqual(messagesOf(events), ['after']);
  });
});

test('switches to a newer log file and reads it from the beginning', async () => {
  await withTempDir('terraria-tail-rotate-', async (dir) => {
    const older = path.join(dir, logName(7, 0));
    writeFileSync(older, chatLine('old-history'));
    stampMtime(older, 0);

    const events: GameEvent[] = [];
    const tailer = new LogTailer(dir, (event) => events.push(event), 1000);
    await tailer.pollOnce();
    assert.equal(tailer.currentFile(), older);

    // The server restarts and writes plugin markers into the new log immediately. Those
    // must be delivered, so a rotated-into file is read from byte 0 despite startAtEnd.
    const newer = path.join(dir, logName(7, 31));
    writeFileSync(newer, chatLine('first-line-after-restart'));
    stampMtime(newer, 31);
    await tailer.pollOnce();

    assert.equal(tailer.currentFile(), newer);
    assert.deepEqual(messagesOf(events), ['first-line-after-restart']);
  });
});

test('does not re-emit lines already sent from the previous file', async () => {
  await withTempDir('terraria-tail-nodupe-', async (dir) => {
    const older = path.join(dir, logName(7, 0));
    writeFileSync(older, chatLine('old-history'));
    stampMtime(older, 0);

    const events: GameEvent[] = [];
    const tailer = new LogTailer(dir, (event) => events.push(event), 1000, false);

    // startAtEnd=false, so this line is genuinely emitted from the old file.
    await tailer.pollOnce();
    assert.deepEqual(messagesOf(events), ['old-history']);

    const newer = path.join(dir, logName(7, 31));
    writeFileSync(newer, chatLine('new-line'));
    stampMtime(newer, 31);
    await tailer.pollOnce();
    // Extra polls must not replay either file.
    await tailer.pollOnce();
    await tailer.pollOnce();

    assert.deepEqual(messagesOf(events), ['old-history', 'new-line']);
  });
});

test('detects a truncated same-path file and re-reads it from the start', async () => {
  await withTempDir('terraria-tail-truncate-', async (dir) => {
    const log = path.join(dir, 'server.log');
    writeFileSync(log, chatLine('first') + chatLine('second'));

    const events: GameEvent[] = [];
    const tailer = new LogTailer(log, (event) => events.push(event), 1000, false);
    await tailer.pollOnce();
    assert.deepEqual(messagesOf(events), ['first', 'second']);

    // Some setups reuse the filename: the file is replaced and is now shorter than the
    // offset we already consumed, which is the signal to start over.
    writeFileSync(log, chatLine('after-truncate'));
    await tailer.pollOnce();

    assert.deepEqual(messagesOf(events), ['first', 'second', 'after-truncate']);
  });
});

test('does not throw on a missing directory and recovers once it appears', async () => {
  await withTempDir('terraria-tail-missing-', async (parent) => {
    const dir = path.join(parent, 'logs');
    const events: GameEvent[] = [];
    const tailer = new LogTailer(dir + path.sep, (event) => events.push(event), 1000, false);

    // The log directory does not exist yet; polling must stay quiet rather than crash.
    await tailer.pollOnce();
    await tailer.pollOnce();
    assert.deepEqual(messagesOf(events), []);
    assert.equal(tailer.currentFile(), null);

    mkdirSync(dir);
    writeFileSync(path.join(dir, logName(7, 31)), chatLine('recovered'));
    await tailer.pollOnce();

    assert.deepEqual(messagesOf(events), ['recovered']);
  });
});

test('does not throw when the followed file is deleted mid-run', async () => {
  await withTempDir('terraria-tail-deleted-', async (dir) => {
    const log = path.join(dir, logName(7, 0));
    writeFileSync(log, chatLine('present'));

    const events: GameEvent[] = [];
    const tailer = new LogTailer(dir, (event) => events.push(event), 1000, false);
    await tailer.pollOnce();
    assert.deepEqual(messagesOf(events), ['present']);

    rmSync(log);
    await tailer.pollOnce();

    const replacement = path.join(dir, logName(7, 31));
    writeFileSync(replacement, chatLine('replacement'));
    stampMtime(replacement, 31);
    await tailer.pollOnce();

    assert.deepEqual(messagesOf(events), ['present', 'replacement']);
  });
});

test('follows a single absolute file config unchanged', async () => {
  await withTempDir('terraria-tail-singlefile-', async (dir) => {
    const log = path.join(dir, 'server.log');
    writeFileSync(log, chatLine('history'));
    // A newer sibling exists but must be ignored: an explicit file path is pinned by design.
    const sibling = path.join(dir, logName(9, 0));
    writeFileSync(sibling, chatLine('sibling'));
    stampMtime(sibling, 59);

    const events: GameEvent[] = [];
    const tailer = new LogTailer(log, (event) => events.push(event), 1000);

    await tailer.pollOnce();
    assert.deepEqual(messagesOf(events), []);

    appendFileSync(log, chatLine('appended'));
    await tailer.pollOnce();

    assert.equal(tailer.currentFile(), log);
    assert.deepEqual(messagesOf(events), ['appended']);
  });
});

test('follows a glob pattern and picks the newest match', async () => {
  await withTempDir('terraria-tail-glob-', async (dir) => {
    const ignored = path.join(dir, 'notes.txt');
    writeFileSync(ignored, chatLine('ignored'));
    stampMtime(ignored, 59);
    const log = path.join(dir, logName(7, 0));
    writeFileSync(log, chatLine('matched'));
    stampMtime(log, 0);

    const events: GameEvent[] = [];
    const tailer = new LogTailer(path.join(dir, '*.log'), (event) => events.push(event), 1000, false);

    await tailer.pollOnce();

    assert.equal(tailer.currentFile(), log);
    assert.deepEqual(messagesOf(events), ['matched']);
  });
});

/** The exact shape TShock writes for a REST call the bridge itself made. */
function restManagerLine(command: string): string {
  return `2026-09-01 14:08:28 - RestManager: INFO: takaro-rest executed: ${command}.\n`;
}

test('suppresses excluded log lines so the bridge does not feed its own REST traffic back', async () => {
  await withTempDir('terraria-tail-exclude-', async (dir) => {
    const log = path.join(dir, logName(7, 0));
    writeFileSync(log, chatLine('seed'));
    const events: GameEvent[] = [];
    const tailer = new LogTailer(dir, (event) => events.push(event), 1000, true, ['takaro-rest executed:']);
    await tailer.pollOnce();

    appendFileSync(log, restManagerLine('/takarogive TestPlayer 1050 1'));
    appendFileSync(log, restManagerLine('/takaropos TestPlayer'));
    await tailer.pollOnce();

    assert.deepEqual(events.filter((event) => event.type === 'log'), []);
  });
});

test('keeps unexcluded log lines flowing', async () => {
  await withTempDir('terraria-tail-exclude-keep-', async (dir) => {
    const log = path.join(dir, logName(7, 0));
    writeFileSync(log, chatLine('seed'));
    const events: GameEvent[] = [];
    const tailer = new LogTailer(dir, (event) => events.push(event), 1000, true, ['takaro-rest executed:']);
    await tailer.pollOnce();

    appendFileSync(log, '2026-09-01 14:08:29 - TShock: INFO: Server started\n');
    await tailer.pollOnce();

    assert.equal(events.filter((event) => event.type === 'log').length, 1);
  });
});

test('never suppresses a gameplay event, even when its text matches an exclude pattern', async () => {
  await withTempDir('terraria-tail-exclude-gameplay-', async (dir) => {
    const log = path.join(dir, logName(7, 0));
    writeFileSync(log, chatLine('seed'));
    const events: GameEvent[] = [];
    const tailer = new LogTailer(dir, (event) => events.push(event), 1000, true, ['takaro-rest executed:']);
    await tailer.pollOnce();

    // A player can say anything, including the pattern itself. Chat is data, not noise.
    appendFileSync(log, chatLine('takaro-rest executed: is this filtered?'));
    await tailer.pollOnce();

    assert.deepEqual(messagesOf(events), ['takaro-rest executed: is this filtered?']);
  });
});

test('emits every log line when no exclude patterns are configured', async () => {
  await withTempDir('terraria-tail-exclude-none-', async (dir) => {
    const log = path.join(dir, logName(7, 0));
    writeFileSync(log, chatLine('seed'));
    const events: GameEvent[] = [];
    const tailer = new LogTailer(dir, (event) => events.push(event), 1000);
    await tailer.pollOnce();

    appendFileSync(log, restManagerLine('/takarogive TestPlayer 1050 1'));
    await tailer.pollOnce();

    assert.equal(events.filter((event) => event.type === 'log').length, 1);
  });
});
