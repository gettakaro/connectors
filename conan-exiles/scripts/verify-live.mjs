#!/usr/bin/env node

import fs from 'node:fs';
import path from 'node:path';
import { loadConfig } from '../dist/config.js';
import { loadConanItemCatalog } from '../dist/conan/itemCatalog.js';
import { ConanSaveDbReader } from '../dist/conan/saveDb.js';
import { sendRconCommand } from '../dist/rcon/client.js';

const startedAt = new Date();
const configPath = findConfigPath();
const config = configPath ? loadConfig(configPath) : null;
const healthPort = Number.parseInt(process.env.TAKARO_CONAN_HEALTH_PORT || '', 10) || config?.httpPort || 3010;
const healthUrl = `http://127.0.0.1:${healthPort}/health`;

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
    record('bridge health', response.ok && body.ok === true, `gameServerId=${body.gameServerId ?? '<none>'}`);
    return body.gameServerId ?? null;
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

function checkConanSaveDb() {
  if (!config?.databasePath) {
    record('conan save DB reads', true, 'skipped; databasePath is not configured');
    return;
  }

  try {
    const reader = new ConanSaveDbReader(config.databasePath, loadConanItemCatalog(config.itemCatalogPath));
    const items = reader.listItems();
    const entities = reader.listEntities();
    const locations = reader.listPlayerLocations();
    record(
      'conan save DB reads',
      items.length > 0 && entities.length > 0 && locations.length > 0,
      `items=${items.length}, entities=${entities.length}, locations=${locations.length}`,
    );
  } catch (err) {
    record('conan save DB reads', false, errorMessage(err));
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

function printMcpRunbook(gameServerId) {
  console.log('');
  console.log('Takaro MCP checks to run from Codex MCP:');
  console.log(`- gameserverGetPlayers({ id: '${gameServerId ?? '<gameServerId from /health>'}' })`);
  console.log(`- gameserverListBans({ id: '${gameServerId ?? '<gameServerId from /health>'}' })`);
  console.log(`- gameserverSendMessage({ id: '${gameServerId ?? '<gameServerId from /health>'}', message: 'CODEX_CONAN_VERIFY_<timestamp>' })`);
  console.log(`- gameserverExecuteCommand({ id: '${gameServerId ?? '<gameServerId from /health>'}', command: 'help' })`);
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
      if (attempt < 3) await new Promise((resolve) => setTimeout(resolve, 1500));
    }
  }
  throw lastError;
}

function compact(value) {
  return value.replace(/\s+/g, ' ').trim().slice(0, 160) || '<empty>';
}

function errorMessage(err) {
  return err instanceof Error ? err.message : String(err);
}

const gameServerId = await checkHealth();

if (config) {
  checkConanSaveDb();
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
    await new Promise((resolve) => setTimeout(resolve, 1500));
  }
  await checkRconCommand('listbans');
} else {
  record('rcon help probes', true, 'skipped; set BRIDGE_CONFIG or run from a directory with TakaroConfig.txt');
}

checkFreshValidationErrors();
printMcpRunbook(gameServerId);

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
