import { describe, expect, it } from 'vitest';
import { Bridge, type TakaroSink } from '../bridge.js';
import type { GameEventType } from '../takaro/protocol.js';
import { harness } from './helpers.js';

describe('events produced before the first identify', () => {
  it('are queued without a send attempt and delivered in order once Takaro identifies', async () => {
    const h = await harness();
    let identified = false;
    let attempts = 0;
    const wire: [GameEventType, unknown][] = [];
    const sink: TakaroSink = {
      send: () => identified,
      sendGameEvent: (type, data) => {
        attempts += 1;
        if (!identified) return false;
        wire.push([type, data]);
        return true;
      },
      identified: () => identified,
    };
    const bridge = new Bridge({ adapter: h.adapter, takaro: sink, drainRetryMs: 1 });

    for (let i = 0; i < 5; i += 1) expect(bridge.emit('log', { msg: `boot ${i}` })).toBe('queued');
    await new Promise((resolve) => setTimeout(resolve, 20));
    expect(attempts).toBe(0);

    identified = true;
    const result = await bridge.onTakaroUp();
    expect(result.flushed).toBe(5);
    expect(wire.map(([, data]) => (data as { msg: string }).msg)).toEqual(['boot 0', 'boot 1', 'boot 2', 'boot 3', 'boot 4']);

    expect(bridge.emit('log', { msg: 'live' })).toBe(true);
    expect(wire).toHaveLength(6);
  });
});
