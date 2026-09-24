import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { afterEach, describe, expect, it, vi } from 'vitest';
import { BanManager } from '../native/banManager.js';
import { FileBanStore, MemoryBanStore, type BanStore } from '../native/banStore.js';
import type { NativeClient } from '../native/client.js';

const id = '76561198000000000';
const other = '76561198000000001';
const day = Date.parse('2026-09-24T00:00:00.000Z');
const tempDirs: string[] = [];

afterEach(() => {
  for (const dir of tempDirs.splice(0)) fs.rmSync(dir, { recursive: true, force: true });
});

function nativeFixture() {
  const members = new Set<string>();
  const actions: { id: string; action: string }[] = [];
  const client = {
    bans: vi.fn(async () => [...members]),
    moderate: vi.fn(async (playerId: string, action: 'ban' | 'unban') => {
      actions.push({ id: playerId, action });
      if (action === 'ban') members.add(playerId);
      else members.delete(playerId);
      return { success: true };
    }),
  } as unknown as NativeClient;
  return { client, members, actions };
}

describe('durable native ban metadata', () => {
  it('preserves reason and timed expiry in fresh listBans, with the core permanent sentinel normalized', async () => {
    const native = nativeFixture();
    const manager = new BanManager(native.client, new MemoryBanStore(), () => day);
    await manager.ban(id, 'rule violation', '3021-01-01T00:00:00.000Z');
    expect(await manager.list()).toEqual([{ id, reason: 'rule violation', expiresAt: null }]);
    await manager.ban(id, 'repeat', '2026-09-25T02:00:00+02:00');
    expect(await manager.list()).toEqual([{ id, reason: 'repeat', expiresAt: '2026-09-25T00:00:00.000Z' }]);
    expect(native.actions).toEqual([{ id, action: 'ban' }, { id, action: 'ban' }]);
  });

  it('restarts from the same atomic journal and unbans an expired native member', async () => {
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'ark-ban-store-'));
    tempDirs.push(dir);
    const file = path.join(dir, 'sidecar', 'ban-metadata.json');
    const native = nativeFixture();
    let now = day;
    const first = new BanManager(native.client, new FileBanStore(file), () => now);
    await first.ban(id, 'temporary', '2026-09-24T00:00:02Z');
    expect(fs.existsSync(file)).toBe(true);
    now += 3000;
    const restarted = new BanManager(native.client, new FileBanStore(file), () => now);
    await restarted.reconcile();
    expect(native.actions).toEqual([{ id, action: 'ban' }, { id, action: 'unban' }]);
    expect(native.members.has(id)).toBe(false);
    expect(await restarted.list()).toEqual([]);
    expect(new FileBanStore(file).load()).toEqual({});
  });

  it('rejects stale expired global-sync replays without touching a newer permanent ban', async () => {
    const native = nativeFixture();
    const manager = new BanManager(native.client, new MemoryBanStore(), () => day);
    await manager.ban(id, 'new permanent', null);
    await expect(manager.ban(id, 'old timed', '2026-09-23T23:00:00Z')).rejects.toThrow('future');
    expect(native.actions).toEqual([{ id, action: 'ban' }]);
    expect(await manager.list()).toEqual([{ id, reason: 'new permanent', expiresAt: null }]);
  });

  it('keeps uncertain timed intent after a native timeout, then reconciles a late effect', async () => {
    const native = nativeFixture();
    vi.mocked(native.client.moderate).mockRejectedValueOnce(new Error('native timeout'));
    const store = new MemoryBanStore();
    let now = day;
    const manager = new BanManager(native.client, store, () => now);
    await expect(manager.ban(id, 'late', '2026-09-24T00:00:02Z')).rejects.toThrow('timeout');
    await manager.reconcile();
    expect(store.load()[id]?.state).toBe('pending');
    native.members.add(id); // Game thread finished after the HTTP timeout.
    now += 3000;
    await manager.reconcile();
    expect(native.members.has(id)).toBe(false);
    expect(store.load()).toEqual({});
  });

  it('never attaches stale metadata to a subsequently observed external ban', async () => {
    const native = nativeFixture();
    const manager = new BanManager(native.client, new MemoryBanStore(), () => day);
    await manager.ban(id, 'managed', null);
    native.members.delete(id);
    expect(await manager.list()).toEqual([]);
    native.members.add(id);
    await expect(manager.list()).rejects.toThrow('requires migration');
    expect(manager.ready()).toBe(false);
    expect(native.actions).toEqual([{ id, action: 'ban' }]);
  });

  it('reports ban reconciliation unready when a preexisting native ban has no journal', async () => {
    const native = nativeFixture();
    native.members.add(other);
    const manager = new BanManager(native.client, new MemoryBanStore(), () => day);
    await expect(manager.reconcile()).rejects.toThrow('requires migration');
    expect(manager.ready()).toBe(false);
  });

  it('fails before native mutation when storage is unavailable or corrupt', async () => {
    const native = nativeFixture();
    const unavailable: BanStore = { load: () => ({}), save: () => { throw new Error('disk full'); } };
    const manager = new BanManager(native.client, unavailable, () => day);
    await expect(manager.ban(id, 'reason', null)).rejects.toThrow('disk full');
    expect(native.actions).toEqual([]);
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'ark-ban-corrupt-'));
    tempDirs.push(dir);
    const file = path.join(dir, 'ban-metadata.json');
    fs.writeFileSync(file, '{bad');
    expect(() => new BanManager(native.client, new FileBanStore(file))).toThrow('Cannot load ban metadata');
  });

  it('drops a queued moderation request once its deadline passes, without late mutation', async () => {
    const native = nativeFixture();
    let release!: () => void;
    const wait = new Promise<void>((resolve) => { release = resolve; });
    vi.mocked(native.client.moderate).mockImplementationOnce(async () => {
      await wait;
      native.members.add(id);
      return { success: true };
    });
    const manager = new BanManager(native.client, new MemoryBanStore(), () => day, 15);
    const first = manager.ban(id, 'first', null);
    const second = manager.ban(other, 'second', null);
    await new Promise((resolve) => setTimeout(resolve, 25));
    release();
    await first;
    await expect(second).rejects.toThrow('expired in queue');
    expect(vi.mocked(native.client.moderate)).toHaveBeenCalledTimes(1);
  });
});
