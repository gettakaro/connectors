#if defined _takaro_tf2_reconnect_included
  #endinput
#endif
#define _takaro_tf2_reconnect_included

// Reconnect policy for the Takaro WebSocket. The plugin owns reconnects; the extension's own
// automatic reconnection is off because it has no jitter and resets its backoff on any handshake.
//
// - A connection that was identified and stayed up for CONNECTION_STABLE_SECONDS is retried
//   RECONNECT_FAST_MIN_MS plus up to RECONNECT_FAST_JITTER_MS after it drops (0.25-1.0 s), so with
//   the TLS handshake and identify it is identified again within 2 s.
// - Only consecutive failures back off: base, 2x base, 4x base ... each plus up to 100 % jitter,
//   capped at the configured maximum (60 s by default); at the cap the wait is 75-100 % of it.
// - The streak resets once a connection is identified and stable again.

#define RECONNECT_FAST_MIN_MS 250
#define RECONNECT_FAST_JITTER_MS 750
#define RECONNECT_MIN_BASE_MS 100
#define RECONNECT_MAX_CAP_MS 3600000

// A connection counts as stable after it has been identified for this long.
#define CONNECTION_STABLE_SECONDS 10.0
// The plugin pings Takaro this often while identified; every ping is answered.
#define HEARTBEAT_INTERVAL_SECONDS 10.0
// No inbound frame (message or pong) for this long means the link is dead: three missed heartbeats.
#define INBOUND_TIMEOUT_SECONDS 30.0
// A socket must open within this time and be identified within IDENTIFY_DEADLINE_SECONDS.
#define CONNECT_DEADLINE_SECONDS 15.0
#define IDENTIFY_DEADLINE_SECONDS 20.0

// The failure streak after a connection ends: 0 when it was identified and stable, else one more.
stock int NextFailureStreak(int failureStreak, bool lastConnectionStable)
{
  if (lastConnectionStable) {
    return 0;
  }
  return failureStreak < 0 ? 1 : failureStreak + 1;
}

// Wait before the next connect attempt. jitter is 0..1000 (per mille), drawn by the caller.
stock int ReconnectDelayMs(int failureStreak, int baseMs, int capMs, int jitter)
{
  if (jitter < 0) {
    jitter = 0;
  } else if (jitter > 1000) {
    jitter = 1000;
  }

  if (failureStreak <= 0) {
    return RECONNECT_FAST_MIN_MS + RECONNECT_FAST_JITTER_MS * jitter / 1000;
  }

  if (baseMs < RECONNECT_MIN_BASE_MS) {
    baseMs = RECONNECT_MIN_BASE_MS;
  }
  if (capMs > RECONNECT_MAX_CAP_MS) {
    capMs = RECONNECT_MAX_CAP_MS;
  } else if (capMs < RECONNECT_MIN_BASE_MS) {
    capMs = RECONNECT_MIN_BASE_MS;
  }
  if (baseMs > capMs) {
    baseMs = capMs; // the configured maximum always wins
  }

  int step = baseMs;
  for (int i = 1; i < failureStreak && step < capMs; i++) {
    step *= 2;
  }

  float fraction = float(jitter) / 1000.0;
  if (step >= capMs) {
    return RoundToFloor(float(capMs) * (0.75 + 0.25 * fraction));
  }
  int delay = RoundToFloor(float(step) * (1.0 + fraction));
  return delay > capMs ? capMs : delay;
}
