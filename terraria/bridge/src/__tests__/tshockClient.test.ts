import assert from 'node:assert/strict';
import http from 'node:http';
import { after, before, test } from 'node:test';
import { TShockClient } from '../tshock/client.js';

let server: http.Server;
let baseUrl = '';
const seen: string[] = [];

before(async () => {
  server = http.createServer((req, res) => {
    const url = new URL(req.url || '/', 'http://127.0.0.1');
    seen.push(`${url.pathname}?${url.searchParams.toString()}`);
    res.setHeader('Content-Type', 'application/json');

    if (url.pathname === '/v2/token/create') {
      res.end(JSON.stringify({ status: '200', response: 'Successful login', token: 'created-token' }));
      return;
    }
    if (url.pathname === '/tokentest') {
      res.end(JSON.stringify({ status: '200', response: 'Token is valid' }));
      return;
    }
    if (url.pathname === '/v2/server/status') {
      res.end(JSON.stringify({
        status: '200',
        name: 'Terraria',
        port: 7777,
        playercount: 1,
        players: [
          { nickname: 'Guide', username: 'guide-user', ip: '127.0.0.1', group: 'default', active: true, state: 10, team: 1 },
        ],
      }));
      return;
    }
    if (url.pathname === '/v2/server/broadcast') {
      res.end(JSON.stringify({ status: '200', response: `Broadcasted ${url.searchParams.get('msg')}` }));
      return;
    }
    if (url.pathname === '/v2/server/rawcmd') {
      res.statusCode = 404;
      res.end(JSON.stringify({ status: '404', error: 'v2 rawcmd removed' }));
      return;
    }
    if (url.pathname === '/v3/server/rawcmd') {
      const cmd = url.searchParams.get('cmd') || '';
      // TShock answers HTTP 200 with status "200" even for in-game failures, and puts the
      // failure text in `response`. These are verbatim responses captured from a live server.
      const failures: Record<string, string> = {
        bad: 'Invalid command entered. Type /help for a list of valid commands.',
        '/give "19" "CodexTest" 7': 'Player does not have free slots!',
        '/give 19 NoSuchPlayer 1': 'Invalid player!',
        '/give notanitem CodexTest 1': 'Invalid item type!',
        '/give': 'Invalid syntax. Proper syntax: /give <item type/id> <player> [item amount] [prefix id/name]',
        '/user': 'Invalid user syntax. Try /user help.',
        '/kick NoSuchPlayer': 'Player not found. Unable to kick the player.',
        '/takaroinv Ghost': "No player found matching 'Ghost'.",
        '/takaroinv G': "Multiple players found matching 'G'.",
        '/heal NoSuchPlayer': 'Unable to find any players named "NoSuchPlayer"',
        '/takarotp "CodexTest" a 10': 'X and Y must be numeric world coordinates.',
        '/tp': 'You must use this command in-game.',
      };
      if (failures[cmd]) {
        res.end(JSON.stringify({ status: '200', response: [failures[cmd]] }));
        return;
      }
      if (cmd === '/time') {
        res.end(JSON.stringify({ status: '200', response: ['The current time is 10:44.'] }));
        return;
      }
      if (cmd === '/butcher') {
        res.end(JSON.stringify({ status: '200', response: [] }));
        return;
      }
      if (cmd === '/mystery') {
        res.end(JSON.stringify({ status: '200', response: ['Whatever this plugin decided to print.'] }));
        return;
      }
      res.end(JSON.stringify({ status: '200', response: `ran ${cmd}` }));
      return;
    }
    if (url.pathname === '/v2/bans/list') {
      res.end(JSON.stringify({ status: '200', bans: [{ ticket_number: 7, identifier: 'BadPlayer', ip: '10.0.0.1', reason: 'test' }] }));
      return;
    }
    if (url.pathname === '/bans/create') {
      res.end(JSON.stringify({ status: '200', response: 'ban created' }));
      return;
    }
    if (url.pathname === '/v2/bans/destroy') {
      res.end(JSON.stringify({ status: '200', response: 'ban removed' }));
      return;
    }
    if (url.pathname === '/v2/server/off') {
      res.end(JSON.stringify({ status: '200', response: 'server shutting down' }));
      return;
    }

    res.statusCode = 404;
    res.end(JSON.stringify({ status: '404', error: 'not found' }));
  });

  await new Promise<void>((resolve) => server.listen(0, '127.0.0.1', resolve));
  const address = server.address();
  if (!address || typeof address === 'string') throw new Error('server did not bind');
  baseUrl = `http://127.0.0.1:${address.port}`;
});

after(async () => {
  await new Promise<void>((resolve) => server.close(() => resolve()));
});

test('creates and tests tokens, reads status, players, and bans', async () => {
  const client = new TShockClient({ baseUrl, username: 'admin', password: 'password', timeoutMs: 1000 });

  assert.equal(await client.getToken(), 'created-token');
  assert.equal((await client.testToken()).success, true);
  assert.equal((await client.status()).playercount, 1);
  assert.equal((await client.players())[0]?.name, 'Guide');
  assert.equal((await client.listBans())[0]?.identifier, 'BadPlayer');
});

test('executes raw commands, broadcast, ban, unban, and guarded shutdown with token', async () => {
  const client = new TShockClient({ baseUrl, token: 'static-token', timeoutMs: 1000 });

  assert.deepEqual(await client.rawCommand('/help'), { success: true, rawResult: 'ran /help' });
  assert.deepEqual(await client.rawCommand('bad'), { success: false, rawResult: 'Invalid command entered. Type /help for a list of valid commands.' });
  assert.equal((await client.broadcast('hello')).success, true);
  assert.equal((await client.createBan({ name: 'BadPlayer', reason: 'test' })).success, true);
  assert.equal((await client.destroyBan({ user: 'BadPlayer', type: 'user' })).success, true);
  assert.equal((await client.shutdown(false)).success, true);

  assert.ok(seen.some((entry) => entry.includes('token=static-token')));
  assert.ok(seen.some((entry) => entry.includes('identifier=BadPlayer')));
  assert.ok(seen.some((entry) => entry.includes('ticketNumber=7')));
});

test('reports in-game command failures as failures, not success', async () => {
  const client = new TShockClient({ baseUrl, token: 'static-token', timeoutMs: 1000 });

  // The exact regression: a Takaro shop giveItem to a player with a full inventory used to
  // report success, so the order was marked COMPLETED and the player was charged for nothing.
  assert.deepEqual(await client.rawCommand('/give "19" "CodexTest" 7'), {
    success: false,
    rawResult: 'Player does not have free slots!',
  });

  // The one failure the old hardcoded check caught, kept as a regression guard.
  assert.deepEqual(await client.rawCommand('bad'), {
    success: false,
    rawResult: 'Invalid command entered. Type /help for a list of valid commands.',
  });

  // Every other known TShock failure shape, captured verbatim from a live server.
  for (const command of [
    '/give 19 NoSuchPlayer 1',
    '/give notanitem CodexTest 1',
    '/give',
    '/user',
    '/kick NoSuchPlayer',
    '/takaroinv Ghost',
    '/takaroinv G',
    '/heal NoSuchPlayer',
    '/takarotp "CodexTest" a 10',
    '/tp',
  ]) {
    const result = await client.rawCommand(command);
    assert.equal(result.success, false, `expected failure for ${command}, got ${result.rawResult}`);
    assert.ok(result.rawResult.trim(), `expected a failure reason for ${command}`);
  }
});

test('reports successful and unrecognised command output as success', async () => {
  const client = new TShockClient({ baseUrl, token: 'static-token', timeoutMs: 1000 });

  assert.deepEqual(await client.rawCommand('/time'), { success: true, rawResult: 'The current time is 10:44.' });

  // Conservative by design: output we do not recognise must stay a success, so an
  // unfamiliar command or plugin never has its working actions turned into failures.
  assert.equal((await client.rawCommand('/mystery')).success, true);
  assert.equal((await client.rawCommand('/butcher')).success, true);
  assert.equal((await client.rawCommand('/help')).success, true);
});
