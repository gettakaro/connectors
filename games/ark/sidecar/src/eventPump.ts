import { logger } from './logger.js';
import { mapPlayer } from './adapter.js';
import type { NativeClient, NativeEvent } from './native/client.js';
import type { CursorStore } from './native/cursorStore.js';
import type { TakaroWsClient } from './takaro/client.js';
import type { GameEventType } from './takaro/protocol.js';

function mapEvent(event: NativeEvent): { type: GameEventType; data: Record<string, unknown> } {
  if (event.type === 'log') {
    const msg = event.data?.msg;
    if (typeof msg !== 'string' || !msg.trim() || msg.length > 512 ||
        /[\x00-\x1f\x7f]/.test(msg) ||
        /commandline|password|token|authorization|auth=|rcon|steam_|(?:\d{1,3}\.){3}\d{1,3}/i.test(msg)) {
      throw new Error('Native log event has no safe bounded message');
    }
    const data: Record<string, unknown> = { msg };
    const date = new Date(event.ts);
    if (Number.isFinite(date.getTime())) data.timestamp = date.toISOString();
    return { type: 'log', data };
  }
  if (!event.data?.player || !['chat-message', 'player-connected', 'player-disconnected', 'player-death', 'entity-killed'].includes(event.type)) {
    throw new Error('Native event has an unsupported type or no player');
  }
  const data: Record<string, unknown> = { player: mapPlayer(event.data.player) };
  if (event.type === 'chat-message') {
    if (typeof event.data.msg !== 'string') throw new Error('Native chat event has no message');
    data.msg = event.data.msg;
    data.channel = ['global', 'team', 'friends', 'whisper'].includes(event.data.channel ?? '')
      ? event.data.channel : 'global';
  }
  if (event.type === 'player-death' && event.data.killer) {
    const attacker = mapPlayer(event.data.killer);
    if (attacker.gameId !== (data.player as { gameId: string }).gameId) data.attacker = attacker;
  }
  if (event.type === 'entity-killed') {
    const entity = event.data.entity;
    if (!entity || typeof entity !== 'object' ||
        typeof entity.code !== 'string' || !entity.code.trim() ||
        typeof entity.name !== 'string' || !entity.name.trim()) {
      throw new Error('Native entity-killed event has no verified catalog entity');
    }
    data.entity = entity.code.trim();
    // Takaro EventEntityKilled requires a string. Empty means native could
    // verify the kill but could not identify the weapon; never invent one.
    data.weapon = '';
  }
  const date = new Date(event.ts);
  if (Number.isFinite(date.getTime())) data.timestamp = date.toISOString();
  return { type: event.type, data };
}

/** Native ring cursor advances on transport receipt evidence, not durable Takaro processing. */
export class EventPump {
  private timer: NodeJS.Timeout | null = null;
  private polling = false;
  private epoch = 0; // invalidates a native poll begun for an earlier socket/boot
  private bootId?: string;
  private scan: number;
  private persisted: number;
  private readonly inFlight: { sendId: number; seq: number }[] = [];

  constructor(
    private readonly native: NativeClient,
    private readonly takaro: TakaroWsClient,
    private readonly store: CursorStore,
    private readonly intervalMs: number,
  ) {
    const state = store.load();
    this.scan = state.seq;
    this.persisted = state.seq;
    this.bootId = state.bootId;
    takaro.onConfirmed((id) => this.confirm(id));
  }

  cursor(): number { return this.persisted; }
  scanCursor(): number { return this.scan; }
  currentBootId(): string | undefined { return this.bootId; }

  start(): void {
    if (this.timer) return;
    this.timer = setInterval(() => void this.pollOnce(), this.intervalMs);
    void this.pollOnce();
  }

  stop(): void {
    if (this.timer) clearInterval(this.timer);
    this.timer = null;
    this.disconnected();
  }

  disconnected(): void {
    this.epoch += 1;
    this.inFlight.length = 0;
    this.scan = this.persisted;
  }

  async pollOnce(): Promise<void> {
    if (this.polling || !this.takaro.identified()) return;
    this.polling = true;
    const epoch = this.epoch;
    try {
      const result = await this.native.events(this.scan);
      if (epoch !== this.epoch || !this.takaro.identified()) return;
      if (typeof result.bootId !== 'string' || !result.bootId) throw new Error('Native /events missing bootId');
      if (this.bootId && result.bootId !== this.bootId) {
        logger.warn('Native process restarted; resetting event cursor to this boot');
        this.epoch += 1;
        this.bootId = result.bootId;
        this.scan = 0;
        this.persisted = 0;
        this.inFlight.length = 0;
        this.store.save({ bootId: result.bootId, seq: 0 });
        return;
      }
      if (!this.bootId) {
        this.bootId = result.bootId;
        this.store.save({ bootId: result.bootId, seq: this.persisted });
      }
      if (result.truncated) logger.error(`Native chat event ring truncated before cursor ${this.scan}; some chat was lost`);
      if (!Array.isArray(result.events)) throw new Error('Native /events did not return an events array');
      for (const event of [...result.events].sort((a, b) => a.seq - b.seq)) {
        if (!Number.isSafeInteger(event.seq) || event.seq <= this.scan) continue;
        const mapped = mapEvent(event);
        if (!this.takaro.sendGameEvent(mapped.type, mapped.data)) break;
        this.scan = event.seq;
        this.inFlight.push({ sendId: this.takaro.lastSendId(), seq: event.seq });
      }
    } catch (error) {
      logger.warn(`Native event poll failed: ${error instanceof Error ? error.message : String(error)}`);
    } finally {
      this.polling = false;
    }
  }

  private confirm(sendId: number): void {
    let seq = this.persisted;
    while (this.inFlight.length && this.inFlight[0].sendId <= sendId) {
      seq = this.inFlight.shift()!.seq;
    }
    if (seq > this.persisted) {
      this.persisted = seq;
      this.store.save({ bootId: this.bootId, seq });
    }
  }
}
