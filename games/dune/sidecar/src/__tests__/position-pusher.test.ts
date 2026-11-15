import { describe, expect, it, vi } from 'vitest';
import { PositionPusher, POSITION_INTERVAL_MS } from '../dune/positionPusher.js';
import { sanitizeGameEvent } from '../takaro/eventWhitelist.js';
import type { TakaroSink } from '../bridge.js';
import type { TakaroPlayer } from '../dune/types.js';
import { PluginJoin } from '../dune/pluginJoin.js';
import type { DunePlayerRow } from '../dune/types.js';

const position = { x: 1, y: 2, z: 3, dimension: 'survival_1' };

function harness() {
  let now = 10_000;
  let connected = true;
  let version = 1;
  let online: TakaroPlayer[] = [{ gameId: 'a', name: 'A' }, { gameId: 'b', name: 'B' }];
  const location = vi.fn(async () => position);
  const sendGameEvent = vi.fn(() => true);
  const takaro = { sendGameEvent, identified: () => connected, getProtocolVersion: () => version } as unknown as TakaroSink & { identified(): boolean; getProtocolVersion(): number };
  const pusher = new PositionPusher({ online: () => online, location, takaro, now: () => now });
  return { pusher, sendGameEvent, location, setNow: (n: number) => { now = n; }, setConnected: (c: boolean) => { connected = c; }, setVersion: (v: number) => { version = v; }, setOnline: (p: TakaroPlayer[]) => { online = p; } };
}

describe('Dune position pushes', () => {
  it('sends a complete unique live batch at most once per five seconds', async () => {
    const h = harness();
    h.setOnline([{ gameId: 'a', name: 'A' }, { gameId: 'a', name: 'A' }, { gameId: 'b', name: 'B' }]);
    expect(await h.pusher.tick()).toBe(true);
    expect(h.sendGameEvent).toHaveBeenCalledWith('player-positions', {
      timestamp: new Date(10_000).toISOString(),
      positions: [{ gameId: 'a', position }, { gameId: 'b', position }],
    });
    expect(h.location).toHaveBeenCalledTimes(2);
    h.setNow(14_999);
    expect(await h.pusher.tick()).toBe(false);
    h.setNow(15_000);
    expect(await h.pusher.tick()).toBe(true);
  });

  it('does not queue disconnected, legacy, incomplete, or oversized snapshots', async () => {
    const h = harness();
    h.setConnected(false);
    expect(await h.pusher.tick()).toBe(false);
    h.setConnected(true);
    h.setVersion(0);
    expect(await h.pusher.tick()).toBe(false);
    h.setVersion(1);
    h.location.mockResolvedValueOnce(null as never);
    expect(await h.pusher.tick()).toBe(false);
    h.setOnline(Array.from({ length: 1001 }, (_, i) => ({ gameId: String(i), name: String(i) })));
    expect(await h.pusher.tick()).toBe(false);
    expect(h.sendGameEvent).not.toHaveBeenCalled();
  });

  it('switches capability availability with the live plugin', async () => {
    let ready = false;
    let now = 10_000;
    const availability = vi.fn();
    const sendGameEvent = vi.fn(() => true);
    const takaro = { sendGameEvent, identified: () => true, getProtocolVersion: () => 1 } as unknown as TakaroSink & { identified(): boolean; getProtocolVersion(): number };
    const pusher = new PositionPusher({ online: () => [], location: async () => position, ready: async () => ready, availability, takaro, now: () => now });
    expect(await pusher.tick()).toBe(false);
    expect(availability).toHaveBeenLastCalledWith(false);
    ready = true;
    expect(await pusher.tick()).toBe(true);
    expect(availability).toHaveBeenLastCalledWith(true);
    now += POSITION_INTERVAL_MS;
    ready = false;
    expect(await pusher.tick()).toBe(false);
    expect(availability).toHaveBeenLastCalledWith(false);
    expect(sendGameEvent).toHaveBeenCalledTimes(1);
  });

  it('whitelists chat and batch positions including nested fields', () => {
    expect(sanitizeGameEvent('chat-message', { msg: 'hi', position, extra: 1 }).data).toEqual({ msg: 'hi', position });
    const batch = sanitizeGameEvent('player-positions', { positions: [{ gameId: 'a', position: { ...position, stale: true }, secret: 1 }] });
    expect(batch.data).toEqual({ positions: [{ gameId: 'a', position }] });
    expect(batch.removed).toEqual(['positions.0.position.stale', 'positions.0.secret']);
  });

  it('shares one plugin join refresh across a position batch', async () => {
    const players = vi.fn(async () => ({ players: [{ ref: 'ref-a', accountId: 1, characterName: 'A' }] }));
    const roster = vi.fn(async () => [{ accountId: 1, flsId: 'a', characterName: 'A' } as DunePlayerRow]);
    const join = new PluginJoin({ players, roster });
    const matches = await Promise.all(Array.from({ length: 20 }, () => join.refFor('a')));
    expect(matches.every((match) => match?.ref === 'ref-a')).toBe(true);
    expect(players).toHaveBeenCalledTimes(1);
    expect(roster).toHaveBeenCalledTimes(1);
  });
});
