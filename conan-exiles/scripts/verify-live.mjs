#!/usr/bin/env node

import fs from 'node:fs';
import path from 'node:path';
import { loadConfig } from '../dist/config.js';
import { loadConanItemCatalog } from '../dist/conan/itemCatalog.js';
import { ConanSaveDbReader } from '../dist/conan/saveDb.js';
import { sendRconCommand } from '../dist/rcon/client.js';
import { parseListPlayers } from '../dist/conan/parsers.js';

const startedAt = new Date();
const configPath = findConfigPath();
const config = configPath ? loadConfig(configPath) : null;
const healthPort = Number.parseInt(process.env.TAKARO_CONAN_HEALTH_PORT || '', 10) || config?.httpPort || 3010;
const healthUrl = `http://127.0.0.1:${healthPort}/health`;
const mcpUrl = process.env.TAKARO_MCP_URL || 'http://127.0.0.1:3000/mcp';
const mcpActionGapMs = Number.parseInt(process.env.TAKARO_CONAN_MCP_ACTION_GAP_MS || '', 10) || 6000;
const rconProbeGapMs = Number.parseInt(process.env.TAKARO_CONAN_RCON_PROBE_GAP_MS || '', 10) || 10000;
const rconRetryGapMs = Number.parseInt(process.env.TAKARO_CONAN_RCON_RETRY_GAP_MS || '', 10) || rconProbeGapMs;
const runRconProbes = process.env.TAKARO_CONAN_RUN_RCON_PROBES === '1';
const skipRconProbes = process.env.TAKARO_CONAN_SKIP_RCON_PROBES === '1' || !runRconProbes;

const rcon = (command) =>
  sendRconCommand({
    host: config.rcon.host,
    port: config.rcon.port,
    password: config.rcon.password,
    command,
    timeoutMs: config.rcon.timeoutMs,
  });

const checks = [];

function record(name, ok, detail) {
  checks.push({ name, ok, detail });
  const status = ok ? 'ok' : 'fail';
  console.log(`${status} ${name}${detail ? ` - ${detail}` : ''}`);
}

async function checkHealth() {
  try {
    const response = await fetch(healthUrl);
    const body = await response.json();
    const detail = body.takaroIdentifyError?.message
      ? `gameServerId=${body.gameServerId ?? '<none>'}; identifyError=${body.takaroIdentifyError.message}`
      : `gameServerId=${body.gameServerId ?? '<none>'}`;
    record('bridge health', response.ok && body.ok === true, detail);
    return body;
  } catch (err) {
    record('bridge health', false, errorMessage(err));
    return null;
  }
}

async function checkRconHelp(command, expectedAvailable) {
  try {
    const output = await rconWithRetry(`help ${command}`);
    const available = helpOutputShowsCommand(output, command);
    record(`rcon help ${command}`, available === expectedAvailable, `${available ? 'available' : 'unavailable'}; ${compact(output)}`);
  } catch (err) {
    record(`rcon help ${command}`, false, errorMessage(err));
  }
}

async function checkRconCommand(command) {
  try {
    const output = await rconWithRetry(command);
    record(`rcon ${command}`, true, compact(output));
  } catch (err) {
    record(`rcon ${command}`, false, errorMessage(err));
  }
}

async function checkConanPlayerReadActions() {
  if (!config?.rcon) {
    record('conan RCON getPlayers action evidence', true, 'skipped; RCON config is not available');
    record('conan RCON getPlayer action evidence', true, 'skipped; RCON config is not available');
    return;
  }

  try {
    const players = parseListPlayers(await rconWithRetry('listplayers'));
    record(
      'conan RCON getPlayers action evidence',
      Array.isArray(players),
      `players=${players.length}`,
    );

    const first = players[0] ?? null;
    record(
      'conan RCON getPlayer action evidence',
      Boolean(first?.gameId || first?.steamId || first?.platformId || first?.name) || players.length === 0,
      first
        ? `candidate=${first.gameId ?? first.steamId ?? first.platformId ?? first.name}`
        : 'no online player; getPlayer null path covered by source tests',
    );
  } catch (err) {
    record('conan RCON getPlayers action evidence', false, errorMessage(err));
    record('conan RCON getPlayer action evidence', false, errorMessage(err));
  }
}

function checkConanSaveDb() {
  if (!config?.databasePath) {
    record('conan save DB reads', true, 'skipped; databasePath is not configured');
    record('conan save DB listItems action evidence', true, 'skipped; databasePath is not configured');
    record('conan save DB listEntities action evidence', true, 'skipped; databasePath is not configured');
    record('conan save DB listLocations action evidence', true, 'skipped; databasePath is not configured');
    record('conan save DB getPlayerLocation action evidence', true, 'skipped; databasePath is not configured');
    record('conan save DB getPlayerInventory action evidence', true, 'skipped; databasePath is not configured');
    return;
  }

  try {
    const reader = new ConanSaveDbReader(config.databasePath, loadConanItemCatalog(config.itemCatalogPath));
    const items = reader.listItems();
    const entities = reader.listEntities();
    const locations = reader.listPlayerLocations();
    const firstLocation = locations.find((entry) => entry?.code || entry?.name) ?? null;
    const locationCandidate = firstLocation?.code ?? firstLocation?.name ?? null;
    const location = locationCandidate ? reader.getPlayerLocation(locationCandidate) : null;
    const inventoryCandidate =
      locationCandidate ??
      locations.map((entry) => entry?.code ?? entry?.name).find(Boolean) ??
      null;
    const inventory = inventoryCandidate ? reader.getPlayerInventory(inventoryCandidate) : [];

    record(
      'conan save DB reads',
      items.length > 0 && entities.length > 0 && locations.length > 0,
      `items=${items.length}, entities=${entities.length}, locations=${locations.length}`,
    );
    record(
      'conan save DB listItems action evidence',
      Array.isArray(items) && items.length > 0 && items.every((item) => typeof item.code === 'string' && item.code.length > 0),
      `items=${items.length}`,
    );
    record(
      'conan save DB listEntities action evidence',
      Array.isArray(entities) && entities.length > 0 && entities.every((entity) => typeof entity.code === 'string' && entity.code.length > 0),
      `entities=${entities.length}`,
    );
    record(
      'conan save DB listLocations action evidence',
      Array.isArray(locations) && locations.length > 0 && locations.every((entry) =>
        typeof entry.x === 'number' &&
        typeof entry.y === 'number' &&
        typeof entry.z === 'number' &&
        typeof entry.code === 'string' &&
        entry.code.length > 0,
      ),
      `locations=${locations.length}`,
    );
    record(
      'conan save DB getPlayerLocation action evidence',
      Boolean(location && typeof location.x === 'number' && typeof location.y === 'number' && typeof location.z === 'number'),
      `candidate=${locationCandidate ?? '<none>'}`,
    );
    record(
      'conan save DB getPlayerInventory action evidence',
      Array.isArray(inventory),
      `candidate=${inventoryCandidate ?? '<none>'}, inventoryItems=${inventory.length}`,
    );
  } catch (err) {
    record('conan save DB reads', false, errorMessage(err));
    record('conan save DB listItems action evidence', false, errorMessage(err));
    record('conan save DB listEntities action evidence', false, errorMessage(err));
    record('conan save DB listLocations action evidence', false, errorMessage(err));
    record('conan save DB getPlayerLocation action evidence', false, errorMessage(err));
    record('conan save DB getPlayerInventory action evidence', false, errorMessage(err));
  }
}

function checkFreshValidationErrors() {
  const logPath = path.join(process.cwd(), 'logs', 'conan-exiles-takaro.log');
  if (!fs.existsSync(logPath)) {
    record('fresh Takaro validation errors', true, 'bridge log does not exist yet');
    return;
  }

  const lines = fs.readFileSync(logPath, 'utf8').split(/\r?\n/);
  const freshErrors = lines.filter((line) => {
    const timestamp = line.match(/^(\S+)/)?.[1];
    if (!timestamp) return false;
    const lineDate = new Date(timestamp);
    if (Number.isNaN(lineDate.getTime()) || lineDate < startedAt) return false;
    return /takaro/i.test(line) && /validation|validate|schema/i.test(line) && /error|failed|reject/i.test(line);
  });

  record('fresh Takaro validation errors', freshErrors.length === 0, `${freshErrors.length} matching log lines`);
}

async function checkMcp(health) {
  if (process.env.TAKARO_CONAN_SKIP_MCP === '1') {
    record('takaro MCP checks', true, 'skipped with TAKARO_CONAN_SKIP_MCP=1');
    return;
  }

  let session = null;
  try {
    session = await initializeMcpSession();
    record('takaro MCP initialize', true, `session=${session ? 'received' : 'missing'}`);
  } catch (err) {
    record('takaro MCP initialize', false, errorMessage(err));
    return;
  }

  const gameServerId = await resolveGameServerId(session, health);
  if (!gameServerId) {
    record('takaro MCP checks', false, 'missing gameServerId from /health and MCP identityToken fallback');
    return;
  }

  const baseArg = { id: gameServerId };

  await checkMcpTool(session, 'gameserverTestReachabilityForId', baseArg, (data) =>
    data?.connectable === true || data?.data?.connectable === true,
  );
  await checkMcpTool(session, 'gameserverGetPlayers', baseArg, (data) => Array.isArray(data?.data));
  await checkMcpTool(session, 'gameserverListBans', baseArg, (data) => Array.isArray(data?.data));
  await checkMcpTool(session, 'gameserverGetMapInfo', baseArg, (data) =>
    data?.enabled === false || data?.data?.enabled === false,
  );
  await checkMcpTool(session, 'gameserverGetMapTile', { id: gameServerId, x: '0', y: '0', z: '0' }, (data) =>
    data?.status === 400 || data?.success === false || data?.data?.status === 400,
  );
  await checkMcpTool(session, 'gameserverExecuteCommand', { id: gameServerId, command: 'help' }, (data) =>
    data?.success === true || data?.data?.success === true || typeof data?.rawResult === 'string',
  );

  if (process.env.TAKARO_CONAN_VERIFY_SEND_MESSAGE === '0') {
    record('takaro MCP gameserverSendMessage', true, 'skipped with TAKARO_CONAN_VERIFY_SEND_MESSAGE=0');
    return;
  }

  const marker = `CODEX_CONAN_VERIFY_${new Date().toISOString().replace(/[-:.]/g, '').slice(0, 15)}Z`;
  await checkMcpTool(session, 'gameserverSendMessage', {
    id: gameServerId,
    message: marker,
    opts: { senderNameOverride: 'Takaro' },
  }, (data) => data?.success === true || data?.sent === true || data?.data?.success === true || data?.data?.sent === true);
}

async function resolveGameServerId(session, health) {
  if (health?.gameServerId) {
    record('takaro MCP gameServerId source', true, 'health');
    return health.gameServerId;
  }

  if (!config?.identityToken) {
    record('takaro MCP gameServerId source', false, 'health missing gameServerId and config has no identityToken');
    return null;
  }

  try {
    const data = await callMcpTool(session, 'gameserverSearch', {
      filters: { identityToken: [config.identityToken] },
      limit: 10,
    });
    const matches = Array.isArray(data?.data) ? data.data : [];
    if (matches.length === 1 && matches[0]?.id) {
      record('takaro MCP gameServerId source', true, 'MCP identityToken filter');
      return matches[0].id;
    }
    record('takaro MCP gameServerId source', false, `MCP identityToken filter returned ${matches.length} matches`);
  } catch (err) {
    record('takaro MCP gameServerId source', false, errorMessage(err));
  }
  return null;
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

  if (!response.ok) throw new Error(`HTTP ${response.status}`);
  const session = response.headers.get('mcp-session-id');
  if (!session) throw new Error('missing Mcp-Session-Id response header');
  return session;
}

async function checkMcpTool(session, name, args, predicate) {
  try {
    const data = await callMcpTool(session, name, args);
    const ok = predicate(data);
    record(`takaro MCP ${name}`, ok, compact(JSON.stringify(data)));
  } catch (err) {
    record(`takaro MCP ${name}`, false, errorMessage(err));
  }
  if (mcpActionGapMs > 0) await sleep(mcpActionGapMs);
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
      params: {
        name,
        arguments: args,
      },
      id: Date.now(),
    }),
  });

  if (!response.ok) throw new Error(`HTTP ${response.status}`);
  const body = await response.json();
  if (body.error) throw new Error(JSON.stringify(body.error));

  const text = body.result?.content?.find((item) => item.type === 'text')?.text;
  if (typeof text !== 'string') return body.result ?? {};

  try {
    return JSON.parse(text);
  } catch {
    return { text };
  }
}

function helpOutputShowsCommand(output, command) {
  const normalized = output.toLowerCase();
  if (normalized.includes('unknown command')) return false;

  const header = `commands matching search string: ${command.toLowerCase()}`;
  const remainder = normalized.replace(header, '').trim();
  return remainder.includes('usage:') || remainder.includes(` ${command.toLowerCase()} `) || remainder.includes(`${command.toLowerCase()} <`);
}

async function rconWithRetry(command) {
  let lastError = null;
  for (let attempt = 1; attempt <= 3; attempt += 1) {
    try {
      return await rcon(command);
    } catch (err) {
      lastError = err;
      if (attempt < 3) await sleep(rconRetryGapMs);
    }
  }
  throw lastError;
}

function sleep(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

function compact(value) {
  return value.replace(/\s+/g, ' ').trim().slice(0, 160) || '<empty>';
}

function errorMessage(err) {
  return err instanceof Error ? err.message : String(err);
}

const health = await checkHealth();

if (config) {
  await checkConanPlayerReadActions();
  checkConanSaveDb();
} else {
  record('rcon help probes', true, 'skipped; set BRIDGE_CONFIG or run from a directory with TakaroConfig.txt');
}

checkFreshValidationErrors();
await checkMcp(health);

if (config && !skipRconProbes) {
  for (const [command, expectedAvailable] of [
    ['teleport', false],
    ['teleportplayer', false],
    ['setplayerpos', false],
    ['getplayerpos', false],
    ['inventory', false],
    ['giveitem', false],
    ['spawnitem', false],
    ['listitems', false],
    ['con', true],
    ['listbans', true],
    ['server', true],
    ['directmessage', true],
  ]) {
    await checkRconHelp(command, expectedAvailable);
    await sleep(rconProbeGapMs);
  }
  await checkRconCommand('listbans');
} else if (config) {
  const reason = process.env.TAKARO_CONAN_SKIP_RCON_PROBES === '1'
    ? 'skipped with TAKARO_CONAN_SKIP_RCON_PROBES=1'
    : 'skipped by default; set TAKARO_CONAN_RUN_RCON_PROBES=1 for the RCON help sweep';
  record('rcon help probes', true, reason);
}

if (checks.some((check) => !check.ok)) {
  process.exitCode = 1;
}

function findConfigPath() {
  const candidates = [
    process.env.BRIDGE_CONFIG,
    path.join(process.cwd(), 'TakaroConfig.txt'),
    path.resolve(process.cwd(), '../../../../.runtime/conan-bridge/TakaroConfig.txt'),
  ].filter(Boolean);

  for (const candidate of candidates) {
    if (fs.existsSync(candidate)) return candidate;
  }

  return null;
}
