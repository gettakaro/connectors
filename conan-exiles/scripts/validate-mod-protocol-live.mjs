#!/usr/bin/env node

import { spawn } from 'node:child_process';

const healthUrl = process.env.TAKARO_CONAN_HEALTH_URL || 'http://127.0.0.1:3010/health';
const bridgeUrl = process.env.TAKARO_CONAN_BRIDGE_URL || healthUrl.replace(/\/health$/, '');
const mcpUrl = process.env.TAKARO_MCP_URL || 'http://127.0.0.1:3000/mcp';
const source = process.env.TAKARO_CONAN_PROTOCOL_SOURCE || 'TakaroConanProtocolProbe/1.0';
const pauseHostPoller = process.env.TAKARO_CONAN_PROTOCOL_PAUSE_POLLER !== '0';
const marker = `TAKARO_CONAN_PROTOCOL_${new Date().toISOString().replace(/[-:.]/g, '').slice(0, 15)}Z`;
const failures = [];
const ok = [];
const notes = [];
let pausedPids = [];

function record(condition, pass, fail) {
  if (condition) ok.push(pass);
  else failures.push(fail);
}

async function main() {
  const before = await getJson(healthUrl);
  const gameServerId = before.gameServerId;
  record(before.ok === true, 'connector health ok', 'connector health is not ok');
  record(Boolean(gameServerId), `gameServerId=${gameServerId}`, 'health did not include gameServerId');
  if (!gameServerId) return finish();

  if (pauseHostPoller) {
    pausedPids = await pausePollers();
    if (pausedPids.length > 0) notes.push(`temporarily paused host poller pid(s): ${pausedPids.join(', ')}`);
    else notes.push('no host poller process found to pause');
  } else {
    notes.push('host poller pause disabled; probe may race with the active Pippi helper');
  }

  const session = await initializeMcpSession();
  ok.push('MCP session initialized');

  const mcpPromise = callMcpTool(session, 'gameserverSendMessage', {
    id: gameServerId,
    message: marker,
    opts: { senderNameOverride: 'Takaro' },
  });

  const polled = await waitForCommand();
  record(polled.hasCommand === true, 'mod poll returned a queued command', 'mod poll did not return a queued command');
  record(polled.command?.action === 'sendMessage', 'queued command action is sendMessage', `queued command action was ${polled.command?.action ?? '<none>'}`);
  record(polled.command?.args?.message === marker, 'queued command marker matches MCP message', 'queued command marker did not match MCP message');

  if (!polled.command?.requestId) {
    failures.push('queued command did not include requestId');
    return finish();
  }

  const resultBody = {
    requestId: polled.command.requestId,
    result: {
      success: true,
      sent: true,
      renderer: 'protocol-probe',
      marker,
    },
  };
  const resultResponse = await postJson(`${bridgeUrl}/mod/result`, resultBody);
  record(resultResponse.success === true, 'mod result accepted for queued command', 'mod result was not accepted');

  const mcpResult = await mcpPromise;
  record(
    mcpResult && typeof mcpResult === 'object' && !mcpResult.error,
    'MCP sendMessage resolved without MCP error after protocol probe result',
    `MCP sendMessage returned an error-like result: ${JSON.stringify(mcpResult).slice(0, 300)}`,
  );

  const eventMarker = `${marker}_INBOUND`;
  const eventResponse = await postJson(`${bridgeUrl}/mod/event`, {
    type: 'chat-message',
    data: {
      msg: eventMarker,
      message: eventMarker,
      channel: 'global',
      timestamp: new Date().toISOString(),
      player: {
        gameId: '76561198000735875',
        platformId: 'steam:76561198000735875',
        name: 'TakaroConan Protocol Probe',
      },
    },
  });
  record(eventResponse.success === true, 'mod event accepted for chat-message', 'mod event was not accepted');

  const after = await getJson(healthUrl);
  record(after.modBridge?.lastPollSource === source, `lastPollSource=${source}`, `lastPollSource=${after.modBridge?.lastPollSource ?? '<none>'}`);
  record(after.modBridge?.lastResultSource === source, `lastResultSource=${source}`, `lastResultSource=${after.modBridge?.lastResultSource ?? '<none>'}`);
  record(after.modBridge?.lastEventSource === source, `lastEventSource=${source}`, `lastEventSource=${after.modBridge?.lastEventSource ?? '<none>'}`);
  record(after.modBridge?.lastEventType === 'chat-message', 'lastEventType=chat-message', `lastEventType=${after.modBridge?.lastEventType ?? '<none>'}`);
  record(
    Array.isArray(after.modBridge?.recentResults) && after.modBridge.recentResults.some((item) =>
      item.action === 'sendMessage'
      && item.message === marker
      && item.source === source
      && item.resultSuccess === true
    ),
    'recentResults contains protocol marker handled by probe source',
    'recentResults did not contain protocol marker handled by probe source',
  );
  record(
    Array.isArray(after.modBridge?.recentEvents) && after.modBridge.recentEvents.some((item) =>
      item.type === 'chat-message'
      && item.message === eventMarker
      && item.source === source
      && (item.playerGameId === '76561198000735875' || item.playerPlatformId === 'steam:76561198000735875')
    ),
    'recentEvents contains protocol inbound marker with stable identity',
    'recentEvents did not contain protocol inbound marker with stable identity',
  );
  record(after.modBridge?.pendingCommands === 0 && after.modBridge?.pendingResults === 0, 'mod bridge queues drained', 'mod bridge queues did not drain');

  notes.push('protocol probe source intentionally does not satisfy the final TakaroConan install/live source regex');
  finish();
}

async function waitForCommand() {
  const deadline = Date.now() + Number.parseInt(process.env.TAKARO_CONAN_PROTOCOL_TIMEOUT_MS || '15000', 10);
  let last = null;
  while (Date.now() < deadline) {
    const response = await fetch(`${bridgeUrl}/mod/poll?source=${encodeURIComponent(source)}`, {
      headers: { 'user-agent': source },
    });
    if (!response.ok) throw new Error(`/mod/poll HTTP ${response.status}`);
    const body = await response.json();
    last = body;
    if (body.hasCommand) return body;
    await sleep(250);
  }
  return last ?? { hasCommand: false };
}

async function postJson(url, body) {
  const response = await fetch(url, {
    method: 'POST',
    headers: {
      'content-type': 'application/json',
      'x-takaro-mod-source': source,
      'user-agent': source,
    },
    body: JSON.stringify(body),
  });
  const text = await response.text();
  let parsed = {};
  try {
    parsed = text ? JSON.parse(text) : {};
  } catch {
    parsed = { text };
  }
  if (!response.ok) throw new Error(`${url} HTTP ${response.status}: ${text}`);
  return parsed;
}

async function getJson(url) {
  const response = await fetch(url);
  if (!response.ok) throw new Error(`${url} HTTP ${response.status}`);
  return response.json();
}

async function initializeMcpSession() {
  const response = await fetch(mcpUrl, {
    method: 'POST',
    headers: { 'content-type': 'application/json' },
    body: JSON.stringify({
      jsonrpc: '2.0',
      method: 'initialize',
      params: { protocolVersion: '2024-11-05', capabilities: {} },
      id: 1,
    }),
  });
  if (!response.ok) throw new Error(`MCP initialize HTTP ${response.status}`);
  const session = response.headers.get('mcp-session-id');
  if (!session) throw new Error('MCP initialize did not return Mcp-Session-Id');
  return session;
}

async function callMcpTool(session, name, args) {
  const response = await fetch(mcpUrl, {
    method: 'POST',
    headers: {
      'content-type': 'application/json',
      'mcp-session-id': session,
    },
    body: JSON.stringify({
      jsonrpc: '2.0',
      method: 'tools/call',
      params: { name, arguments: args },
      id: Date.now(),
    }),
  });
  if (!response.ok) throw new Error(`${name} HTTP ${response.status}`);
  const body = await response.json();
  if (body.error) throw new Error(JSON.stringify(body.error));
  const text = body.result?.content?.find((item) => item.type === 'text')?.text;
  if (typeof text !== 'string') return body.result ?? {};
  return JSON.parse(text);
}

async function pausePollers() {
  const rows = await runCommand('pgrep', ['-af', 'pollerCli\\.(js|ts)']).catch(() => '');
  const pids = rows
    .split(/\r?\n/)
    .map((line) => line.trim())
    .filter((line) => line && /conan-exiles|TAKARO_CONAN_BRIDGE_URL|\/mod\/poll/.test(line))
    .map((line) => Number.parseInt(line.split(/\s+/, 1)[0], 10))
    .filter((pid) => Number.isInteger(pid) && pid > 0 && pid !== process.pid);
  for (const pid of pids) process.kill(pid, 'SIGSTOP');
  return pids;
}

async function resumePollers() {
  for (const pid of pausedPids) {
    try {
      process.kill(pid, 'SIGCONT');
    } catch {
      // Process may have exited while paused.
    }
  }
  pausedPids = [];
}

function runCommand(command, args) {
  return new Promise((resolve, reject) => {
    const child = spawn(command, args, { stdio: ['ignore', 'pipe', 'pipe'] });
    let stdout = '';
    let stderr = '';
    child.stdout.on('data', (chunk) => { stdout += chunk; });
    child.stderr.on('data', (chunk) => { stderr += chunk; });
    child.on('error', reject);
    child.on('close', (code) => {
      if (code === 0) resolve(stdout);
      else reject(new Error(stderr || `${command} exited ${code}`));
    });
  });
}

function sleep(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

async function finish() {
  await resumePollers();
  for (const line of ok) console.log(`OK: ${line}`);
  for (const line of notes) console.log(`NOTE: ${line}`);
  if (failures.length > 0) {
    for (const line of failures) console.error(`FAIL: ${line}`);
    console.error(`\nTakaro Conan mod protocol probe failed (${failures.length} failure(s)).`);
    process.exit(1);
  }
  console.log('\nTakaro Conan mod protocol probe passed.');
}

process.on('SIGINT', () => {
  void resumePollers().finally(() => process.exit(130));
});
process.on('SIGTERM', () => {
  void resumePollers().finally(() => process.exit(143));
});

main().catch(async (err) => {
  await resumePollers();
  console.error(`FAIL: ${err instanceof Error ? err.message : String(err)}`);
  process.exit(1);
});
