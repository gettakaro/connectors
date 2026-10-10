import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { afterEach, describe, expect, it, vi } from 'vitest';
import { WebSocketServer, type WebSocket } from 'ws';
import { describeTakaroError, TakaroWsClient } from '../takaro/client.js';
import { IDENTITY_FILE_NAME, LEGACY_IDENTITY, parseDotenv, resolveConfigFile, TakaroSettingsSource } from '../takaro/settings.js';
import { redactSecrets } from '../logger.js';

const UUID = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/;

function waitFor<T>(fn: () => T | undefined | false, timeoutMs = 4000): Promise<T> {
  const start = Date.now();
  return new Promise((resolve, reject) => {
    const tick = (): void => {
      const v = fn();
      if (v !== undefined && v !== false) return resolve(v as T);
      if (Date.now() - start > timeoutMs) return reject(new Error('waitFor timeout'));
      setTimeout(tick, 10);
    };
    tick();
  });
}

const dirs: string[] = [];
function tmp(): string {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'dune-cfg-'));
  dirs.push(dir);
  return dir;
}
afterEach(() => {
  for (const d of dirs.splice(0)) fs.rmSync(d, { recursive: true, force: true });
});

function install(opts: { env?: Record<string, string>; file?: string; state?: boolean; savedIdentity?: string } = {}) {
  const root = tmp();
  const dataDir = path.join(root, 'data');
  fs.mkdirSync(dataDir);
  const configFile = path.join(root, '.env');
  if (opts.file !== undefined) fs.writeFileSync(configFile, opts.file);
  if (opts.state) fs.writeFileSync(path.join(dataDir, 'known-players.json'), '{}');
  if (opts.savedIdentity) fs.writeFileSync(path.join(dataDir, IDENTITY_FILE_NAME), JSON.stringify({ identityToken: opts.savedIdentity }));
  const make = (env = opts.env ?? {}) =>
    new TakaroSettingsSource({
      env,
      configFile,
      dataDir,
      legacyStateFiles: ['event-cursor.json', 'online-players.json', 'bans.json', 'known-players.json'].map((f) => path.join(dataDir, f)),
    });
  const saved = () => (JSON.parse(fs.readFileSync(path.join(dataDir, IDENTITY_FILE_NAME), 'utf8')) as { identityToken: string }).identityToken;
  return { root, dataDir, configFile, make, saved };
}

describe('parseDotenv', () => {
  it('reads what compose --env-file and a shell agree on', () => {
    expect(
      parseDotenv(
        [
          '# comment',
          'TAKARO_REGISTRATION_TOKEN=            # registration token for a GENERIC game server',
          'TAKARO_IDENTITY_TOKEN="my server"',
          "export TAKARO_WS_URL='wss://x/'",
          'TAKARO_SERVER_NAME=Dune#1',
          'garbage line',
          'DUNE_PG_URL=postgres://u:p@h/db',
        ].join('\r\n'),
      ),
    ).toEqual({
      TAKARO_REGISTRATION_TOKEN: '',
      TAKARO_IDENTITY_TOKEN: 'my server',
      TAKARO_WS_URL: 'wss://x/',
      TAKARO_SERVER_NAME: 'Dune#1',
      DUNE_PG_URL: 'postgres://u:p@h/db',
    });
  });

  it('looks for .env beside the sidecar folder unless TAKARO_CONFIG_FILE says otherwise', () => {
    expect(resolveConfigFile({}, '/opt/dune-connector/sidecar')).toBe('/opt/dune-connector/.env');
    expect(resolveConfigFile({ TAKARO_CONFIG_FILE: '/takaro-config/.env' }, '/app')).toBe('/takaro-config/.env');
  });
});

describe('identity', () => {
  it('a fresh install generates a UUID, saves it, and keeps it on the next start (upgrade)', () => {
    const i = install({ file: 'TAKARO_IDENTITY_TOKEN=\n' });
    const first = i.make().current().identityToken;
    expect(first).toMatch(UUID);
    expect(i.saved()).toBe(first);
    expect(i.make().current().identityToken).toBe(first);
  });

  it(`an existing install with state but no identity keeps the old default '${LEGACY_IDENTITY}'`, () => {
    const i = install({ state: true });
    expect(i.make().current().identityToken).toBe(LEGACY_IDENTITY);
    expect(i.saved()).toBe(LEGACY_IDENTITY);
    // ...and still does after the state files are gone, because it was saved.
    fs.rmSync(path.join(i.dataDir, 'known-players.json'));
    expect(i.make().current().identityToken).toBe(LEGACY_IDENTITY);
  });

  it('a configured identity wins over the saved one and is saved in turn', () => {
    const i = install({ env: { TAKARO_IDENTITY_TOKEN: 'my-dune' }, savedIdentity: 'old' });
    expect(i.make().current().identityToken).toBe('my-dune');
    expect(i.saved()).toBe('my-dune');
    // Removing it from the configuration later keeps it rather than generating a new one.
    expect(i.make({}).current().identityToken).toBe('my-dune');
  });

  it('the saved identity is kept when nothing is configured, even with state present', () => {
    const i = install({ state: true, savedIdentity: '0f7d1c3e-1111-4222-8333-444455556666' });
    expect(i.make().current().identityToken).toBe('0f7d1c3e-1111-4222-8333-444455556666');
  });
});

describe('settings precedence and reload', () => {
  it('the environment wins at start, exactly as before', () => {
    const i = install({ env: { TAKARO_REGISTRATION_TOKEN: 'from-env' }, file: 'TAKARO_REGISTRATION_TOKEN=from-file\n' });
    expect(i.make().current().registrationToken).toBe('from-env');
  });

  it('an empty environment value falls back to the file', () => {
    const i = install({ env: { TAKARO_REGISTRATION_TOKEN: '' }, file: 'TAKARO_REGISTRATION_TOKEN=from-file\n' });
    expect(i.make().current().registrationToken).toBe('from-file');
  });

  it('no file at all = environment only, with the old defaults', () => {
    const i = install({ env: { TAKARO_REGISTRATION_TOKEN: 'reg' } });
    const s = i.make();
    expect(s.hasConfigFile()).toBe(false);
    expect(s.current()).toMatchObject({ registrationToken: 'reg', wsUrl: 'wss://connect.takaro.io/', serverName: 'Takaro Dev Dune' });
    expect(s.poll()).toBeNull();
  });

  it('a token saved into the file while running wins over the stale container environment (docker env_file)', () => {
    const i = install({ env: { TAKARO_REGISTRATION_TOKEN: 'old' }, file: 'TAKARO_REGISTRATION_TOKEN=old\nDUNE_PG_URL=x\n' });
    const s = i.make();
    expect(s.poll()).toBeNull();
    // Same text again: nothing to do.
    fs.writeFileSync(i.configFile, 'TAKARO_REGISTRATION_TOKEN=old\nDUNE_PG_URL=x\n');
    expect(s.poll()).toBeNull();
    // Another key changed: text differs, but no Takaro setting did.
    fs.writeFileSync(i.configFile, 'TAKARO_REGISTRATION_TOKEN=old\nDUNE_PG_URL=y\n');
    expect(s.poll()).toBeNull();
    fs.writeFileSync(i.configFile, 'TAKARO_REGISTRATION_TOKEN=new\nDUNE_PG_URL=y\n');
    expect(s.poll()).toMatchObject({ registrationToken: 'new' });
    expect(s.current().registrationToken).toBe('new');
  });

  it('a file that appears after start is picked up', () => {
    const i = install({});
    const s = i.make();
    expect(s.current().registrationToken).toBe('');
    fs.writeFileSync(i.configFile, 'TAKARO_REGISTRATION_TOKEN=pasted\n');
    expect(s.poll()).toMatchObject({ registrationToken: 'pasted' });
    expect(s.hasConfigFile()).toBe(true);
  });

  it('a read error mid-save keeps the current settings and retries next tick', () => {
    const i = install({ file: 'TAKARO_REGISTRATION_TOKEN=good\n' });
    let fail = true;
    const s = new TakaroSettingsSource({
      env: {},
      configFile: i.configFile,
      dataDir: i.dataDir,
      legacyStateFiles: [],
      readFile: (f) => {
        if (fail) throw Object.assign(new Error('busy'), { code: 'EBUSY' });
        return fs.readFileSync(f, 'utf8');
      },
    });
    expect(s.current().registrationToken).toBe('');
    expect(s.poll()).toBeNull();
    fail = false;
    expect(s.poll()).toMatchObject({ registrationToken: 'good' });
  });

  it('changing the identity in the file reconnects with it; clearing it keeps the saved one', () => {
    const i = install({ file: 'TAKARO_IDENTITY_TOKEN=\nTAKARO_REGISTRATION_TOKEN=r\n' });
    const s = i.make();
    const generated = s.current().identityToken;
    fs.writeFileSync(i.configFile, 'TAKARO_IDENTITY_TOKEN=renamed\nTAKARO_REGISTRATION_TOKEN=r\n');
    expect(s.poll()).toMatchObject({ identityToken: 'renamed' });
    fs.writeFileSync(i.configFile, 'TAKARO_IDENTITY_TOKEN=\nTAKARO_REGISTRATION_TOKEN=r\n');
    expect(s.poll()).toBeNull();
    expect(s.current().identityToken).toBe('renamed');
    expect(generated).toMatch(UUID);
  });
});

describe('error logging', () => {
  it('logs only name, message and HTTP status of a Takaro error, never its request or JWT', () => {
    const jwt = 'eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOiJ4In0.c2lnbmF0dXJl';
    const err = { name: 'BadRequestError', message: 'Invalid registration token', status: 400, config: { headers: { 'x-takaro-token': jwt } } };
    const text = describeTakaroError(err);
    expect(text).toBe('BadRequestError: Invalid registration token: HTTP 400');
    expect(describeTakaroError(`failed with ${jwt}`)).not.toContain(jwt);
    expect(redactSecrets(`x-takaro-token ${jwt}`)).not.toContain(jwt);
  });
});

describe('TakaroWsClient connection control', () => {
  const cleanups: (() => unknown)[] = [];
  afterEach(async () => {
    for (const c of cleanups.splice(0).reverse()) await c();
    vi.restoreAllMocks();
  });

  async function server(accept: (token: unknown) => boolean) {
    const wss = new WebSocketServer({ port: 0, host: '127.0.0.1' });
    await new Promise((r) => wss.once('listening', r));
    cleanups.push(() => new Promise((r) => wss.close(r)));
    const state = { connections: 0, identifies: [] as Record<string, unknown>[], sockets: [] as WebSocket[] };
    wss.on('connection', (ws) => {
      state.connections += 1;
      state.sockets.push(ws);
      ws.on('message', (raw) => {
        const msg = JSON.parse(raw.toString()) as { type: string; payload: Record<string, unknown> };
        if (msg.type !== 'identify') return;
        state.identifies.push(msg.payload);
        ws.send(
          JSON.stringify(
            accept(msg.payload.registrationToken)
              ? { type: 'identifyResponse', payload: { gameServerId: `gs-${String(msg.payload.identityToken)}` } }
              : { type: 'identifyResponse', payload: { error: { name: 'BadRequestError', message: 'bad token', http: { headers: { 'x-takaro-token': 'eyJa.eyJb.c' } } } } },
          ),
        );
      });
    });
    return { url: `ws://127.0.0.1:${(wss.address() as { port: number }).port}`, state };
  }

  it('does not connect without a registration token, and says so in a banner; a saved token connects at once', async () => {
    const lines: string[] = [];
    vi.spyOn(console, 'warn').mockImplementation((m: string) => void lines.push(m));
    vi.spyOn(console, 'info').mockImplementation(() => undefined);
    const { url, state } = await server(() => true);
    const takaro = new TakaroWsClient(url, { identityToken: 'id-1', registrationToken: '' }, { pingIntervalMs: 0, baseReconnectMs: 60_000 });
    cleanups.push(() => takaro.shutdown());
    takaro.setConfigHint({ file: '/srv/dune-connector/.env', hasFile: true });
    takaro.connect();
    await new Promise((r) => setTimeout(r, 150));
    expect(state.connections).toBe(0);
    expect(lines.join('\n')).toContain('TAKARO_REGISTRATION_TOKEN not set, the server is not connected to Takaro.');
    expect(lines.join('\n')).toContain('/srv/dune-connector/.env');
    expect(lines.join('\n')).toContain('no restart needed');

    takaro.reconfigure(url, { identityToken: 'id-1', registrationToken: 'good' });
    await waitFor(() => takaro.identified() || undefined);
    expect(takaro.getGameServerId()).toBe('gs-id-1');
  });

  it('a rejected identify logs a banner without the JWT; a fixed token reconnects without waiting for the backoff', async () => {
    const errors: string[] = [];
    vi.spyOn(console, 'error').mockImplementation((m: string) => void errors.push(m));
    vi.spyOn(console, 'warn').mockImplementation(() => undefined);
    vi.spyOn(console, 'info').mockImplementation(() => undefined);
    const { url, state } = await server((t) => t === 'good');
    // Backoff of one minute: only `reconfigure` can make the second attempt happen within the test.
    const takaro = new TakaroWsClient(url, { identityToken: 'id-2', registrationToken: 'wrong' }, { pingIntervalMs: 0, baseReconnectMs: 60_000 });
    cleanups.push(() => takaro.shutdown());
    takaro.connect();
    await waitFor(() => errors.some((l) => l.includes('Takaro rejected identify: BadRequestError: bad token')) || undefined);
    expect(errors.join('\n')).not.toContain('eyJa');
    expect(takaro.identified()).toBe(false);

    takaro.reconfigure(url, { identityToken: 'id-2', registrationToken: 'good' });
    await waitFor(() => takaro.identified() || undefined, 2000);
    expect(state.identifies.map((p) => p.registrationToken)).toEqual(['wrong', 'good']);
  });

  it("an old socket's late close does not tear down the new one, and shutdown stops a reconnect", async () => {
    vi.spyOn(console, 'info').mockImplementation(() => undefined);
    vi.spyOn(console, 'warn').mockImplementation(() => undefined);
    const { url, state } = await server(() => true);
    const takaro = new TakaroWsClient(url, { identityToken: 'a', registrationToken: 'r' }, { pingIntervalMs: 0, baseReconnectMs: 20 });
    cleanups.push(() => takaro.shutdown());
    let downs = 0;
    takaro.on('disconnected', () => (downs += 1));
    takaro.connect();
    await waitFor(() => takaro.identified() || undefined);
    takaro.reconfigure(url, { identityToken: 'b', registrationToken: 'r' });
    await waitFor(() => (takaro.identified() && takaro.getGameServerId() === 'gs-b') || undefined);
    // Let the first socket's close callback arrive.
    await new Promise((r) => setTimeout(r, 150));
    expect(takaro.identified()).toBe(true);
    expect(downs).toBe(1);
    expect(state.connections).toBe(2);

    takaro.shutdown();
    takaro.reconfigure(url, { identityToken: 'c', registrationToken: 'r' });
    await new Promise((r) => setTimeout(r, 150));
    expect(state.connections).toBe(2);
  });
});

describe('compose inline-comment values', () => {
  it("treats compose's `# text` value of an empty `KEY=   # text` line as empty", () => {
    const i = install({ env: { TAKARO_REGISTRATION_TOKEN: '# registration token for a GENERIC game server', TAKARO_IDENTITY_TOKEN: '# identity' } });
    const s = i.make().current();
    expect(s.registrationToken).toBe('');
    expect(s.identityToken).toMatch(UUID);
  });
});
