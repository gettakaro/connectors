#!/usr/bin/env node
// Dry-run smoke test for the F9 RPT tail (no Takaro, no WS).
//
//   npm run build
//   DAYZ_LOG_DIR=/srv/.../profiles node scripts/smoke-rpt-tail.mjs
//   node scripts/smoke-rpt-tail.mjs --self-test   # uses a synthetic temp dir
//
// Prints the `log` gameEvent payload that WOULD be sent for every accepted
// line, so the deny-list and rate limit can be eyeballed against a real RPT.
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { RptTail } from '../dist/local/rptTail.js';

const selfTest = process.argv.includes('--self-test');
let dir = process.env.DAYZ_LOG_DIR;

if (selfTest) {
  dir = fs.mkdtempSync(path.join(os.tmpdir(), 'rpttail-'));
  fs.writeFileSync(path.join(dir, 'DayZServer_x64_2026-09-14.RPT'), 'old line that must NOT be replayed\n');
}

if (!dir) {
  console.error('Set DAYZ_LOG_DIR=<dayz profiles dir>, or pass --self-test.');
  process.exit(2);
}

let sent = 0;
const tail = new RptTail({ dir, pollIntervalMs: 250 }, (ev) => {
  sent++;
  console.log('WOULD SEND gameEvent', JSON.stringify({ type: 'log', data: { timestamp: ev.timestamp, msg: ev.msg } }));
});
console.log(`Tailing ${dir} (dry run; Ctrl-C to stop)`);
tail.start();

if (selfTest) {
  const file = path.join(dir, 'DayZServer_x64_2026-09-14.RPT');
  setTimeout(() => {
    fs.appendFileSync(
      file,
      [
        'SCRIPT : [Takaro] Bridge v0.2.0-takaro starting',
        'SCRIPT : [Takaro][DBG] this must be denied',
        'BattlEye Server: RCon admin #0 denied',
        '',
        'Warning Message: denied too',
        'SCRIPT : [Takaro] event: entity-killed ZmbM_CitizenASkinny_Brown',
        '',
      ].join('\n'),
    );
  }, 500);
  setTimeout(() => {
    tail.stop();
    const ok = sent === 2;
    console.log(`\nforwarded=${sent} dropped-by-rate-limit=${tail.droppedCount()}`);
    console.log(ok ? 'SELF-TEST PASS (2 accepted, 4 denied, 1 pre-existing line not replayed)' : 'SELF-TEST FAIL');
    fs.rmSync(dir, { recursive: true, force: true });
    process.exit(ok ? 0 : 1);
  }, 1500);
}
