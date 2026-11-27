import os from 'node:os';

// Poll watchdog for the Docker sidecar (network_mode: service:<game>). The sidecar must exit
// when the game container restarted into a new network namespace, because it keeps listening in
// the old one and the mod can never reach it again. It must NOT exit when the mod is merely
// silent: a game hang, a host under memory pressure, or the bridge's own event loop stalling
// all look like "no poll", and exiting then only adds a restart that can itself fail.
//
// A stale namespace has a precise signature: Docker deletes the game container's veth, so only
// loopback is left. The watchdog exits once only loopback has been left for the threshold, even
// if the mod never polled this bridge (the game container can restart while the server is still
// booting). Mod silence on a live namespace is only reported. Time the bridge spent stalled (a
// tick that arrived far later than scheduled) does not count as mod silence.

export type WatchdogVerdict = 'ok' | 'silent' | 'exit';

export interface WatchdogOptions {
  thresholdMs: number;
  tickMs: number;
  /** True while the network namespace still has an interface other than loopback. */
  namespaceAlive?: () => boolean;
}

export function namespaceHasExternalInterface(): boolean {
  const ifaces = os.networkInterfaces();
  return Object.values(ifaces).some((addrs) => (addrs ?? []).some((a) => !a.internal));
}

export class PollWatchdog {
  private lastTickMs: number | null = null;
  private stallEndedMs: number | null = null;
  private deadSinceMs: number | null = null;
  private readonly namespaceAlive: () => boolean;

  constructor(private readonly opts: WatchdogOptions) {
    this.namespaceAlive = opts.namespaceAlive ?? namespaceHasExternalInterface;
  }

  /** Called every tickMs; lastPollMs is the time of the mod's last poll (null = never polled). */
  tick(nowMs: number, lastPollMs: number | null): { verdict: WatchdogVerdict; silentMs: number; stalledMs: number } {
    let stalledMs = 0;
    if (this.lastTickMs !== null) {
      const gap = nowMs - this.lastTickMs;
      if (gap > this.opts.tickMs * 3) {
        stalledMs = gap - this.opts.tickMs;
        this.stallEndedMs = nowMs;
      }
    }
    this.lastTickMs = nowMs;

    if (!this.namespaceAlive()) {
      if (this.deadSinceMs === null) this.deadSinceMs = nowMs;
      const deadMs = nowMs - this.deadSinceMs;
      const silentMs = lastPollMs === null ? deadMs : nowMs - lastPollMs;
      if (deadMs >= this.opts.thresholdMs) return { verdict: 'exit', silentMs, stalledMs };
      return { verdict: 'ok', silentMs, stalledMs };
    }
    this.deadSinceMs = null;
    if (lastPollMs === null) return { verdict: 'ok', silentMs: 0, stalledMs };

    const since = Math.max(lastPollMs, this.stallEndedMs ?? lastPollMs);
    const silentMs = nowMs - since;
    return { verdict: silentMs > this.opts.thresholdMs ? 'silent' : 'ok', silentMs, stalledMs };
  }
}
