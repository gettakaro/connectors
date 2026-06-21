import assert from 'node:assert/strict';
import { test } from 'node:test';
import { ConanAdapter } from '../conan/adapter.js';
import { ConanItemCatalog } from '../conan/itemCatalog.js';

test('dispatches getPlayers through listplayers parser', async () => {
  const adapter = new ConanAdapter(fakeExecutor({ listplayers: '0. Alice | 76561198000000001' }));

  const result = await adapter.handleAction('getPlayers', {});

  assert.deepEqual(result, [
    {
      gameId: '76561198000000001',
      name: 'Alice',
      steamId: '76561198000000001',
      platformId: 'steam:76561198000000001',
      online: true,
    },
  ]);
});

test('strips connector-only character names from Takaro player action responses', async () => {
  const adapter = new ConanAdapter(
    fakeExecutor({
      listplayers: 'Idx | Char name | Player name |      User ID |       Platform ID | Platform Name\n  0 | TestExile | Tester#1000 | A-TESTUSER | 76561198000000001 |         STEAM',
    }),
  );

  assert.deepEqual(await adapter.handleAction('getPlayers', {}), [
    {
      gameId: '76561198000000001',
      name: 'Tester#1000',
      steamId: '76561198000000001',
      platformId: 'steam:76561198000000001',
      online: true,
    },
  ]);
  assert.deepEqual(await adapter.handleAction('getPlayer', { gameId: '76561198000000001' }), {
    gameId: '76561198000000001',
    name: 'Tester#1000',
    steamId: '76561198000000001',
    platformId: 'steam:76561198000000001',
    online: true,
  });

  assert.deepEqual(await adapter.getPlayers(), [
    {
      gameId: '76561198000000001',
      name: 'Tester#1000',
      rconId: '0',
      characterName: 'TestExile',
      steamId: '76561198000000001',
      platformId: 'steam:76561198000000001',
      online: true,
    },
  ]);
});

test('keeps last known players for log event enrichment when Conan listplayers is temporarily empty', async () => {
  const adapter = new ConanAdapter(sequenceExecutor({
    listplayers: [
      'Idx | Char name | Player name |      User ID |       Platform ID | Platform Name\n  0 | TestExile | Tester#1000 | A-TESTUSER | 76561198000000001 |         STEAM',
      'No players connected',
    ],
  }));

  assert.equal((await adapter.getPlayers()).length, 1);

  assert.deepEqual(await adapter.getKnownPlayersForEvents(), [
    {
      gameId: '76561198000000001',
      name: 'Tester#1000',
      rconId: '0',
      characterName: 'TestExile',
      steamId: '76561198000000001',
      platformId: 'steam:76561198000000001',
      online: true,
    },
  ]);
});

test('routes giveItem through Conan online-player console SpawnItem command', async () => {
  const calls: string[] = [];
  const adapter = new ConanAdapter(async (command) => {
    calls.push(command);
    if (command === 'listplayers') {
      return 'Idx | Char name | Player name |      User ID |       Platform ID | Platform Name\n  0 | TestExile | Tester#1000 | A-TESTUSER | 76561198000000001 |         STEAM';
    }
    return `Successfully executed: ${command}`;
  });

  const result = await adapter.handleAction('giveItem', {
    player: { gameId: '76561198000000001' },
    name: 'Stone',
    amount: 3,
  });

  assert.deepEqual(calls, ['listplayers', 'con 0 SpawnItem 10001 3']);
  assert.deepEqual(result, { success: true, rawResult: 'Successfully executed: con 0 SpawnItem 10001 3' });
});

test('routes giveItem catalog names and aliases through numeric Conan template ids', async () => {
  const calls: string[] = [];
  const catalog = new ConanItemCatalog(
    {
      version: 1,
      source: 'fixture',
      items: [{ templateId: '41001', code: 'Iron_Ore', name: 'Iron Ore', aliases: ['iron'] }],
    },
    { hasExternalCatalog: true },
  );
  const adapter = new ConanAdapter(
    async (command) => {
      calls.push(command);
      if (command === 'listplayers') {
        return 'Idx | Char name | Player name |      User ID |       Platform ID | Platform Name\n  0 | TestExile | Tester#1000 | A-TESTUSER | 76561198000000001 |         STEAM';
      }
      return `Successfully executed: ${command}`;
    },
    undefined,
    undefined,
    catalog,
  );

  const result = await adapter.handleAction('giveItem', {
    player: { gameId: '76561198000000001' },
    name: 'Iron Ore',
    amount: 2,
  });

  assert.deepEqual(calls, ['listplayers', 'con 0 SpawnItem 41001 2']);
  assert.deepEqual(result, { success: true, rawResult: 'Successfully executed: con 0 SpawnItem 41001 2' });
});

test('routes giveItem nested item objects through catalog item codes', async () => {
  const calls: string[] = [];
  const catalog = new ConanItemCatalog(
    {
      version: 1,
      source: 'fixture',
      items: [{ templateId: '51706', code: '51706', name: 'Abysmal Blade' }],
    },
    { hasExternalCatalog: true },
  );
  const adapter = new ConanAdapter(
    async (command) => {
      calls.push(command);
      if (command === 'listplayers') {
        return 'Idx | Char name | Player name |      User ID |       Platform ID | Platform Name\n  0 | TestExile | Tester#1000 | A-TESTUSER | 76561198000000001 |         STEAM';
      }
      return `Successfully executed: ${command}`;
    },
    undefined,
    undefined,
    catalog,
  );

  const result = await adapter.handleAction('giveItem', {
    player: { gameId: '76561198000000001' },
    item: { code: '51706', name: 'Abysmal Blade' },
    amount: 2,
  });

  assert.deepEqual(calls, ['listplayers', 'con 0 SpawnItem 51706 2']);
  assert.deepEqual(result, { success: true, rawResult: 'Successfully executed: con 0 SpawnItem 51706 2' });
});

test('routes fallback Conan item labels through numeric Conan template ids', async () => {
  const calls: string[] = [];
  const adapter = new ConanAdapter(async (command) => {
    calls.push(command);
    if (command === 'listplayers') {
      return 'Idx | Char name | Player name |      User ID |       Platform ID | Platform Name\n  0 | TestExile | Tester#1000 | A-TESTUSER | 76561198000000001 |         STEAM';
    }
    return `Successfully executed: ${command}`;
  });

  await adapter.handleAction('giveItem', {
    player: { gameId: '76561198000000001' },
    name: 'Conan item 52562',
    amount: 1,
  });

  assert.deepEqual(calls, ['listplayers', 'con 0 SpawnItem 52562 1']);
});

test('merges Takaro-granted items into inventory until Conan save DB catches up', async () => {
  let savedStoneAmount = 2;
  const adapter = new ConanAdapter(async (command) => {
    if (command === 'listplayers') {
      return 'Idx | Char name | Player name |      User ID |       Platform ID | Platform Name\n  0 | TestExile | Tester#1000 | A-TESTUSER | 76561198000000001 |         STEAM';
    }
    return `Successfully executed: ${command}`;
  }, undefined, {
    getPlayerLocation: () => null,
    getPlayerInventory: (identifier: string) =>
      identifier === 'steam:76561198000000001'
        ? [{ code: 'Stone', name: 'Stone', amount: savedStoneAmount, quality: '1' }]
        : [],
    listItems: () => [],
    listEntities: () => [],
    listPlayerLocations: () => [],
  } as never);

  await adapter.handleAction('giveItem', {
    player: {
      playerId: '11111111-1111-4111-8111-111111111111',
      player: { platformId: 'steam:76561198000000001' },
    },
    name: 'Stone',
    amount: 1,
  });

  assert.deepEqual(
    await adapter.handleAction('getPlayerInventory', {
      player: {
        playerId: '11111111-1111-4111-8111-111111111111',
        player: { platformId: 'steam:76561198000000001' },
      },
    }),
    [
      {
        code: 'Stone',
        name: 'Stone',
        amount: 3,
        quality: '1',
      },
    ],
  );

  savedStoneAmount = 3;
  assert.deepEqual(await adapter.handleAction('getPlayerInventory', { player: { platformId: 'steam:76561198000000001' } }), [
    {
      code: 'Stone',
      name: 'Stone',
      amount: 3,
      quality: '1',
    },
  ]);
});

test('tries every Takaro player identifier when reading Conan save DB fields', async () => {
  const locationLookups: string[] = [];
  const inventoryLookups: string[] = [];
  const saveDb = {
    getPlayerLocation: (identifier: string) => {
      locationLookups.push(identifier);
      return identifier === 'steam:76561198000000001' ? { x: 1, y: 2, z: 3, dimension: 'ConanSandbox' } : null;
    },
    getPlayerInventory: (identifier: string) => {
      inventoryLookups.push(identifier);
      return identifier === 'steam:76561198000000001'
        ? [{ code: 'Stone', name: 'Stone', amount: 2, quality: '1' }]
        : [];
    },
    listItems: () => [],
    listEntities: () => [],
    listPlayerLocations: () => [],
  };
  const adapter = new ConanAdapter(fakeExecutor({}), undefined, saveDb as never);
  const args = {
    player: {
      playerId: '11111111-1111-4111-8111-111111111111',
      player: {
        platformId: 'steam:76561198000000001',
      },
    },
  };

  assert.deepEqual(await adapter.handleAction('getPlayerLocation', args), {
    x: 1,
    y: 2,
    z: 3,
    dimension: 'ConanSandbox',
  });
  assert.deepEqual(await adapter.handleAction('getPlayerInventory', args), [
    { code: 'Stone', name: 'Stone', amount: 2, quality: '1' },
  ]);
  assert.deepEqual(locationLookups, ['11111111-1111-4111-8111-111111111111', 'steam:76561198000000001']);
  assert.deepEqual(inventoryLookups, ['11111111-1111-4111-8111-111111111111', 'steam:76561198000000001']);
});

test('expands single-player Takaro UUID-only reads to Conan player aliases', async () => {
  const locationLookups: string[] = [];
  const inventoryLookups: string[] = [];
  const calls: string[] = [];
  const saveDb = {
    getPlayerLocation: (identifier: string) => {
      locationLookups.push(identifier);
      return identifier === 'steam:76561198000000001' ? { x: 1, y: 2, z: 3, dimension: 'ConanSandbox' } : null;
    },
    getPlayerInventory: (identifier: string) => {
      inventoryLookups.push(identifier);
      return identifier === 'steam:76561198000000001'
        ? [{ code: '52562', name: 'Cimmerian Steel Pauldron', amount: 3, quality: '1' }]
        : [];
    },
    listItems: () => [],
    listEntities: () => [],
    listPlayerLocations: () => [],
  };
  const adapter = new ConanAdapter(async (command) => {
    calls.push(command);
    if (command === 'listplayers') {
      return 'Idx | Char name | Player name |      User ID |       Platform ID | Platform Name\n  0 | TestExile | Tester#1000 | A-TESTUSER | 76561198000000001 |         STEAM';
    }
    return 'ok';
  }, undefined, saveDb as never);
  const args = { playerId: '11111111-1111-4111-8111-111111111111' };

  assert.deepEqual(await adapter.handleAction('getPlayerLocation', args), {
    x: 1,
    y: 2,
    z: 3,
    dimension: 'ConanSandbox',
  });
  assert.deepEqual(await adapter.handleAction('getPlayerInventory', args), [
    { code: '52562', name: 'Cimmerian Steel Pauldron', amount: 3, quality: '1' },
  ]);
  assert.deepEqual(calls, ['listplayers', 'listplayers']);
  assert.deepEqual(locationLookups, ['11111111-1111-4111-8111-111111111111', '76561198000000001', 'steam:76561198000000001']);
  assert.deepEqual(inventoryLookups, ['11111111-1111-4111-8111-111111111111', '76561198000000001', 'steam:76561198000000001']);
});

test('routes teleportPlayer through Conan online-player console TeleportPlayer command', async () => {
  const calls: string[] = [];
  const adapter = new ConanAdapter(async (command) => {
    calls.push(command);
    if (command === 'listplayers') {
      return 'Idx | Char name | Player name |      User ID |       Platform ID | Platform Name\n  0 | TestExile | Tester#1000 | A-TESTUSER | 76561198000000001 |         STEAM';
    }
    return `Successfully executed: ${command}`;
  });

  const result = await adapter.handleAction('teleportPlayer', {
    player: { platformId: 'steam:76561198000000001' },
    x: 1,
    y: 2.5,
    z: -3,
  });

  assert.deepEqual(calls, ['listplayers', 'con 0 TeleportPlayer 1 2.5 -3']);
  assert.deepEqual(result, { success: true, rawResult: 'Successfully executed: con 0 TeleportPlayer 1 2.5 -3' });
});

test('sendMessage fails clearly when Conan chat bridge is not connected', async () => {
  const calls: string[] = [];
  const adapter = new ConanAdapter(async (command) => {
    calls.push(command);
    return 'sent';
  });

  const result = await adapter.handleAction('sendMessage', JSON.stringify({ message: 'Restart in 5 minutes' }));

  assert.deepEqual(calls, []);
  assert.deepEqual(result, {
    success: false,
    error: 'Conan chat bridge is not connected; vanilla RCON broadcast is not used for Takaro chat messages',
  });
});

test('sendMessage routes through Conan chat bridge with recipient identifiers', async () => {
  const calls: string[] = [];
  const chatBridge = {
    isConnected: () => true,
    sendMessage: async (message: string, recipientIdentifier: string | null, senderNameOverride: string | null) => {
      calls.push(`${senderNameOverride ?? '*'}:${recipientIdentifier ?? '*'}:${message}`);
      return { success: true, sent: true };
    },
  };
  const adapter = new ConanAdapter(async () => 'unused', chatBridge);

  const result = await adapter.handleAction('sendMessage', {
    message: 'Restart in 5 minutes',
    opts: {
      senderNameOverride: 'Takaro',
      recipient: {
        steamId: '76561198000000001',
      },
    },
  });

  assert.deepEqual(calls, ['Takaro:76561198000000001:Restart in 5 minutes']);
  assert.deepEqual(result, { success: true, sent: true });
});

test('dispatches moderation commands with identifiers and reasons', async () => {
  const calls: string[] = [];
  const adapter = new ConanAdapter(async (command) => {
    calls.push(command);
    return 'ok';
  });

  await adapter.handleAction('kickPlayer', { gameId: '76561198000000001', reason: 'rule break' });
  await adapter.handleAction('banPlayer', { platformId: 'steam:76561198000000002', reason: 'spam' });
  await adapter.handleAction('unbanPlayer', { gameId: '76561198000000003' });

  assert.deepEqual(calls, [
    'kickplayer platformid 76561198000000001 "rule break"',
    'banplayer platformid 76561198000000002 "spam"',
    'unbanplayer platformid 76561198000000003',
  ]);
});

test('strips Takaro platform prefixes before sending Conan platform identifiers', async () => {
  const calls: string[] = [];
  const adapter = new ConanAdapter(async (command) => {
    calls.push(command);
    return 'ok';
  });

  await adapter.handleAction('banPlayer', { platformId: 'steam:76561198000000001', reason: 'contract test' });

  assert.deepEqual(calls, ['banplayer platformid 76561198000000001 "contract test"']);
});

test('uses nested Takaro player platform identifiers for moderation actions', async () => {
  const calls: string[] = [];
  const adapter = new ConanAdapter(async (command) => {
    calls.push(command);
    return 'ok';
  });

  await adapter.handleAction('kickPlayer', {
    player: {
      gameId: '76561198000000001',
      playerId: '11111111-1111-4111-8111-111111111111',
      player: {
        platformId: 'steam:76561198000000001',
      },
    },
    reason: 'nested player contract test',
  });

  assert.deepEqual(calls, ['kickplayer platformid 76561198000000001 "nested player contract test"']);
});

test('returns structured errors for unsupported command-style actions', async () => {
  const adapter = new ConanAdapter(fakeExecutor({}));

  for (const action of ['getMapTile'] as const) {
    const result = await adapter.handleAction(action, {});

    assert.equal((result as { success: boolean }).success, false);
    assert.match(
      (result as { error: string }).error,
      new RegExp(`Action ${action} is not supported by the Conan Exiles RCON sidecar: .+not available`, 'i'),
    );
  }
});

test('returns Takaro schema-compatible fallbacks for unsupported DTO-constrained actions', async () => {
  const adapter = new ConanAdapter(fakeExecutor({}));

  assert.deepEqual(await adapter.handleAction('listItems', {}), []);
  assert.deepEqual(await adapter.handleAction('listEntities', {}), []);
  assert.deepEqual(await adapter.handleAction('listLocations', {}), []);
  assert.deepEqual(await adapter.handleAction('getPlayerInventory', {}), []);
  assert.deepEqual(await adapter.handleAction('getPlayerLocation', {}), { x: 0, y: 0, z: 0 });
  assert.deepEqual(await adapter.handleAction('getMapInfo', {}), {
    enabled: false,
    mapBlockSize: 0,
    maxZoom: 0,
    mapSizeX: 0,
    mapSizeY: 0,
    mapSizeZ: 0,
  });
});

test('uses Conan save DB reader for player location and inventory actions', async () => {
  const saveDb = {
    getPlayerLocation: (identifier: string) => {
      assert.equal(identifier, '76561198000000001');
      return { x: 1, y: 2, z: 3, dimension: 'ConanSandbox' };
    },
    getPlayerInventory: (identifier: string) => {
      assert.equal(identifier, 'steam:76561198000000001');
      return [{ code: '1001', name: 'Conan item 1001', amount: 1, quality: '1' }];
    },
    listItems: () => [{ code: '1001', name: 'Conan item 1001' }],
    listEntities: () => [{ code: 'BasePlayerChar_C', name: 'BasePlayerChar' }],
    listPlayerLocations: () => [{ code: 'player:109', name: 'TestExile', x: 1, y: 2, z: 3, dimension: 'ConanSandbox' }],
  };
  const adapter = new ConanAdapter(fakeExecutor({}), undefined, saveDb as never);

  assert.deepEqual(await adapter.handleAction('getPlayerLocation', { player: { gameId: '76561198000000001' } }), {
    x: 1,
    y: 2,
    z: 3,
    dimension: 'ConanSandbox',
  });
  assert.deepEqual(await adapter.handleAction('getPlayerInventory', { player: { platformId: 'steam:76561198000000001' } }), [
    { code: '1001', name: 'Conan item 1001', amount: 1, quality: '1' },
  ]);
  assert.deepEqual(await adapter.handleAction('listItems', {}), [{ code: '1001', name: 'Conan item 1001' }]);
  assert.deepEqual(await adapter.handleAction('listEntities', {}), [{ code: 'BasePlayerChar_C', name: 'BasePlayerChar' }]);
  assert.deepEqual(await adapter.handleAction('listLocations', {}), [
    { code: 'player:109', name: 'TestExile', x: 1, y: 2, z: 3, dimension: 'ConanSandbox' },
  ]);
});

test('returns schema-compatible command output when executeConsoleCommand RCON fails', async () => {
  const adapter = new ConanAdapter(async () => {
    throw new Error('write EPIPE');
  });

  const result = await adapter.handleAction('executeConsoleCommand', { command: 'help' });

  assert.deepEqual(result, {
    success: false,
    rawResult: 'write EPIPE',
  });
});

test('returns unreachable DTO when reachability RCON check fails', async () => {
  const adapter = new ConanAdapter(async () => {
    throw new Error('connect ECONNREFUSED 127.0.0.1:25575');
  });

  const result = await adapter.handleAction('testReachability', {});

  assert.deepEqual(result, {
    connectable: false,
    reason: 'connect ECONNREFUSED 127.0.0.1:25575',
  });
});

test('returns empty player and ban arrays when RCON list commands fail', async () => {
  const adapter = new ConanAdapter(async () => {
    throw new Error('RCON connection closed before response');
  });

  assert.deepEqual(await adapter.handleAction('getPlayers', {}), []);
  assert.deepEqual(await adapter.handleAction('listBans', {}), []);
});

function fakeExecutor(responses: Record<string, string>): (command: string) => Promise<string> {
  return async (command: string) => responses[command] ?? '';
}

function sequenceExecutor(responses: Record<string, string[]>): (command: string) => Promise<string> {
  const positions = new Map<string, number>();
  return async (command: string) => {
    const sequence = responses[command] ?? [];
    const position = positions.get(command) ?? 0;
    positions.set(command, position + 1);
    return sequence[position] ?? sequence.at(-1) ?? '';
  };
}
