import assert from 'node:assert/strict';
import { test } from 'node:test';
import { TerrariaAdapter, type TShockApi } from '../terraria/adapter.js';

class FakeTShock implements TShockApi {
  rawCommands: string[] = [];
  broadcasts: string[] = [];
  bans = [{ name: 'BadPlayer', ip: '10.0.0.1', reason: 'test' }];

  async testToken() {
    return { success: true, rawResult: 'ok' };
  }

  async status() {
    return { name: 'Terraria', port: 7777, playercount: 1 };
  }

  async players() {
    return [
      {
        gameId: 'guide-user',
        name: 'Guide',
        platformId: 'terraria:guide-user',
        ip: '127.0.0.1',
        group: 'default',
        active: true,
        state: 10,
        team: 1,
      },
    ];
  }

  async broadcast(message: string) {
    this.broadcasts.push(message);
    return { success: true, rawResult: message };
  }

  async rawCommand(command: string) {
    this.rawCommands.push(command);
    return { success: true, rawResult: `ran ${command}` };
  }

  async createBan(input: { name?: string; ip?: string; reason?: string }) {
    this.bans.push({ name: input.name || '', ip: input.ip || '', reason: input.reason || '' });
    return { success: true, rawResult: 'ban created' };
  }

  async destroyBan() {
    return { success: true, rawResult: 'ban removed' };
  }

  async listBans() {
    return this.bans;
  }

  async shutdown() {
    return { success: true, rawResult: 'shutdown' };
  }
}

test('maps TShock players to Takaro player DTOs and resolves identities', async () => {
  const tshock = new FakeTShock();
  const adapter = new TerrariaAdapter(tshock, {
    commandAllowlistExact: ['help'],
    commandAllowlistPrefixes: ['say'],
    enableShutdown: false,
  });

  assert.deepEqual(await adapter.handleAction('getPlayers', {}), [
    {
      gameId: 'guide-user',
      name: 'Guide',
      platformId: 'terraria:guide-user',
      ip: '127.0.0.1',
      group: 'default',
      active: true,
      state: 10,
      team: 1,
    },
  ]);
  assert.equal((await adapter.handleAction('getPlayer', { gameId: 'guide-user' }) as { name: string }).name, 'Guide');
  assert.equal(await adapter.handleAction('getPlayer', { gameId: 'missing' }), null);
});

test('returns schema-compatible fallbacks for constrained Terraria gaps', async () => {
  const adapter = new TerrariaAdapter(new FakeTShock(), {
    commandAllowlistExact: ['help'],
    commandAllowlistPrefixes: [],
    enableShutdown: false,
  });

  assert.deepEqual(await adapter.handleAction('getPlayerInventory', {}), []);
  assert.deepEqual(await adapter.handleAction('getMapInfo', {}), {
    enabled: false,
    mapBlockSize: 0,
    maxZoom: 0,
    mapSizeX: 0,
    mapSizeY: 0,
    mapSizeZ: 0,
  });
});

test('reads plugin-backed player location from TShock raw command output', async () => {
  const tshock = new FakeTShock();
  tshock.rawCommand = async (command: string) => {
    tshock.rawCommands.push(command);
    return { success: true, rawResult: 'TAKARO_POSITION {"x":123.5,"y":456.25,"z":0}' };
  };
  const adapter = new TerrariaAdapter(tshock, {
    commandAllowlistExact: [],
    commandAllowlistPrefixes: [],
    enableShutdown: false,
  });

  assert.deepEqual(await adapter.handleAction('getPlayerLocation', { player: { name: 'Guide' } }), {
    x: 123.5,
    y: 456.25,
    z: 0,
  });
  assert.deepEqual(tshock.rawCommands, ['/takaropos "Guide"']);
});

test('returns a Terraria item catalog for Takaro item pickers', async () => {
  const adapter = new TerrariaAdapter(new FakeTShock(), {
    commandAllowlistExact: ['help'],
    commandAllowlistPrefixes: [],
    enableShutdown: false,
  });

  const items = await adapter.handleAction('listItems', {}) as Array<{ code: string; name: string; amount: number; quality: string }>;

  assert.ok(items.length > 1000);
  assert.deepEqual(items.find((item) => item.name === 'Wood'), {
    code: '9',
    name: 'Wood',
    amount: 1,
    quality: '0',
    aliases: [],
  });
  assert.equal(items.find((item) => item.code === '2')?.name, 'Dirt Block');
});

test('executes allowlisted commands, normalizes nested player arguments, and gates shutdown', async () => {
  const tshock = new FakeTShock();
  const adapter = new TerrariaAdapter(tshock, {
    commandAllowlistExact: ['/help'],
    commandAllowlistPrefixes: ['say'],
    enableShutdown: false,
    serverChatName: 'Takaro',
  });

  assert.equal((await adapter.handleAction('executeConsoleCommand', { command: '/help' }) as { success: boolean }).success, true);
  assert.equal((await adapter.handleAction('executeConsoleCommand', { command: 'say hello' }) as { success: boolean }).success, true);
  assert.equal((await adapter.handleAction('executeConsoleCommand', { command: '/say hello' }) as { success: boolean }).success, true);
  assert.equal((await adapter.handleAction('executeConsoleCommand', { command: 'off' }) as { success: false }).success, false);
  assert.equal((await adapter.handleAction('sendMessage', { message: 'hello' }) as { success: boolean }).success, true);
  assert.equal((await adapter.handleAction('sendMessage', { message: 'hello', opts: { senderNameOverride: 'Discord Hendrik' } }) as { success: boolean }).success, true);
  assert.equal((await adapter.handleAction('kickPlayer', { player: { name: 'Guide' }, reason: 'test' }) as { success: boolean }).success, true);
  assert.equal((await adapter.handleAction('banPlayer', { player: { name: 'BadPlayer' }, reason: 'test' }) as { success: boolean }).success, true);
  assert.equal((await adapter.handleAction('unbanPlayer', { player: { name: 'BadPlayer' } }) as { success: boolean }).success, true);
  assert.equal((await adapter.handleAction('shutdown', {}) as { success: false }).success, false);

  assert.deepEqual(tshock.broadcasts, ['Takaro: hello', 'Discord Hendrik: hello']);
  assert.deepEqual(tshock.rawCommands.slice(0, 4), ['/help', 'say hello', '/say hello', '/kick "Guide" "test"']);
});

test('supports give item and plugin-backed teleport through guarded raw TShock commands', async () => {
  const tshock = new FakeTShock();
  const adapter = new TerrariaAdapter(tshock, {
    commandAllowlistExact: ['help'],
    commandAllowlistPrefixes: [],
    enableShutdown: true,
  });

  assert.equal((await adapter.handleAction('giveItem', { player: { name: 'Guide' }, itemCode: '29', amount: 3 }) as { success: boolean }).success, true);
  assert.equal((await adapter.handleAction('giveItem', { player: { name: 'Guide' }, name: 'Wood', amount: 1 }) as { success: boolean }).success, true);
  assert.equal((await adapter.handleAction('teleportPlayer', { player: { name: 'Guide' }, x: 10, y: 20 }) as { success: boolean }).success, true);
  assert.equal((await adapter.handleAction('shutdown', {}) as { success: boolean }).success, true);

  assert.ok(tshock.rawCommands.includes('/give "29" "Guide" 3'));
  assert.ok(tshock.rawCommands.includes('/give "9" "Guide" 1'));
  assert.ok(tshock.rawCommands.includes('/takarotp "Guide" 10 20'));
});
