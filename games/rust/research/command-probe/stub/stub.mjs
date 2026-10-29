#!/usr/bin/env node
// Research stub for issue #197: stands in for Takaro so the command probe can be exercised
// without a shared catalog/invocation contract. It is NOT a Takaro protocol and nothing in
// the connector talks to it.
//
//   GET  /catalog          -> { revision, commands: [{ name }] }   (catalog.json, re-read per request)
//   POST /invoke           -> records a receipt line, answers per the current mode
//   POST /control          -> { mode?, delayMs?, catalog? } changes behaviour at runtime
//   GET  /receipts         -> every receipt recorded so far
//
// Modes: "ok" (authorize every caller), "deny" (deny everything), "drop" (destroy the
// socket mid-request, a backend that disconnects), "slow" (answer after delayMs).
// The command "probedeny" is always denied, so authorized and denied outcomes can be
// shown in one mode. Usage: node stub.mjs <dataDir> <host> [<host> ...]   (port 28099)

import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';

const [dataDir, ...hosts] = process.argv.slice(2);
if (!dataDir || hosts.length === 0) {
  console.error('usage: stub.mjs <dataDir> <host> [<host> ...]');
  process.exit(2);
}
const PORT = 28099;
const catalogFile = path.join(dataDir, 'catalog.json');
const receiptsFile = path.join(dataDir, 'receipts.jsonl');
const state = { mode: 'ok', delayMs: 8000 };

if (!fs.existsSync(catalogFile)) {
  fs.writeFileSync(catalogFile, JSON.stringify({ revision: 1, commands: [{ name: 'probe' }, { name: 'probedeny' }] }, null, 2));
}

function log(line) {
  console.log(`${new Date().toISOString()} ${line}`);
}

function readBody(req) {
  return new Promise((resolve) => {
    let body = '';
    req.on('data', (c) => (body += c));
    req.on('end', () => resolve(body));
  });
}

function send(res, code, obj) {
  res.writeHead(code, { 'content-type': 'application/json' });
  res.end(JSON.stringify(obj));
}

async function handle(req, res) {
  const url = new URL(req.url, 'http://stub');
  if (req.method === 'GET' && url.pathname === '/catalog') {
    return send(res, 200, JSON.parse(fs.readFileSync(catalogFile, 'utf8')));
  }
  if (req.method === 'GET' && url.pathname === '/receipts') {
    const lines = fs.existsSync(receiptsFile) ? fs.readFileSync(receiptsFile, 'utf8').trim().split('\n').filter(Boolean) : [];
    return send(res, 200, lines.map((l) => JSON.parse(l)));
  }
  if (req.method === 'POST' && url.pathname === '/control') {
    const body = JSON.parse((await readBody(req)) || '{}');
    if (body.mode) state.mode = body.mode;
    if (body.delayMs !== undefined) state.delayMs = body.delayMs;
    if (body.catalog) fs.writeFileSync(catalogFile, JSON.stringify(body.catalog, null, 2));
    log(`control mode=${state.mode} delayMs=${state.delayMs}${body.catalog ? ` catalog rev=${body.catalog.revision}` : ''}`);
    return send(res, 200, { ...state, catalog: JSON.parse(fs.readFileSync(catalogFile, 'utf8')) });
  }
  if (req.method === 'POST' && url.pathname === '/invoke') {
    const raw = await readBody(req);
    const inv = JSON.parse(raw || '{}');
    const denied = state.mode === 'deny' || inv.command === 'probedeny';
    const receipt = { receivedAt: new Date().toISOString(), mode: state.mode, outcome: state.mode === 'drop' ? 'dropped' : denied ? 'denied' : 'authorized', invocation: inv };
    fs.appendFileSync(receiptsFile, JSON.stringify(receipt) + '\n');
    log(`invoke ${inv.invocationId} ${inv.command} caller=${inv.caller?.steamId} -> ${receipt.outcome}`);
    if (state.mode === 'drop') return req.socket.destroy();
    const answer = () =>
      send(res, 200, denied
        ? { status: 'denied', reply: `DENIED ${inv.command}: not allowed for you` }
        // A spoofed caller in the answer must never change who the probe replies to.
        : { status: 'ok', reply: `PROBE-OK ${inv.command} ${(inv.args || []).join(' ')} for ${inv.caller?.steamId}`.trim(), caller: { steamId: '76561190000000000' } });
    if (state.mode === 'slow') return setTimeout(answer, state.delayMs);
    return answer();
  }
  send(res, 404, { error: 'not found' });
}

for (const host of hosts) {
  http.createServer((req, res) => handle(req, res).catch((e) => send(res, 500, { error: String(e) })))
    .listen(PORT, host, () => log(`stub listening on ${host}:${PORT}, data ${dataDir}`));
}
