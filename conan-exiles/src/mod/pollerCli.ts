import fs from 'node:fs';
import { loadConfig, type RconConfig } from '../config.js';
import {
  ExternalCommandRenderer,
  HttpBridgeClient,
  ModCommandPoller,
  RconChatModRenderer,
  type CommandRenderer,
  type ConanChatMod,
} from './poller.js';

const bridgeUrl = process.env.TAKARO_CONAN_BRIDGE_URL || 'http://127.0.0.1:3010';
const pollIntervalMs = Number.parseInt(process.env.TAKARO_CONAN_POLL_INTERVAL_MS || '1000', 10);

const renderer = createRenderer();
if (!renderer) {
  console.error(
    [
      'Missing Conan renderer configuration. Refusing to acknowledge Conan chat commands without a renderer.',
      'Set TAKARO_CONAN_RENDER_COMMAND for an external renderer, or set TAKARO_CONAN_CHAT_MOD=pippi|amunet with RCON config.',
    ].join('\n'),
  );
  process.exit(1);
}

const poller = new ModCommandPoller(new HttpBridgeClient(bridgeUrl), renderer);

let stopped = false;
process.on('SIGINT', () => {
  stopped = true;
});
process.on('SIGTERM', () => {
  stopped = true;
});

while (!stopped) {
  try {
    await poller.pollOnce();
  } catch (err) {
    const message = err instanceof Error ? err.message : String(err);
    console.error(`Conan helper poll failed: ${message}`);
  }
  await new Promise((resolve) => setTimeout(resolve, Number.isFinite(pollIntervalMs) ? pollIntervalMs : 1000));
}

function createRenderer(): CommandRenderer | null {
  const rendererCommand = process.env.TAKARO_CONAN_RENDER_COMMAND;
  if (rendererCommand) return new ExternalCommandRenderer(rendererCommand);

  const chatMod = parseChatMod(process.env.TAKARO_CONAN_CHAT_MOD);
  if (!chatMod) return null;

  return new RconChatModRenderer({
    mod: chatMod,
    rcon: loadRcon(),
    senderName: process.env.TAKARO_CONAN_SENDER_NAME,
  });
}

function parseChatMod(value: string | undefined): ConanChatMod | null {
  if (!value) return null;
  const normalized = value.trim().toLowerCase();
  if (normalized === 'pippi') return 'pippi';
  if (normalized === 'amunet' || normalized === 'amunat' || normalized === 'ast') return 'amunet';
  throw new Error(`Unsupported TAKARO_CONAN_CHAT_MOD: ${value}`);
}

function loadRcon(): RconConfig {
  const envConfig = {
    host: process.env.TAKARO_CONAN_RCON_HOST,
    port: process.env.TAKARO_CONAN_RCON_PORT,
    password: process.env.TAKARO_CONAN_RCON_PASSWORD,
    timeoutMs: process.env.TAKARO_CONAN_RCON_TIMEOUT_MS,
  };

  if (envConfig.host && envConfig.port && envConfig.password) {
    return {
      host: envConfig.host,
      port: parseNumber(envConfig.port, 25575),
      password: envConfig.password,
      timeoutMs: parseNumber(envConfig.timeoutMs, 5000),
    };
  }

  const configPath = process.env.BRIDGE_CONFIG || 'TakaroConfig.txt';
  if (fs.existsSync(configPath)) return loadConfig(configPath).rcon;

  throw new Error(
    'Missing RCON config. Set TAKARO_CONAN_RCON_HOST, TAKARO_CONAN_RCON_PORT, and TAKARO_CONAN_RCON_PASSWORD, or provide BRIDGE_CONFIG.',
  );
}

function parseNumber(value: string | undefined, fallback: number): number {
  if (!value) return fallback;
  const parsed = Number.parseInt(value, 10);
  return Number.isFinite(parsed) ? parsed : fallback;
}
