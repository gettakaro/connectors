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

  assert.deepEqual(await adapter.handleAction('listEntities', {}), []);
  assert.deepEqual(await adapter.handleAction('listLocations', {}), []);
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

function inventoryAdapter(rawResult: string, success = true) {
  const tshock = new FakeTShock();
  tshock.rawCommand = async (command: string) => {
    tshock.rawCommands.push(command);
    return { success, rawResult };
  };
  const adapter = new TerrariaAdapter(tshock, {
    commandAllowlistExact: [],
    commandAllowlistPrefixes: [],
    enableShutdown: false,
  });
  return { tshock, adapter };
}

test('reads plugin-backed player inventory from TShock raw command output', async () => {
  const { tshock, adapter } = inventoryAdapter(
    'TAKARO_INVENTORY {"items":[{"code":"3506","name":"Copper Coin","amount":42,"quality":""},{"code":"29","name":"Life Crystal","amount":3,"quality":""}]}',
  );

  assert.deepEqual(await adapter.handleAction('getPlayerInventory', { player: { name: 'Guide' } }), [
    { code: '3506', name: 'Copper Coin', amount: 42, quality: '' },
    { code: '29', name: 'Life Crystal', amount: 3, quality: '' },
  ]);
  assert.deepEqual(tshock.rawCommands, ['/takaroinv "Guide"']);
});

test('returns an empty inventory array for an empty, absent, or failed marker', async () => {
  const empty = inventoryAdapter('TAKARO_INVENTORY {"items":[]}');
  assert.deepEqual(await empty.adapter.handleAction('getPlayerInventory', { player: { name: 'Guide' } }), []);

  const absent = inventoryAdapter('Invalid command.');
  assert.deepEqual(await absent.adapter.handleAction('getPlayerInventory', { player: { name: 'Guide' } }), []);

  const notFound = inventoryAdapter("No player found matching 'Ghost'.");
  assert.deepEqual(await notFound.adapter.handleAction('getPlayerInventory', { player: { name: 'Ghost' } }), []);

  const ambiguous = inventoryAdapter("Multiple players found matching 'G'.");
  assert.deepEqual(await ambiguous.adapter.handleAction('getPlayerInventory', { player: { name: 'G' } }), []);

  const failed = inventoryAdapter('TAKARO_INVENTORY {"items":[{"code":"9","name":"Wood","amount":1,"quality":""}]}', false);
  assert.deepEqual(await failed.adapter.handleAction('getPlayerInventory', { player: { name: 'Guide' } }), []);
});

test('keeps parsing the inventory marker when the plugin adds fields alongside items', async () => {
  // The marker payload is allowed to grow; the items array must keep parsing unchanged.
  const { adapter } = inventoryAdapter(
    'TAKARO_INVENTORY {"items":[{"code":"9","name":"Wood","amount":1,"quality":""}],"capacity":{"freeSlots":0,"totalSlots":50}}',
  );

  assert.deepEqual(await adapter.handleAction('getPlayerInventory', { player: { name: 'Guide' } }), [
    { code: '9', name: 'Wood', amount: 1, quality: '' },
  ]);
});

test('never throws on malformed inventory payloads and drops malformed entries', async () => {
  const malformed = inventoryAdapter('TAKARO_INVENTORY {"items":[{"code":"9",');
  assert.deepEqual(await malformed.adapter.handleAction('getPlayerInventory', { player: { name: 'Guide' } }), []);

  const notAnArray = inventoryAdapter('TAKARO_INVENTORY {"items":"nope"}');
  assert.deepEqual(await notAnArray.adapter.handleAction('getPlayerInventory', { player: { name: 'Guide' } }), []);

  const mixed = inventoryAdapter(
    'TAKARO_INVENTORY {"items":['
    + '{"code":"9","name":"Wood","amount":1,"quality":""},'
    + '{"code":"1","amount":5,"quality":""},'
    + '{"code":"2","name":"Dirt Block","amount":"lots","quality":""},'
    + 'null,'
    + '"garbage",'
    + '{"code":29,"name":"Life Crystal","amount":"3","quality":""}'
    + ']}',
  );
  assert.deepEqual(await mixed.adapter.handleAction('getPlayerInventory', { player: { name: 'Guide' } }), [
    { code: '9', name: 'Wood', amount: 1, quality: '' },
    { code: '29', name: 'Life Crystal', amount: 3, quality: '' },
  ]);

  const noIdentifier = inventoryAdapter('TAKARO_INVENTORY {"items":[]}');
  assert.deepEqual(await noIdentifier.adapter.handleAction('getPlayerInventory', {}), []);
  assert.deepEqual(noIdentifier.tshock.rawCommands, []);
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

  // giveItem goes through the plugin's /takarogive, not TShock's /give: /give refuses
  // whenever no inventory slot is completely empty, which silently lost shop purchases.
  assert.ok(tshock.rawCommands.includes('/takarogive "Guide" "29" 3'));
  assert.ok(tshock.rawCommands.includes('/takarogive "Guide" "9" 1'));
  assert.ok(!tshock.rawCommands.some((command) => command.startsWith('/give ')));
  assert.ok(tshock.rawCommands.includes('/takarotp "Guide" 10 20'));
});

test('getPlayers returns an array when TShock is unreachable', async () => {
  // Regression for "Expected array for action getPlayers but got object": an unreachable
  // TShock made refreshPlayers throw, the request handler answered with the error envelope
  // { message }, and Takaro rejected that object against the array schema for getPlayers.
  const unreachable = {
    async players(): Promise<never> {
      const err = new Error('fetch failed');
      (err as Error & { code?: string }).code = 'ECONNREFUSED';
      throw err;
    },
  } as unknown as TShockApi;
  const adapter = new TerrariaAdapter(unreachable, {
    commandAllowlistExact: [],
    commandAllowlistPrefixes: [],
    enableShutdown: false,
  });

  const result = await adapter.handleAction('getPlayers', {});

  assert.ok(Array.isArray(result), `expected an array, got ${typeof result}: ${JSON.stringify(result)}`);
  assert.deepEqual(result, []);
});

test('getPlayers returns an array when TShock status omits the players field', async () => {
  const noPlayersField = {
    async players() {
      return [];
    },
  } as unknown as TShockApi;
  const adapter = new TerrariaAdapter(noPlayersField, {
    commandAllowlistExact: [],
    commandAllowlistPrefixes: [],
    enableShutdown: false,
  });

  const result = await adapter.handleAction('getPlayers', {});

  assert.ok(Array.isArray(result));
  assert.deepEqual(result, []);
});

test('the poller still sees TShock failures after the getPlayers action stops throwing', async () => {
  // getPlayers (the action) swallows to []; getPlayers (the poller read) must not, or the
  // health signal built on poll outcomes would go blind.
  const unreachable = {
    async players(): Promise<never> {
      throw new Error('fetch failed');
    },
  } as unknown as TShockApi;
  const adapter = new TerrariaAdapter(unreachable, {
    commandAllowlistExact: [],
    commandAllowlistPrefixes: [],
    enableShutdown: false,
  });

  await assert.rejects(() => adapter.getPlayers(), /fetch failed/);
});

test('delivers a giveItem into the inventory and reports the unchanged success shape', async () => {
  const tshock = new FakeTShock();
  tshock.rawCommand = async (command: string) => {
    tshock.rawCommands.push(command);
    return {
      success: true,
      rawResult: 'TAKARO_GIVE {"success":true,"delivered":7,"method":"inventory","item":"Gold Bar"}',
    };
  };
  const adapter = new TerrariaAdapter(tshock, {
    commandAllowlistExact: [],
    commandAllowlistPrefixes: [],
    enableShutdown: false,
  });

  assert.deepEqual(await adapter.handleAction('giveItem', { player: { name: 'CodexTest' }, itemCode: '19', amount: 7 }), {
    success: true,
    rawResult: 'Gave 7x 19 to CodexTest',
  });
  assert.deepEqual(tshock.rawCommands, ['/takarogive "CodexTest" "19" 7']);
});

test('reports a giveItem that had to be dropped at the player\'s feet as a visible success', async () => {
  const tshock = new FakeTShock();
  tshock.rawCommand = async (command: string) => {
    tshock.rawCommands.push(command);
    return {
      success: true,
      rawResult: 'TAKARO_GIVE {"success":true,"delivered":7,"method":"dropped","item":"Gold Bar"}',
    };
  };
  const adapter = new TerrariaAdapter(tshock, {
    commandAllowlistExact: [],
    commandAllowlistPrefixes: [],
    enableShutdown: false,
  });

  // A full inventory must not fail the purchase: the plugin drops the item at the player's
  // feet, so the player still receives it. The response says so rather than hiding it.
  const result = await adapter.handleAction('giveItem', { player: { name: 'CodexTest' }, itemCode: '19', amount: 7 }) as { success: boolean; rawResult: string };
  assert.equal(result.success, true);
  assert.match(result.rawResult, /dropped at their feet/);
  assert.deepEqual(tshock.rawCommands, ['/takarogive "CodexTest" "19" 7']);
});

test('returns an error for a giveItem the plugin could not deliver at all', async () => {
  const tshock = new FakeTShock();
  tshock.rawCommand = async (command: string) => {
    tshock.rawCommands.push(command);
    return {
      success: true,
      rawResult: 'TAKARO_GIVE {"success":false,"delivered":0,"method":"none","reason":"Unknown item id \'999999\'."}',
    };
  };
  const adapter = new TerrariaAdapter(tshock, {
    commandAllowlistExact: [],
    commandAllowlistPrefixes: [],
    enableShutdown: false,
  });

  // Takaro's generic connector throws on a response carrying an `error` key, so a genuine
  // delivery failure becomes visible instead of completing quietly.
  assert.deepEqual(await adapter.handleAction('giveItem', { player: { name: 'CodexTest' }, itemCode: '999999', amount: 1 }), {
    error: "Failed to give item to CodexTest: Unknown item id '999999'.",
  });
});

test('returns an error carrying the reason when the give command itself fails', async () => {
  const tshock = new FakeTShock();
  tshock.rawCommand = async (command: string) => {
    tshock.rawCommands.push(command);
    return { success: false, rawResult: "No player found matching 'Ghost'." };
  };
  const adapter = new TerrariaAdapter(tshock, {
    commandAllowlistExact: [],
    commandAllowlistPrefixes: [],
    enableShutdown: false,
  });

  assert.deepEqual(await adapter.handleAction('giveItem', { player: { name: 'Ghost' }, itemCode: '19', amount: 7 }), {
    error: "No player found matching 'Ghost'.",
  });

  // The other rawCommand-backed actions still carry the reason through as a CommandResult.
  for (const [action, args] of [
    ['teleportPlayer', { player: { name: 'Ghost' }, x: 10, y: 20 }],
    ['kickPlayer', { player: { name: 'Ghost' } }],
  ] as const) {
    const result = await adapter.handleAction(action, args) as { success: boolean; rawResult: string };
    assert.equal(result.success, false, `expected ${action} to report failure`);
    assert.equal(result.rawResult, "No player found matching 'Ghost'.");
  }
});

test('surfaces a thrown giveItem transport failure as an error', async () => {
  const tshock = new FakeTShock();
  tshock.rawCommand = async () => {
    throw new Error('fetch failed');
  };
  const adapter = new TerrariaAdapter(tshock, {
    commandAllowlistExact: [],
    commandAllowlistPrefixes: [],
    enableShutdown: false,
  });

  assert.deepEqual(await adapter.handleAction('giveItem', { player: { name: 'CodexTest' }, itemCode: '19', amount: 1 }), {
    error: 'Failed to give item to CodexTest: fetch failed',
  });
});
