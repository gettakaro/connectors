import { describe, expect, it, vi } from 'vitest';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { WebSocketServer, type WebSocket } from 'ws';
import { ArkAdapter } from '../adapter.js';
import { EventPump } from '../eventPump.js';
import { NativeClient, type NativeEvents } from '../native/client.js';
import { FileCursorStore, MemoryCursorStore } from '../native/cursorStore.js';
import { handleTakaroRequest } from '../requestHandler.js';
import { TakaroWsClient } from '../takaro/client.js';
import { normalizeArgs, type WsMessage } from '../takaro/protocol.js';

const steam = '76561198000000000';

describe('Takaro Generic failure contract', () => {
  it('withholds failed replies so a void action times out while a concurrent success resolves', async () => {
    const warn = vi.spyOn(console, 'warn').mockImplementation(() => undefined);
    const sent: WsMessage[] = [];
    const pending = new Map<string, { resolve: (value: unknown) => void; timer: NodeJS.Timeout }>();
    const adapter = { handle: async (action: string) => {
      if (action === 'kickPlayer') throw new Error('private native failure detail');
      return {};
    } } as unknown as ArkAdapter;
    // Takaro's Generic connector resolves a matched void-action frame from
    // payload alone, even when its type is "error" or it has a top-level error.
    const send = (reply: WsMessage): boolean => {
      sent.push(reply);
      const request = pending.get(reply.requestId ?? '');
      if (request) {
        clearTimeout(request.timer);
        pending.delete(reply.requestId ?? '');
        request.resolve(reply.payload);
      }
      return true;
    };
    const ask = (requestId: string, action: string): Promise<unknown> => new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        pending.delete(requestId);
        reject(new Error(`Request timed out: ${action}`));
      }, 30);
      pending.set(requestId, { resolve, timer });
      void handleTakaroRequest({ type: 'request', requestId, payload: { action, args: {} } }, adapter, send);
    });
    const failed = ask('failed', 'kickPlayer');
    const failedExpectation = expect(failed).rejects.toThrow('Request timed out: kickPlayer');
    const succeeded = ask('succeeded', 'sendMessage');
    await expect(succeeded).resolves.toEqual({});
    await failedExpectation;
    expect(sent).toEqual([{ type: 'response', requestId: 'succeeded', payload: {} }]);
    expect(pending.size).toBe(0);
    expect(String(warn.mock.calls[0]?.[0])).toContain('"category":"action-failed"');
    expect(String(warn.mock.calls[0]?.[0])).not.toContain('private native failure detail');
    const oldAccepted = new Promise<unknown>((resolve, reject) => {
      const timer = setTimeout(() => reject(new Error('legacy reply was not accepted')), 30);
      pending.set('old-error', { resolve, timer });
      send({ type: 'response', requestId: 'old-error', error: 'kick rejected' });
    });
    await expect(oldAccepted).resolves.toBeUndefined(); // Previous error frame falsely acknowledged a void action.
    warn.mockRestore();
  });

  it('withholds a malformed request instead of acknowledging an unknown action', async () => {
    const warn = vi.spyOn(console, 'warn').mockImplementation(() => undefined);
    const send = vi.fn(() => true);
    const adapter = { handle: vi.fn() } as unknown as ArkAdapter;
    await handleTakaroRequest({ type: 'request', requestId: 'malformed', payload: { args: {} } }, adapter, send);
    expect(send).not.toHaveBeenCalled();
    expect(adapter.handle).not.toHaveBeenCalled();
    expect(String(warn.mock.calls[0]?.[0])).toContain('"category":"invalid-request"');
    warn.mockRestore();
  });

  it('rejects malformed shutdown args before calling the adapter while retaining documented empty forms', async () => {
    for (const value of [null, '', [], {}, '{}', '[]']) expect(normalizeArgs(value)).toEqual({});
    expect(normalizeArgs('{"gameId":"test"}')).toEqual({ gameId: 'test' });
    for (const value of ['{bad', '42', 42, true, [1]]) expect(() => normalizeArgs(value)).toThrow();
    const warn = vi.spyOn(console, 'warn').mockImplementation(() => undefined);
    const send = vi.fn(() => true);
    const adapter = { handle: vi.fn() } as unknown as ArkAdapter;
    await handleTakaroRequest({ type: 'request', requestId: 'shutdown-bad',
      payload: { action: 'shutdown', args: '{bad' } }, adapter, send);
    expect(adapter.handle).not.toHaveBeenCalled();
    expect(send).not.toHaveBeenCalled();
    expect(String(warn.mock.calls[0]?.[0])).toContain('"category":"invalid-request"');
    warn.mockRestore();
  });
});

describe('native transport and narrow actions', () => {
  it('traces health, roster, player and broadcast requests by request ID without logging message text', async () => {
    const info = vi.spyOn(console, 'info').mockImplementation(() => undefined);
    const fetchImpl = vi.fn(async (input: string | URL | Request) => {
      const route = new URL(String(input)).pathname;
      const payload = route === '/health' ? { status: 'ok', bootId: 'test-boot' } :
        route === '/players' ? [{ steamId: steam, name: 'Survivor' }] :
        route === `/players/${steam}` ? { steamId: steam, name: 'Survivor' } : { success: true };
      return new Response(JSON.stringify(payload), { status: 200 });
    }) as typeof fetch;
    const adapter = new ArkAdapter(new NativeClient('http://127.0.0.1:18891', 'secret', 1000, fetchImpl));
    try {
      expect(await adapter.handle('testReachability', {}, 'health-9942')).toMatchObject({ connectable: true });
      expect(await adapter.handle('getPlayers', {}, 'roster-9942')).toHaveLength(1);
      expect(await adapter.handle('getPlayer', { gameId: steam }, 'player-9942')).toMatchObject({ gameId: steam });
      expect(await adapter.handle('sendMessage', { message: 'private fixture text' }, 'message-9942')).toEqual({});
      const traces = info.mock.calls.map(([entry]) => JSON.parse(String(entry).replace(/^\[Takaro ARK native\] /, '')));
      expect(traces.map(({ requestId, path, status }) => [requestId, path, status])).toEqual([
        ['health-9942', '/health', 200], ['roster-9942', '/players', 200],
        ['player-9942', `/players/${steam}`, 200], ['message-9942', '/message', 200],
      ]);
      expect(JSON.stringify(info.mock.calls)).not.toContain('private fixture text');
      expect(JSON.stringify(info.mock.calls)).not.toContain('secret');
    } finally {
      info.mockRestore();
    }
  });

  it('sends bearer auth and raw UTF-8 text, and accepts only native success acknowledgment', async () => {
    const calls: { input: string; init?: RequestInit }[] = [];
    const fetchImpl = vi.fn(async (input: string | URL | Request, init?: RequestInit) => {
      calls.push({ input: String(input), init });
      return new Response(JSON.stringify({ success: true }), { status: 200 });
    }) as typeof fetch;
    const adapter = new ArkAdapter(new NativeClient('http://127.0.0.1:18891', 'secret', 1000, fetchImpl));
    expect(await adapter.handle('sendMessage', { message: 'héllo ARK' })).toEqual({});
    expect(calls[0].input).toBe('http://127.0.0.1:18891/message');
    expect(calls[0].init?.headers).toEqual({ Authorization: 'Bearer secret', 'Content-Type': 'text/plain; charset=utf-8' });
    expect(calls[0].init?.body).toBe('héllo ARK');
    expect(await adapter.handle('sendMessage', { message: 'private', opts: { recipient: { gameId: steam } } }, 'target-1')).toEqual({});
    expect(calls[1].input).toBe(`http://127.0.0.1:18891/players/${steam}/message`);
    expect(calls[1].init?.body).toBe('private');
    expect(calls[1].init?.headers).toEqual({ Authorization: 'Bearer secret', 'Content-Type': 'text/plain; charset=utf-8' });
    expect(calls).toHaveLength(2);
    await expect(adapter.handle('kickPlayer', {})).rejects.toThrow('Steam64');
  });

  it('accepts only unambiguous typed Steam64 recipients and never broadcasts malformed targets', async () => {
    const calls: string[] = [];
    const fetchImpl = vi.fn(async (input: string | URL | Request) => {
      calls.push(String(input));
      return new Response('{"success":true}', { status: 200 });
    }) as typeof fetch;
    const adapter = new ArkAdapter(new NativeClient('http://127.0.0.1:18891', 'secret', 1000, fetchImpl));
    for (const args of [
      { message: 'private', opts: { recipient: { steamId: steam } } },
      { message: 'private', opts: { recipient: { platformId: `steam:${steam}` } } },
      { message: 'private', recipientGameId: steam },
    ]) expect(await adapter.handle('sendMessage', args)).toEqual({});
    expect(calls).toHaveLength(3);
    expect(calls.every((url) => url.endsWith(`/players/${steam}/message`))).toBe(true);
    for (const args of [
      { message: 'private', opts: { recipient: { gameId: 'bad-id' } } },
      { message: 'private', opts: { recipient: {} } },
      { message: 'private', opts: { recipient: { playerId: 'uuid' } } },
      { message: 'private', opts: { recipient: 'bad-id' } },
      { message: 'private', recipientGameId: steam, opts: { recipient: { gameId: '76561198000000001' } } },
    ]) await expect(adapter.handle('sendMessage', args)).rejects.toThrow('recipient');
    expect(calls).toHaveLength(3);
  });

  it('propagates an unavailable targeted recipient without retrying as a broadcast', async () => {
    const calls: string[] = [];
    const fetchImpl = vi.fn(async (input: string | URL | Request) => {
      calls.push(String(input));
      return new Response('{"success":false}', { status: 503 });
    }) as typeof fetch;
    const adapter = new ArkAdapter(new NativeClient('http://127.0.0.1:18891', 'secret', 1000, fetchImpl));
    await expect(adapter.handle('sendMessage', { message: 'private', opts: { recipient: { gameId: steam } } }))
      .rejects.toThrow('503');
    expect(calls).toEqual([`http://127.0.0.1:18891/players/${steam}/message`]);
  });

  it('does not report sendMessage success when native refuses the broadcast', async () => {
    const fetchImpl = vi.fn(async () => new Response('{"success":false}', { status: 200 })) as typeof fetch;
    const adapter = new ArkAdapter(new NativeClient('http://127.0.0.1:18891', 'secret', 1000, fetchImpl));
    await expect(adapter.handle('sendMessage', { message: 'never delivered' })).rejects.toThrow('not acknowledged');
  });

  it('sends one authenticated empty-body native shutdown and requires an explicit ack', async () => {
    const calls: { input: string; init?: RequestInit }[] = [];
    const fetchImpl = vi.fn(async (input: string | URL | Request, init?: RequestInit) => {
      calls.push({ input: String(input), init });
      return new Response(calls.length === 1 ? '{"success":true}' : '{"success":false}', { status: 200 });
    }) as typeof fetch;
    const adapter = new ArkAdapter(new NativeClient('http://127.0.0.1:18891', 'secret', 1000, fetchImpl));
    await expect(adapter.handle('shutdown', { delay: 1 })).rejects.toThrow('no options');
    expect(calls).toHaveLength(0);
    expect(await adapter.handle('shutdown', {}, 'shutdown-req')).toEqual({});
    expect(calls[0]).toEqual({ input: 'http://127.0.0.1:18891/shutdown', init: expect.objectContaining({
      method: 'POST', body: '', headers: { Authorization: 'Bearer secret', 'Content-Type': 'text/plain; charset=utf-8' },
    }) });
    await expect(adapter.handle('shutdown', {})).rejects.toThrow('not acknowledged');
    expect(calls).toHaveLength(2);
    const unavailable = vi.fn(async () => { throw new Error('connection closed before ack'); }) as typeof fetch;
    await expect(new ArkAdapter(new NativeClient('http://127.0.0.1:18891', 'secret', 1000, unavailable))
      .handle('shutdown', {})).rejects.toThrow('connection closed before ack');
    expect(unavailable).toHaveBeenCalledTimes(1); // never retry an ambiguous shutdown
  });

  it('passes through bounded native-handled console output, including empty success and HTTP 503 failures', async () => {
    const calls: { input: string; init?: RequestInit }[] = [];
    const fetchImpl = vi.fn(async (input: string | URL | Request, init?: RequestInit) => {
      calls.push({ input: String(input), init });
      const ok = calls.length === 1;
      return new Response(JSON.stringify(ok ?
        { success: true, rawResult: '', errorMessage: null } :
        { success: false, rawResult: 'unknown command', errorMessage: 'native console rejected or unhandled' }),
      { status: ok ? 200 : 503 });
    }) as typeof fetch;
    const adapter = new ArkAdapter(new NativeClient('http://127.0.0.1:18891', 'secret', 1000, fetchImpl));
    expect(await adapter.handle('executeConsoleCommand', { command: 'SaveWorld' }, 'console-1')).toEqual({
      success: true, rawResult: '', errorMessage: null,
    });
    expect(await adapter.handle('executeConsoleCommand', { command: 'unknown command' })).toEqual({
      success: false, rawResult: 'unknown command', errorMessage: 'native console rejected or unhandled',
    });
    expect(calls[0]).toEqual({ input: 'http://127.0.0.1:18891/console', init: expect.objectContaining({
      method: 'POST', headers: { Authorization: 'Bearer secret', 'Content-Type': 'text/plain; charset=utf-8' }, body: 'SaveWorld',
    }) });
    expect(calls[1]?.init?.body).toBe('unknown command');
  });

  it('rejects malformed or oversized console commands before native HTTP', async () => {
    const fetchImpl = vi.fn(async () => new Response('{"success":true,"rawResult":"","errorMessage":null}')) as typeof fetch;
    const adapter = new ArkAdapter(new NativeClient('http://127.0.0.1:18891', 'secret', 1000, fetchImpl));
    for (const command of [undefined, '', '   ', 'save\nworld', 'save\rworld', 'save\0world',
      'save\x7fworld', 'save\u0085world', 'save\u2028world', 'save\ud800world',
      'x'.repeat(1025), '😀'.repeat(1025)]) {
      await expect(adapter.handle('executeConsoleCommand', { command })).rejects.toThrow('executeConsoleCommand');
    }
    expect(fetchImpl).not.toHaveBeenCalled();
  });

  it('rejects malformed native console acknowledgments', async () => {
    const payloads = [
      { status: 200, body: { success: true, rawResult: '', errorMessage: 'wrong' } },
      { status: 503, body: { success: false, rawResult: '', errorMessage: null } },
      { status: 200, body: { success: 'true', rawResult: '', errorMessage: null } },
      { status: 200, body: { success: true, rawResult: 'x'.repeat(32769), errorMessage: null } },
      { status: 503, body: { success: false, rawResult: '', errorMessage: 'x'.repeat(1025) } },
      { status: 200, body: { success: false, rawResult: '', errorMessage: 'failed' } },
      { status: 503, body: { success: true, rawResult: '', errorMessage: null } },
    ];
    const fetchImpl = vi.fn(async () => {
      const next = payloads.shift()!;
      return new Response(JSON.stringify(next.body), { status: next.status });
    }) as typeof fetch;
    const adapter = new ArkAdapter(new NativeClient('http://127.0.0.1:18891', 'secret', 1000, fetchImpl));
    for (let index = 0; index < 7; index++) {
      await expect(adapter.handle('executeConsoleCommand', { command: 'ListPlayers' })).rejects.toThrow('Native console');
    }
    expect(fetchImpl).toHaveBeenCalledTimes(7);
  });

  it('returns null for an absent player and maps real Steam64 records', async () => {
    const fetchImpl = vi.fn(async (input: string | URL | Request) => new Response(
      String(input).endsWith(`/players/${steam}`) ? 'null' : JSON.stringify([{ steamId: steam, name: 'Survivor' }]),
      { status: 200 },
    )) as typeof fetch;
    const adapter = new ArkAdapter(new NativeClient('http://127.0.0.1:18891', 'secret', 1000, fetchImpl));
    expect(await adapter.handle('getPlayer', { player: { platformId: `steam:${steam}` } })).toBeNull();
    expect(await adapter.handle('getPlayers', {})).toEqual([{ gameId: steam, steamId: steam, platformId: `steam:${steam}`, name: 'Survivor' }]);
  });

  it('returns a finite native player location using authenticated Steam64 lookup', async () => {
    const fetchImpl = vi.fn(async () => new Response('{"x":12.5,"y":-4,"z":300}', { status: 200 })) as typeof fetch;
    const adapter = new ArkAdapter(new NativeClient('http://127.0.0.1:18891', 'secret', 1000, fetchImpl));
    expect(await adapter.handle('getPlayerLocation', { gameId: steam })).toEqual({ x: 12.5, y: -4, z: 300 });
    expect(fetchImpl).toHaveBeenCalledWith(
      `http://127.0.0.1:18891/players/${steam}/location`,
      expect.objectContaining({ method: 'GET', headers: { Authorization: 'Bearer secret' } }),
    );
  });

  it('rejects incomplete or non-finite native locations', async () => {
    for (const payload of ['{}', '{"x":0,"y":1}', '{"x":"0","y":1,"z":2}', 'null']) {
      const fetchImpl = vi.fn(async () => new Response(payload, { status: 200 })) as typeof fetch;
      const adapter = new ArkAdapter(new NativeClient('http://127.0.0.1:18891', 'secret', 1000, fetchImpl));
      await expect(adapter.handle('getPlayerLocation', { gameId: steam })).rejects.toThrow('unavailable or invalid');
    }
  });

  it('maps authenticated inventory and catalog entries without changing native item code or stack quantity', async () => {
    const log = vi.spyOn(console, 'info').mockImplementation(() => undefined);
    const calls: string[] = [];
    const fetchImpl = vi.fn(async (input: string | URL | Request, init?: RequestInit) => {
      expect(init?.headers).toEqual({ Authorization: 'Bearer secret' });
      calls.push(String(input));
      return new Response(JSON.stringify(String(input).includes('/items?') ?
        { items: [{ code: '/Game/Items/PrimalItem_Stone.PrimalItem_Stone_C', name: 'Stone', description: 'A rock' }],
          offset: 0, nextOffset: 1, total: 1, complete: true } :
        [{ code: '/Game/Items/PrimalItem_Stone.PrimalItem_Stone_C', name: 'Stone', amount: 17 }]), { status: 200 });
    }) as typeof fetch;
    const adapter = new ArkAdapter(new NativeClient('http://127.0.0.1:18891', 'secret', 1000, fetchImpl));
    expect(await adapter.handle('getPlayerInventory', { player: { steamId: steam } })).toEqual([
      { code: '/Game/Items/PrimalItem_Stone.PrimalItem_Stone_C', name: 'Stone', amount: 17 },
    ]);
    expect(await adapter.handle('listItems', { search: 'stone' }, 'catalog-req')).toEqual([
      { code: '/Game/Items/PrimalItem_Stone.PrimalItem_Stone_C', name: 'Stone', description: 'A rock' },
    ]);
    expect(calls).toEqual([
      `http://127.0.0.1:18891/players/${steam}/inventory`,
      'http://127.0.0.1:18891/items?offset=0&limit=128',
    ]);
    const trace = String(log.mock.calls.at(-1)?.[0]);
    expect(trace).toContain('"requestId":"catalog-req"');
    expect(trace).toContain('"path":"/items"');
    expect(trace).toContain('"itemCount":1');
    expect(trace).not.toContain('secret');
    log.mockRestore();
  });

  it('fails closed on unverified inventory quantities or catalog codes', async () => {
    const invalid = [
      { route: 'getPlayerInventory', payload: [{ code: 'Stone', name: 'Stone' }] },
      { route: 'getPlayerInventory', payload: [{ code: 'Stone', name: 'Stone', amount: 0 }] },
      { route: 'listItems', payload: { items: [{ name: 'Stone' }], offset: 0, nextOffset: 1, total: 1, complete: true } },
    ];
    for (const sample of invalid) {
      const fetchImpl = vi.fn(async () => new Response(JSON.stringify(sample.payload), { status: 200 })) as typeof fetch;
      const adapter = new ArkAdapter(new NativeClient('http://127.0.0.1:18891', 'secret', 1000, fetchImpl));
      await expect(adapter.handle(sample.route, { gameId: steam })).rejects.toThrow('verified');
    }
  });

  it('collects only a complete, contiguous native catalog and rejects partial or duplicate pages', async () => {
    const item = (suffix: string) => ({ code: `/Game/Items/${suffix}.${suffix}_C`, name: suffix });
    const pages = [
      { items: [item('Stone')], offset: 0, nextOffset: 1, total: 2, complete: false },
      { items: [item('Wood')], offset: 1, nextOffset: 2, total: 2, complete: true },
    ];
    const fetchImpl = vi.fn(async (input: string | URL | Request) => new Response(JSON.stringify(
      String(input).includes('offset=0') ? pages[0] : pages[1]), { status: 200 })) as typeof fetch;
    const adapter = new ArkAdapter(new NativeClient('http://127.0.0.1:18891', 'secret', 1000, fetchImpl));
    expect(await adapter.handle('listItems', {})).toEqual([item('Stone'), item('Wood')]);
    expect(fetchImpl).toHaveBeenCalledTimes(2);
    for (const bad of [
      { items: [item('Stone')], offset: 0, nextOffset: 1, total: 2, complete: true },
      { items: [item('Stone')], offset: 0, nextOffset: 0, total: 1, complete: false },
      { items: [item('Stone')], offset: 1, nextOffset: 1, total: 1, complete: true },
      { items: [], offset: 0, nextOffset: 1, total: 65537, complete: false },
    ]) {
      const native = new NativeClient('http://127.0.0.1:18891', 'secret', 1000,
        vi.fn(async () => new Response(JSON.stringify(bad), { status: 200 })) as typeof fetch);
      await expect(new ArkAdapter(native).handle('listItems', {})).rejects.toThrow('incomplete or invalid');
    }
    pages[1] = { ...pages[1], items: [item('Stone')] };
    await expect(adapter.handle('listItems', {})).rejects.toThrow('duplicate class paths');
  });

  it('maps only complete native DinoNameTag entity pages with unique real names', async () => {
    const pages = [
      { items: [{ code: 'Dodo', name: 'Dodo' }], offset: 0, nextOffset: 1, total: 2, complete: false },
      { items: [{ code: 'Raptor', name: 'Raptor' }], offset: 1, nextOffset: 2, total: 2, complete: true },
    ];
    const calls: string[] = [];
    const fetchImpl = vi.fn(async (input: string | URL | Request, init?: RequestInit) => {
      expect(init?.headers).toEqual({ Authorization: 'Bearer secret' });
      calls.push(String(input));
      return new Response(JSON.stringify(String(input).includes('offset=0') ? pages[0] : pages[1]), { status: 200 });
    }) as typeof fetch;
    const adapter = new ArkAdapter(new NativeClient('http://127.0.0.1:18891', 'secret', 1000, fetchImpl));
    expect(await adapter.handle('listEntities', { search: 'rap' }, 'entities-req')).toEqual([
      { code: 'Raptor', name: 'Raptor' },
    ]);
    expect(calls).toEqual([
      'http://127.0.0.1:18891/entities?offset=0&limit=128',
      'http://127.0.0.1:18891/entities?offset=1&limit=128',
    ]);
    pages[1] = { ...pages[1], items: [{ code: 'Dodo', name: 'Raptor' }] };
    await expect(adapter.handle('listEntities', {})).rejects.toThrow('duplicate codes');
  });

  it('rejects missing, partial, inconsistent, or unnamed entity catalog data', async () => {
    for (const bad of [
      { items: [{ code: 'Dodo', name: 'Dodo' }], offset: 0, nextOffset: 1, total: 2, complete: true },
      { items: [], offset: 0, nextOffset: 1, total: 1, complete: true },
      { items: [{ code: 'Dodo', name: 'Dodo' }], offset: 1, nextOffset: 2, total: 2, complete: true },
      { items: [{ code: 'Dodo', name: 'Dodo' }], offset: 0, nextOffset: 1, total: 65537, complete: false },
      { items: [{ code: 'Dodo', name: 'Dodo' }], offset: 0, nextOffset: 0, total: 1, complete: false },
    ]) {
      const native = new NativeClient('http://127.0.0.1:18891', 'secret', 1000,
        vi.fn(async () => new Response(JSON.stringify(bad), { status: 200 })) as typeof fetch);
      await expect(new ArkAdapter(native).handle('listEntities', {})).rejects.toThrow('incomplete or invalid');
    }
    const invalidName = { items: [{ code: 'Dodo', name: '' }], offset: 0, nextOffset: 1, total: 1, complete: true };
    const native = new NativeClient('http://127.0.0.1:18891', 'secret', 1000,
      vi.fn(async () => new Response(JSON.stringify(invalidName), { status: 200 })) as typeof fetch);
    await expect(new ArkAdapter(native).handle('listEntities', {})).rejects.toThrow('verified code and name');
    await expect(new ArkAdapter(native).handle('listEntities', { search: 42 })).rejects.toThrow('search must be text');
  });

  it('maps only native rectangular bounds for loaded spawn-point locations', async () => {
    const nativeRow = { code: '/Game/Maps/TheIsland:PersistentLevel.PlayerStart_1',
      name: 'South Zone 1', position: { x: 120, y: -40, z: 20 },
      sizeX: 100, sizeY: 100, sizeZ: 200 };
    const calls: string[] = [];
    const fetchImpl = vi.fn(async (input: string | URL | Request, init?: RequestInit) => {
      calls.push(String(input));
      expect(init?.headers).toEqual({ Authorization: 'Bearer secret' });
      return new Response(JSON.stringify([nativeRow]), { status: 200 });
    }) as typeof fetch;
    const adapter = new ArkAdapter(new NativeClient('http://127.0.0.1:18891', 'secret', 1000, fetchImpl));
    await expect(adapter.handle('listLocations', { search: 'South' })).rejects.toThrow('no options');
    expect(calls).toHaveLength(0);
    expect(await adapter.handle('listLocations', {}, 'locations-req')).toEqual([nativeRow]);
    expect(calls).toEqual(['http://127.0.0.1:18891/locations']);
  });

  it('fails closed on point-only, empty, duplicate, or nonfinite native locations', async () => {
    const row = { code: 'PlayerStart_1', name: 'South Zone', position: { x: 1, y: 2, z: 3 },
      sizeX: 100, sizeY: 100, sizeZ: 200 };
    for (const bad of [[], [{ ...row, sizeX: undefined }], [{ ...row, sizeY: 0 }],
      [{ ...row, position: { x: null, y: 2, z: 3 } }], [row, row]]) {
      const native = new NativeClient('http://127.0.0.1:18891', 'secret', 1000,
        vi.fn(async () => new Response(JSON.stringify(bad), { status: 200 })) as typeof fetch);
      await expect(new ArkAdapter(native).handle('listLocations', {})).rejects.toThrow();
    }
  });

  it('delivers a bounded kick reason, accepts a Takaro-managed permanent ban reason, and rejects invalid actions', async () => {
    const log = vi.spyOn(console, 'info').mockImplementation(() => undefined);
    const calls: { input: string; init?: RequestInit }[] = [];
    const fetchImpl = vi.fn(async (input: string | URL | Request, init?: RequestInit) => {
      calls.push({ input: String(input), init });
      return new Response(calls.length === 4 ? '{"success":false,"errorMessage":"effect unverified"}' : '{"success":true}',
        { status: calls.length === 4 ? 503 : 200 });
    }) as typeof fetch;
    const adapter = new ArkAdapter(new NativeClient('http://127.0.0.1:18891', 'secret', 1000, fetchImpl));
    for (const reason of [123, 'line\nbreak', 'line\u2028break', '\ud800', 'x'.repeat(481), '🙂'.repeat(300)]) {
      await expect(adapter.handle('kickPlayer', { gameId: steam, reason })).rejects.toThrow('reason');
    }
    await expect(adapter.handle('unbanPlayer', { gameId: steam, reason: 'griefing' })).rejects.toThrow('reason');
    await expect(adapter.handle('banPlayer', { gameId: steam, expiresAt: '2001-10-01T00:00:00Z' })).rejects.toThrow('future');
    await expect(adapter.handle('banPlayer', { gameId: 'not-steam' })).rejects.toThrow('Steam64');
    expect(calls).toHaveLength(0);
    const reason = 'ARK kick diagnostic — réglage';
    expect(await adapter.handle('kickPlayer', { gameId: steam, reason })).toEqual({});
    expect(await adapter.handle('banPlayer', { gameId: steam, reason: 'griefing' })).toEqual({});
    await expect(adapter.handle('unbanPlayer', { gameId: steam }, 'moderation-req')).rejects.toThrow('effect unverified');
    expect(calls.map((call) => call.input)).toEqual(['message', 'kick', 'ban', 'unban'].map((action) =>
      `http://127.0.0.1:18891/players/${steam}/${action}`));
    expect(calls[0].init).toEqual(expect.objectContaining({ method: 'POST', body: `Kick reason: ${reason}`,
      headers: { Authorization: 'Bearer secret', 'Content-Type': 'text/plain; charset=utf-8' } }));
    expect(calls[1].init).toEqual(expect.objectContaining({ method: 'POST', body: '' }));
    expect(calls[2].init).toEqual(expect.objectContaining({ method: 'POST', body: '' }));
    expect(calls[3].init).toEqual(expect.objectContaining({ method: 'POST', body: '' }));
    const trace = String(log.mock.calls.at(-1)?.[0]);
    expect(trace).toContain('"requestId":"moderation-req"');
    expect(trace).toContain(`"path":"/players/${steam}/unban"`);
    expect(trace).toContain('"status":503');
    expect(trace).not.toContain('secret');
    log.mockRestore();
  });

  it('never kicks when a targeted reason message is unacknowledged, and skips it for empty reason', async () => {
    const calls: string[] = [];
    let messageAck = false;
    const fetchImpl = vi.fn(async (input: string | URL | Request) => {
      const url = String(input);
      calls.push(url);
      return new Response(JSON.stringify({ success: !url.endsWith('/message') || messageAck }), { status: 200 });
    }) as typeof fetch;
    const adapter = new ArkAdapter(new NativeClient('http://127.0.0.1:18891', 'secret', 1000, fetchImpl));
    await expect(adapter.handle('kickPlayer', { gameId: steam, reason: 'diagnostic' }))
      .rejects.toThrow();
    expect(calls).toEqual([`http://127.0.0.1:18891/players/${steam}/message`]);
    messageAck = true;
    expect(await adapter.handle('kickPlayer', { gameId: steam, reason: '' })).toEqual({});
    expect(calls).toEqual([
      `http://127.0.0.1:18891/players/${steam}/message`,
      `http://127.0.0.1:18891/players/${steam}/kick`,
    ]);
  });

  it('withholds a failed kick reply after a delivered reason without retrying the native kick', async () => {
    const warn = vi.spyOn(console, 'warn').mockImplementation(() => undefined);
    const calls: { url: string; body: BodyInit | null | undefined }[] = [];
    const fetchImpl = vi.fn(async (input: string | URL | Request, init?: RequestInit) => {
      const url = String(input);
      calls.push({ url, body: init?.body });
      return new Response(url.endsWith('/kick') ? '{"success":false,"errorMessage":"effect unverified"}' :
        '{"success":true}', { status: url.endsWith('/kick') ? 503 : 200 });
    }) as typeof fetch;
    const adapter = new ArkAdapter(new NativeClient('http://127.0.0.1:18891', 'secret', 1000, fetchImpl));
    const sent: WsMessage[] = [];
    await handleTakaroRequest({ type: 'request', requestId: 'failed-kick', payload: { action: 'kickPlayer',
      args: JSON.stringify({ player: { gameId: steam }, reason: 'diagnostic' }) } }, adapter,
    (response) => { sent.push(response); return true; });
    expect(calls).toEqual([
      { url: `http://127.0.0.1:18891/players/${steam}/message`, body: 'Kick reason: diagnostic' },
      { url: `http://127.0.0.1:18891/players/${steam}/kick`, body: '' },
    ]);
    expect(sent).toEqual([]);
    expect(String(warn.mock.calls[0]?.[0])).toContain('"response":"withheld"');
    warn.mockRestore();
  });

  it('blocks unknown native bans from overwriting Takaro metadata and rejects malformed snapshots', async () => {
    const other = '76561198000000001';
    const calls: { url: string; init?: RequestInit }[] = [];
    let payload: unknown = [steam, other];
    let status = 200;
    const fetchImpl = vi.fn(async (input: string | URL | Request, init?: RequestInit) => {
      calls.push({ url: String(input), init });
      return new Response(JSON.stringify(payload), { status });
    }) as typeof fetch;
    const adapter = new ArkAdapter(new NativeClient('http://127.0.0.1:18891', 'secret', 1000, fetchImpl));
    await expect(adapter.handle('listBans', { search: 'x' })).rejects.toThrow('no options');
    expect(calls).toHaveLength(0);
    await expect(adapter.handle('listBans', {}, 'ban-list-req')).rejects.toThrow('requires migration');
    expect(calls[0]).toEqual({ url: 'http://127.0.0.1:18891/bans',
      init: expect.objectContaining({ method: 'GET', headers: { Authorization: 'Bearer secret' } }) });
    payload = [];
    expect(await adapter.handle('listBans', {})).toEqual([]); // native validated empty only
    for (const bad of [[steam, steam], ['steam:' + steam], ['bad'], [123], {}, null]) {
      payload = bad;
      await expect(adapter.handle('listBans', {})).rejects.toThrow(/invalid|duplicate/);
    }
    status = 501;
    payload = { error: 'unavailable' };
    await expect(adapter.handle('listBans', {})).rejects.toThrow('501');
  });

  it('maps exact native give-item and teleport requests and requires verified acknowledgments', async () => {
    const calls: { url: string; init?: RequestInit }[] = [];
    const fetchImpl = vi.fn(async (input: string | URL | Request, init?: RequestInit) => {
      calls.push({ url: String(input), init });
      return new Response('{"success":true}', { status: 200 });
    }) as typeof fetch;
    const adapter = new ArkAdapter(new NativeClient('http://127.0.0.1:18891', 'secret', 1000, fetchImpl));
    const code = '/Game/Items/PrimalItem_Stone.PrimalItem_Stone_C';
    expect(await adapter.handle('giveItem', { gameId: steam, item: code, amount: 3, quality: 2.5, blueprint: true })).toEqual({});
    expect(await adapter.handle('teleportPlayer', { gameId: steam, x: 1, y: -2, z: 3 })).toEqual({});
    expect(calls.map((call) => [call.url, call.init?.body])).toEqual([
      [`http://127.0.0.1:18891/players/${steam}/give-item`, JSON.stringify({ code, amount: 3, quality: 2.5, blueprint: true })],
      [`http://127.0.0.1:18891/players/${steam}/teleport`, JSON.stringify({ x: 1, y: -2, z: 3 })],
    ]);
    expect(calls[0].init?.headers).toEqual({ Authorization: 'Bearer secret', 'Content-Type': 'application/json' });
    expect(await adapter.handle('giveItem', { playerId: steam, name: code, amount: 2, quality: '2.5' })).toEqual({});
    expect(calls[2].init?.body).toBe(JSON.stringify({ code, amount: 2, quality: 2.5, blueprint: false }));
    for (const quality of ['bad', '', 'Infinity', '1e999', '-1']) {
      await expect(adapter.handle('giveItem', { playerId: steam, name: code, quality })).rejects.toThrow('quality');
    }
    await expect(adapter.handle('giveItem', { gameId: steam, item: 'Stone' })).rejects.toThrow('class path');
    await expect(adapter.handle('teleportPlayer', { gameId: steam, target: 'base' })).rejects.toThrow('unavailable');
    expect(calls).toHaveLength(3);
  });

  it('logs request ID, method, path, HTTP status, and inventory count without bearer or payload', async () => {
    const spy = vi.spyOn(console, 'info').mockImplementation(() => undefined);
    try {
      const fetchImpl = vi.fn(async () => new Response('[]', { status: 200 })) as typeof fetch;
      const adapter = new ArkAdapter(new NativeClient('http://127.0.0.1:18891', 'trace-secret', 1000, fetchImpl));
      expect(await adapter.handle('getPlayerInventory', { gameId: steam }, 'takaro-req-1')).toEqual([]);
      const line = String(spy.mock.calls.at(-1)?.[0]);
      expect(line).toContain('"requestId":"takaro-req-1"');
      expect(line).toContain('"method":"GET"');
      expect(line).toContain(`"path":"/players/${steam}/inventory"`);
      expect(line).toContain('"status":200');
      expect(line).toContain('"itemCount":0');
      expect(line).not.toContain('trace-secret');
      expect(line).toMatch(/"utc":"[^\"]+Z"/);
      expect(line).toMatch(/"monotonicNs":"\d+"/);
    } finally { spy.mockRestore(); }
  });

  it('reports native capability degradation without inventing roster data', async () => {
    const fetchImpl = vi.fn(async () => new Response(JSON.stringify({ status: 'ok', bootId: 'boot-1', capabilities: { roster: 'degraded' } }), { status: 200 })) as typeof fetch;
    const adapter = new ArkAdapter(new NativeClient('http://127.0.0.1:18891', 'secret', 1000, fetchImpl));
    expect(await adapter.handle('testReachability', {})).toEqual({ connectable: true, reason: 'Native capabilities: roster=degraded' });
  });
});

describe('event cursor', () => {
  it('forwards bounded native game log lines without player data or secrets', async () => {
    const nativeEvent: NativeEvents['events'][number] = {
      seq: 1, type: 'log', ts: '2026-09-24T03:00:00Z', data: { msg: 'Saving world...' },
    };
    const native = { events: async (): Promise<NativeEvents> => ({
      bootId: 'boot-log', seq: nativeEvent.seq, latestSeq: nativeEvent.seq,
      hasMore: false, truncated: false, events: [nativeEvent],
    }) } as unknown as NativeClient;
    const sent: { type: string; data: unknown }[] = [];
    const takaro = {
      identified: () => true,
      sendGameEvent: (type: string, data: unknown) => { sent.push({ type, data }); return true; },
      lastSendId: () => 1,
      onConfirmed: () => undefined,
    } as unknown as TakaroWsClient;
    const pump = new EventPump(native, takaro, new MemoryCursorStore(), 1000);
    await pump.pollOnce();
    expect(sent).toEqual([{ type: 'log', data: {
      msg: 'Saving world...', timestamp: '2026-09-24T03:00:00.000Z',
    } }]);
    for (const bad of ['Commandline: RCONPassword=secret', 'saved from 10.0.0.1', '', 'x'.repeat(513)]) {
      nativeEvent.seq += 1;
      nativeEvent.data.msg = bad;
      await pump.pollOnce();
      expect(pump.scanCursor()).toBe(1);
    }
    expect(sent).toHaveLength(1);
  });

  it('forwards only verified entity kills and uses an empty weapon string for an unknown weapon', async () => {
    const nativeEvent: NativeEvents['events'][number] = {
      seq: 1, type: 'entity-killed', ts: '2026-09-24T03:00:00Z',
      data: { player: { steamId: steam, name: 'Survivor' }, entity: { code: 'Dodo', name: 'Dodo' } },
    };
    const native = { events: async (): Promise<NativeEvents> => ({
      bootId: 'boot-1', seq: 1, latestSeq: 1, hasMore: false, truncated: false, events: [nativeEvent],
    }) } as unknown as NativeClient;
    const sent: { type: string; data: unknown }[] = [];
    const takaro = {
      identified: () => true,
      sendGameEvent: (type: string, data: unknown) => { sent.push({ type, data }); return true; },
      lastSendId: () => 1,
      onConfirmed: () => undefined,
    } as unknown as TakaroWsClient;
    const pump = new EventPump(native, takaro, new MemoryCursorStore(), 1000);
    await pump.pollOnce();
    expect(sent).toEqual([{ type: 'entity-killed', data: {
      player: { gameId: steam, steamId: steam, platformId: `steam:${steam}`, name: 'Survivor' },
      entity: 'Dodo', weapon: '', timestamp: '2026-09-24T03:00:00.000Z',
    } }]);
    nativeEvent.seq = 2;
    nativeEvent.data.entity = { code: '', name: 'Dodo' };
    await pump.pollOnce();
    expect(sent).toHaveLength(1);
    expect(pump.scanCursor()).toBe(1);
  });

  it('rejects bootless, fractional, and unsafe persisted cursors', () => {
    const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'ark-cursor-test-'));
    try {
      const file = path.join(directory, 'cursor.json');
      const store = new FileCursorStore(file);
      for (const value of [{ seq: 99 }, { bootId: '', seq: 99 },
        { bootId: 'boot-1', seq: 1.5 }, { bootId: 'boot-1', seq: Number.MAX_SAFE_INTEGER + 1 }]) {
        fs.writeFileSync(file, JSON.stringify(value));
        expect(store.load().seq).toBe(0);
      }
      fs.writeFileSync(file, JSON.stringify({ bootId: 'boot-1', seq: 7 }));
      expect(store.load()).toEqual({ bootId: 'boot-1', seq: 7 });
    } finally { fs.rmSync(directory, { recursive: true, force: true }); }
  });

  it('confirms only an exact outstanding ping payload', () => {
    const client = new TakaroWsClient('ws://127.0.0.1:1', { identityToken: 'test', registrationToken: '' });
    const internal = client as unknown as {
      outstandingPings: { id: number; upTo: number }[];
      notePong: (payload: string) => void;
    };
    internal.outstandingPings = [{ id: 1, upTo: 4 }, { id: 2, upTo: 6 }];
    const confirmed: number[] = [];
    client.onConfirmed((id) => confirmed.push(id));
    internal.notePong('unsolicited');
    internal.notePong('');
    expect(confirmed).toEqual([]);
    expect(internal.outstandingPings).toHaveLength(2);
    internal.notePong('2');
    expect(confirmed).toEqual([6]);
    expect(internal.outstandingPings).toEqual([]);
    internal.notePong('1');
    expect(confirmed).toEqual([6]);
    client.shutdown();
  });

  it('ignores identify frames delivered by a replaced WebSocket', async () => {
    const server = new WebSocketServer({ host: '127.0.0.1', port: 0 });
    await new Promise<void>((resolve) => server.once('listening', resolve));
    const address = server.address();
    if (!address || typeof address === 'string') throw new Error('missing test listener');
    const client = new TakaroWsClient(`ws://127.0.0.1:${address.port}`, { identityToken: 'test', registrationToken: '' },
      { pingIntervalMs: 0, baseReconnectMs: 100000 });
    try {
      const firstConnection = new Promise<WebSocket>((resolve) => server.once('connection', resolve));
      client.connect();
      const first = await firstConnection;
      const secondConnection = new Promise<WebSocket>((resolve) => server.once('connection', resolve));
      client.connect();
      const second = await secondConnection;
      first.send(JSON.stringify({ type: 'identifyResponse', payload: { gameServerId: 'stale' } }));
      await new Promise((resolve) => setTimeout(resolve, 20));
      expect(client.identified()).toBe(false);
      const identified = new Promise<void>((resolve) => client.once('identified', () => resolve()));
      second.send(JSON.stringify({ type: 'identifyResponse', payload: { gameServerId: 'current' } }));
      await identified;
      expect(client.getGameServerId()).toBe('current');
      first.terminate();
      second.terminate();
    } finally {
      client.shutdown();
      await new Promise<void>((resolve) => server.close(() => resolve()));
    }
  });

  it('discards a native poll started on an old socket epoch after reconnect', async () => {
    const event: NativeEvents['events'][number] = {
      seq: 1, type: 'chat-message', ts: '2026-09-23T10:00:00Z',
      data: { player: { steamId: steam, name: 'Survivor' }, msg: 'old', channel: 'global' },
    };
    let finish!: (value: NativeEvents) => void;
    let count = 0;
    const native = { events: async (): Promise<NativeEvents> => {
      count += 1;
      if (count === 1) return new Promise<NativeEvents>((resolve) => { finish = resolve; });
      return { bootId: 'new-boot', seq: 1, latestSeq: 1, hasMore: false, truncated: false, events: [event] };
    } } as unknown as NativeClient;
    let sendId = 0;
    const sent: unknown[] = [];
    const takaro = {
      identified: () => true,
      sendGameEvent: (_type: string, data: unknown) => { sent.push(data); sendId += 1; return true; },
      lastSendId: () => sendId,
      onConfirmed: () => undefined,
    } as unknown as TakaroWsClient;
    const store = new MemoryCursorStore();
    const pump = new EventPump(native, takaro, store, 1000);
    const pending = pump.pollOnce();
    pump.disconnected();
    finish({ bootId: 'old-boot', seq: 1, latestSeq: 1, hasMore: false, truncated: false, events: [event] });
    await pending;
    expect(sent).toEqual([]);
    expect(store.load()).toEqual({ seq: 0 });
    expect(pump.scanCursor()).toBe(0);
    await pump.pollOnce();
    expect(sent).toHaveLength(1);
    expect(store.load()).toEqual({ bootId: 'new-boot', seq: 0 });
  });
  it('replays an unconfirmed event after disconnect and persists only after a later pong', async () => {
    const event = { seq: 1, type: 'chat-message' as const, ts: '2026-09-23T10:00:00Z', data: { player: { steamId: steam, name: 'Survivor' }, msg: 'hi', channel: 'global' } };
    const seenSince: number[] = [];
    const native = { events: async (since: number): Promise<NativeEvents> => {
      seenSince.push(since);
      return { bootId: 'boot-1', seq: 1, latestSeq: 1, hasMore: false, truncated: false, events: since < 1 ? [event] : [] };
    } } as NativeClient;
    let confirm: (id: number) => void = () => undefined;
    let sendId = 0;
    const sent: unknown[] = [];
    const takaro = {
      identified: () => true,
      sendGameEvent: (_type: string, data: unknown) => { sent.push(data); sendId += 1; return true; },
      lastSendId: () => sendId,
      onConfirmed: (cb: (id: number) => void) => { confirm = cb; },
    } as TakaroWsClient;
    const store = new MemoryCursorStore();
    const pump = new EventPump(native, takaro, store, 1000);
    await pump.pollOnce();
    expect(pump.scanCursor()).toBe(1);
    expect(store.load()).toEqual({ bootId: 'boot-1', seq: 0 });
    pump.disconnected();
    await pump.pollOnce();
    expect(seenSince).toEqual([0, 0]);
    expect(sent).toHaveLength(2); // at-least-once replay; Takaro may have received the first send
    confirm(2);
    expect(store.load()).toEqual({ bootId: 'boot-1', seq: 1 });
  });

  it('resets an old boot cursor and does not skip a truncated empty response', async () => {
    const seenSince: number[] = [];
    const native = { events: async (since: number): Promise<NativeEvents> => {
      seenSince.push(since);
      return { bootId: 'boot-2', seq: 9, latestSeq: 9, hasMore: false, truncated: true, events: [] };
    } } as NativeClient;
    const takaro = { identified: () => true, onConfirmed: () => undefined } as unknown as TakaroWsClient;
    const store = new MemoryCursorStore({ bootId: 'boot-1', seq: 50 });
    const pump = new EventPump(native, takaro, store, 1000);
    await pump.pollOnce();
    await pump.pollOnce();
    expect(seenSince).toEqual([50, 0]);
    expect(store.load()).toEqual({ bootId: 'boot-2', seq: 0 });
  });

  it('forwards native login and logout as player presence events', async () => {
    const events: NativeEvents['events'] = [
      { seq: 1, type: 'player-connected', ts: '2026-09-23T10:00:00Z', data: { player: { steamId: steam, name: 'Survivor' } } },
      { seq: 2, type: 'player-disconnected', ts: '2026-09-23T10:01:00Z', data: { player: { steamId: steam, name: 'Survivor' } } },
    ];
    const native = { events: async (): Promise<NativeEvents> => ({ bootId: 'boot-1', seq: 2, latestSeq: 2, hasMore: false, truncated: false, events }) } as unknown as NativeClient;
    const forwarded: { type: string; data: unknown }[] = [];
    const takaro = {
      identified: () => true,
      sendGameEvent: (type: string, data: unknown) => { forwarded.push({ type, data }); return true; },
      lastSendId: () => forwarded.length,
      onConfirmed: () => undefined,
    } as unknown as TakaroWsClient;
    const pump = new EventPump(native, takaro, new MemoryCursorStore(), 1000);
    await pump.pollOnce();
    expect(forwarded.map((e) => e.type)).toEqual(['player-connected', 'player-disconnected']);
    expect(forwarded[0].data).toEqual({
      player: { gameId: steam, steamId: steam, platformId: `steam:${steam}`, name: 'Survivor' },
      timestamp: '2026-09-23T10:00:00.000Z',
    });
  });

  it('maps native player death with verified player attacker and no self attribution', async () => {
    const killer = '76561198000000001';
    const events: NativeEvents['events'] = [
      { seq: 1, type: 'player-death', ts: '2026-09-23T10:00:00Z', data: { player: { steamId: steam, name: 'Victim' }, killer: { steamId: killer, name: 'Killer' } } },
      { seq: 2, type: 'player-death', ts: '2026-09-23T10:01:00Z', data: { player: { steamId: steam, name: 'Victim' }, killer: { steamId: steam, name: 'Victim' } } },
    ];
    const native = { events: async (): Promise<NativeEvents> => ({ bootId: 'boot-1', seq: 2, latestSeq: 2, hasMore: false, truncated: false, events }) } as unknown as NativeClient;
    const forwarded: { type: string; data: Record<string, unknown> }[] = [];
    const takaro = {
      identified: () => true,
      sendGameEvent: (type: string, data: Record<string, unknown>) => { forwarded.push({ type, data }); return true; },
      lastSendId: () => forwarded.length,
      onConfirmed: () => undefined,
    } as unknown as TakaroWsClient;
    await new EventPump(native, takaro, new MemoryCursorStore(), 1000).pollOnce();
    expect(forwarded.map((entry) => entry.type)).toEqual(['player-death', 'player-death']);
    expect(forwarded[0].data.attacker).toEqual({ gameId: killer, steamId: killer, platformId: `steam:${killer}`, name: 'Killer' });
    expect(forwarded[1].data.attacker).toBeUndefined();
  });
});
