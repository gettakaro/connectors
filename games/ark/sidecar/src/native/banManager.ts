import { logger } from '../logger.js';
import { type BanMetadata, type BanMetadataState, type BanStore } from './banStore.js';
import { NativeClient } from './client.js';

const PERMANENT_SENTINEL = '3021-01-01T00:00:00.000Z';
const ISO_DATE = /^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d{1,3})?(?:Z|[+-]\d{2}:\d{2})$/;

function checkedExpiry(raw: unknown, now: number): string | null {
  if (raw === undefined || raw === null || raw === PERMANENT_SENTINEL) return null;
  if (typeof raw !== 'string' || !ISO_DATE.test(raw)) {
    throw new Error('banPlayer expiresAt must be an ISO 8601 date with a timezone');
  }
  const until = Date.parse(raw);
  if (!Number.isFinite(until) || until <= now) throw new Error('banPlayer expiresAt must be in the future');
  return new Date(until).toISOString();
}

function checkedReason(raw: unknown): string {
  if (raw === undefined || raw === null) return '';
  if (typeof raw !== 'string' || [...raw].length > 10000 || Buffer.byteLength(raw, 'utf8') > 40000) {
    throw new Error('banPlayer reason must be text within 10000 characters');
  }
  return raw;
}

function checkedNativeBans(rows: unknown): Set<string> {
  if (!Array.isArray(rows) || rows.length > 4096) throw new Error('Native ban collection is unavailable or invalid');
  const ids = new Set<string>();
  for (const id of rows) {
    if (typeof id !== 'string' || !/^\d{17}$/.test(id) || ids.has(id)) {
      throw new Error('Native ban collection contains an invalid or duplicate Steam64');
    }
    ids.add(id);
  }
  return ids;
}

/** Serializes moderation with native snapshots and keeps expiry metadata on the persistent data mount. */
export class BanManager {
  private state: BanMetadataState;
  private queue: Promise<void> = Promise.resolve();
  private timer: NodeJS.Timeout | undefined;
  private running = false;
  private queued = 0;
  private reconciliationReady = false;

  constructor(private readonly native: NativeClient, private readonly store: BanStore,
              private readonly now: () => number = Date.now,
              private readonly requestQueueTimeoutMs = 8000) {
    this.state = store.load(); // Corrupt state fails startup before Takaro connects.
  }

  private serial<T>(job: () => Promise<T>, deadline?: number): Promise<T> {
    if (this.queued >= 32) return Promise.reject(new Error('Ban operation queue is full'));
    this.queued++;
    const run = (): Promise<T> => {
      if (deadline !== undefined && Date.now() >= deadline) {
        throw new Error('Ban operation expired in queue before native dispatch');
      }
      return job();
    };
    const result = this.queue.then(run, run);
    this.queue = result.then(() => { this.queued--; }, () => { this.queued--; });
    return result;
  }

  private set(id: string, value?: BanMetadata): void {
    const next = { ...this.state };
    if (value) next[id] = value;
    else delete next[id];
    this.store.save(next);
    this.state = next;
  }

  async ban(id: string, reason: unknown, expiresAt: unknown, requestId?: string): Promise<void> {
    const checked = checkedReason(reason);
    checkedExpiry(expiresAt, this.now());
    const deadline = Date.now() + this.requestQueueTimeoutMs;
    await this.serial(async () => {
      const metadata: BanMetadata = { reason: checked,
        expiresAt: checkedExpiry(expiresAt, this.now()), state: 'pending', pendingSince: this.now() };
      const previous = this.state[id];
      this.set(id, metadata); // Intent is durable before the native mutation can take effect.
      if (Date.now() >= deadline) {
        if (previous) this.set(id, previous);
        else this.set(id);
        throw new Error('Ban operation expired before native dispatch');
      }
      try {
        const ack = await this.native.moderate(id, 'ban', requestId);
        if (!ack || ack.success !== true) {
          throw new Error(typeof ack?.errorMessage === 'string' && ack.errorMessage.trim() ? ack.errorMessage :
            'Native ban effect was not verified');
        }
      } catch (error) {
        // A prior managed ban remains valid when a metadata update fails. For a
        // new ban, keep pending expiry intent in case a timed-out native call took effect.
        if (previous) this.set(id, previous);
        throw error;
      }
      this.set(id, { reason: metadata.reason, expiresAt: metadata.expiresAt, state: 'active' });
    }, deadline);
  }

  async unban(id: string, requestId?: string): Promise<void> {
    const deadline = Date.now() + this.requestQueueTimeoutMs;
    await this.serial(async () => {
      const ack = await this.native.moderate(id, 'unban', requestId);
      if (!ack || ack.success !== true) {
        throw new Error(typeof ack?.errorMessage === 'string' && ack.errorMessage.trim() ? ack.errorMessage :
          'Native unban effect was not verified');
      }
      this.set(id);
    }, deadline);
  }

  /** Native membership is authoritative; metadata supplies only Takaro-owned reason and expiry. */
  async list(requestId?: string): Promise<{ id: string; reason: string; expiresAt: string | null }[]> {
    return this.serial(async () => {
      const ids = await this.reconcileLocked(requestId);
      return [...ids].map((id) => ({ id, reason: this.state[id]?.reason ?? '',
        expiresAt: this.state[id]?.expiresAt ?? null }));
    });
  }

  async reconcile(): Promise<void> {
    await this.serial(async () => { await this.reconcileLocked(); });
  }

  private async reconcileLocked(requestId?: string): Promise<Set<string>> {
    this.reconciliationReady = false;
    let ids = checkedNativeBans(await this.native.bans(requestId));
    for (const [id, metadata] of Object.entries(this.state)) {
      if (!ids.has(id)) {
        // A timed-out native action can finish after an arbitrarily long game
        // thread stall. Keep pending intent until its effect or an explicit
        // successful unban/replacement is observed; storage is capped at 4096.
        if (metadata.state === 'active') this.set(id);
        continue;
      }
      if (metadata.expiresAt !== null && Date.parse(metadata.expiresAt) <= this.now()) {
        const ack = await this.native.moderate(id, 'unban');
        if (!ack || ack.success !== true) throw new Error('Expired native ban could not be removed');
        const after = checkedNativeBans(await this.native.bans());
        if (after.has(id)) throw new Error('Expired native ban remains after unban acknowledgment');
        ids = after;
        this.set(id);
      } else if (metadata.state === 'pending') {
        this.set(id, { reason: metadata.reason, expiresAt: metadata.expiresAt, state: 'active' });
      }
    }
    for (const id of ids) {
      if (!this.state[id]) {
        // Takaro's sync merges listBans reason/expiry into managed rows.
        // Unknown native IDs cannot safely be projected as blank/permanent.
        throw new Error('Native ban metadata is unknown; listBans requires migration');
      }
    }
    this.reconciliationReady = true;
    return ids;
  }

  ready(): boolean { return this.reconciliationReady; }

  start(intervalMs = 1000): void {
    if (this.running) return;
    this.running = true;
    const poll = async (): Promise<void> => {
      try {
        await this.reconcile();
      } catch (error) {
        logger.warn(JSON.stringify({ event: 'ban-reconciliation-failed', category: error instanceof Error ?
          error.name : 'unknown' }));
      } finally {
        if (this.running) {
          this.timer = setTimeout(() => { void poll(); }, intervalMs);
          this.timer.unref();
        }
      }
    };
    void poll();
  }

  stop(): void {
    this.running = false;
    if (this.timer) clearTimeout(this.timer);
    this.timer = undefined;
  }
}
