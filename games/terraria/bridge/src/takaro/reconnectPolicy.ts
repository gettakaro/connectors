/**
 * When to reconnect to Takaro, and how a dead link is noticed.
 *
 * Takaro's edge cuts established sockets now and then (a TCP reset mid-session). That cut is
 * harmless when the bridge is back within a second or two, and turns into an offline/online
 * flap in Takaro when it waits out a backoff first. So a drop after a confirmed, stable
 * session reconnects almost at once, and only a streak of failed attempts backs off.
 *
 * - `failures` is the streak of attempts since the last stable session. 0 means the socket
 *   that just closed had been identified and up for `stableMs`: retry after
 *   `fastRetryMinMs` to `fastRetryMaxMs`, random so a Takaro restart is not a thundering herd.
 * - From the first failure on: exponential from `baseMs`, capped at `maxMs`, each delay
 *   randomly between half and all of that step ("equal jitter").
 */
export interface ReconnectTiming {
  /** First step of the failure backoff. */
  baseMs: number;
  /** No wait is ever longer than this. */
  maxMs: number;
  fastRetryMinMs: number;
  fastRetryMaxMs: number;
  /** An identified socket that stays up this long resets the failure streak. */
  stableMs: number;
  /** An app-level ping is sent this often on an identified socket; Takaro answers each with a pong. */
  pingIntervalMs: number;
  /** No inbound frame for this long means the link is dead even though the socket says open. */
  inboundTimeoutMs: number;
  /** The WebSocket upgrade (TCP + TLS + HTTP) must finish within this. */
  handshakeTimeoutMs: number;
  /** A number in [0, 1); injectable so tests are deterministic. */
  random: () => number;
}

export const DEFAULT_RECONNECT_TIMING: ReconnectTiming = {
  baseMs: 2_000,
  maxMs: 60_000,
  // With a ~0.5 s TLS handshake to Takaro's edge, identify is accepted within 2 s of the drop.
  fastRetryMinMs: 500,
  fastRetryMaxMs: 1_250,
  stableMs: 10_000,
  pingIntervalMs: 15_000,
  // Three missed pongs.
  inboundTimeoutMs: 45_000,
  handshakeTimeoutMs: 15_000,
  random: Math.random,
};

/** The wait before the next connect attempt, given the failure streak so far. */
export function reconnectDelay(failures: number, timing: Pick<ReconnectTiming, 'baseMs' | 'maxMs' | 'fastRetryMinMs' | 'fastRetryMaxMs' | 'random'>): number {
  const r = Math.min(Math.max(timing.random(), 0), 1);
  if (failures <= 0) {
    const fast = timing.fastRetryMinMs + r * (timing.fastRetryMaxMs - timing.fastRetryMinMs);
    return Math.round(Math.min(fast, timing.maxMs));
  }
  // 2 ** 30 is far past any cap; the exponent is clamped so a long outage cannot overflow.
  const step = Math.min(timing.maxMs, timing.baseMs * 2 ** Math.min(failures - 1, 30));
  return Math.round(step / 2 + r * (step / 2));
}
