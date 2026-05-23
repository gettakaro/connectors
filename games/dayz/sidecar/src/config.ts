import fs from 'node:fs';
import path from 'node:path';

export interface BridgeConfig {
  registrationToken: string;
  serverName: string;
  identityToken: string;
  takaroWsUrl: string;
  localPort: number;
}

export function loadConfig(configPath = 'TakaroConfig.txt'): BridgeConfig {
  if (!fs.existsSync(configPath)) {
    throw new Error(
      `Config file not found at ${path.resolve(configPath)}. Copy TakaroConfig.example.txt and fill in registrationToken + serverName.`,
    );
  }

  const raw = fs.readFileSync(configPath, 'utf8');
  const kv: Record<string, string> = {};
  for (const line of raw.split(/\r?\n/)) {
    const trimmed = line.trim();
    if (!trimmed || trimmed.startsWith('#')) continue;
    const eq = trimmed.indexOf('=');
    if (eq < 0) continue;
    kv[trimmed.slice(0, eq).trim()] = trimmed.slice(eq + 1).trim();
  }

  for (const key of ['registrationToken', 'serverName']) {
    if (!kv[key]) throw new Error(`Missing required config: ${key}`);
  }

  return {
    registrationToken: kv.registrationToken!,
    serverName: kv.serverName!,
    identityToken: kv.identityToken || kv.serverName!,
    takaroWsUrl: kv.takaroWsUrl || 'wss://connect.takaro.io/',
    localPort: parseInt(kv.localPort || '8088', 10),
  };
}
