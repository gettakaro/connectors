import { randomUUID } from 'node:crypto';
import EventEmitter from 'node:events';
import WebSocket from 'ws';
import { DEBUG, logger, truncate } from '../logger.js';
import { CONNECTOR_IDENTITY } from '../gameProfile.js';
import type { GameEventType, IdentifyPayload, ProtocolErrorCode, WsMessage } from './protocol.js';

/** Protocol v1 event buffer: at least the 1,000 events the protocol recommends. */
export const EVENT_BUFFER_LIMIT = 2000;

interface BufferedEvent {
  seq: number;
  type: GameEventType;
  data: unknown;
}

/**
 * Takaro drops a server's events past 100 per 5 s (sustained) or 600 per 30 s (burst) and reports
 * them as event-rate-limited. Every event frame leaves through one token bucket that stays under
 * that: a boot backlog or an outage resend drains at this pace instead of in one burst.
 */
export const EVENT_RATE_PER_SEC = 14;
export const EVENT_BURST = 20;

export interface ProtocolOptions {
  /** Offer protocol version 1, with version 0 as the fallback Takaro may pick. */
  offerV1: boolean;
  /** The Capability Manifest; built again on every identify and capabilities refresh. */
  manifest: () => unknown[];
}

export class TakaroWsClient extends EventEmitter {
  private ws: WebSocket | null = null;
  private gameServerId: string | null = null;
  private reconnectMs = 5000;
  private shuttingDown = false;
  private connectedFlag = false;
  private protocolVersion = 0;
  // Protocol v1 delivery: one stream per process, seq strictly increasing across reconnects.
  private readonly streamId = randomUUID();
  private nextSeq = 1;
  private readonly buffer: BufferedEvent[] = [];
  private dropped = 0;
  private lastManifest = '';
  // Event frames waiting for a token; cleared when the socket closes (v1 resends from `buffer`).
  private readonly outgoing: WsMessage[] = [];
  private tokens = EVENT_BURST;
  private tokensAt = Date.now();
  private pacer: NodeJS.Timeout | null = null;

  constructor(
    private url: string,
    private identify: IdentifyPayload,
    private readonly protocol: ProtocolOptions = { offerV1: false, manifest: () => [] },
  ) {
    super();
  }

  /** The protocol version Takaro picked for the current connection (0 until identified). */
  getProtocolVersion(): number {
    return this.protocolVersion;
  }

  /** Events sent on protocol v1 that Takaro has not acknowledged yet. */
  unacknowledged(): number {
    return this.buffer.length;
  }

  private identifyMessage(): IdentifyPayload {
    // game and connector are accepted on version 0 too; the game profile needs version 1.
    const identify: IdentifyPayload = { ...this.identify, ...CONNECTOR_IDENTITY };
    if (!this.protocol.offerV1) return identify;
    const capabilities = this.protocol.manifest();
    this.lastManifest = JSON.stringify(capabilities);
    return { ...identify, protocolVersions: [0, 1], capabilities };
  }

  /**
   * Sends a capabilities replacement on a protocol v1 connection when the manifest changed
   * since the last identify or refresh (the world survey can finish after identify).
   */
  refreshCapabilities(): boolean {
    if (!this.identified() || this.protocolVersion !== 1) return false;
    const capabilities = this.protocol.manifest();
    const json = JSON.stringify(capabilities);
    if (json === this.lastManifest) return false;
    this.lastManifest = json;
    logger.info('Capability Manifest changed; sending capabilities');
    this.send({ type: 'capabilities', requestId: randomUUID(), payload: { capabilities } });
    return true;
  }

  connect(): void {
    logger.info(`WS connect ${this.url}`);
    this.ws = new WebSocket(this.url);

    this.ws.on('open', () => {
      this.connectedFlag = true;
      logger.info('WS open; sending identify');
      this.send({ type: 'identify', payload: this.identifyMessage() });
    });

    this.ws.on('message', (data) => {
      const raw = data.toString();
      if (DEBUG) logger.debug(`WS RECV ${truncate(raw)}`);
      let msg: WsMessage;
      try {
        msg = JSON.parse(raw) as WsMessage;
      } catch (err) {
        logger.warn(`Bad WS payload: ${(err as Error).message}`);
        return;
      }
      this.handle(msg);
    });

    this.ws.on('error', (err) => {
      logger.error(`WS error: ${err.message}`);
    });

    this.ws.on('close', (code, reason) => {
      logger.warn(`WS closed code=${code} reason=${reason.toString()}`);
      this.connectedFlag = false;
      this.gameServerId = null;
      this.protocolVersion = 0;
      // Queued frames belong to the closed connection; version 1 resends from the buffer.
      this.outgoing.length = 0;
      this.emit('disconnected');
      if (!this.shuttingDown) {
        setTimeout(() => this.connect(), this.reconnectMs);
      }
    });
  }

  shutdown(): void {
    this.shuttingDown = true;
    this.connectedFlag = false;
    this.ws?.close();
  }

  send(msg: WsMessage): boolean {
    if (!this.ws || this.ws.readyState !== WebSocket.OPEN) {
      logger.warn(`Cannot send ${msg.type}: WS not open`);
      return false;
    }
    const frame = JSON.stringify(msg);
    if (DEBUG) logger.debug(`WS SEND ${truncate(frame)}`);
    this.ws.send(frame);
    return true;
  }

  sendResponse(requestId: string, payload: unknown): void {
    this.send({ type: 'response', requestId, payload });
  }

  /** A failed request: the protocol v1 `{code, message}` error, or the v0 error frame. */
  sendError(requestId: string, message: string, code: ProtocolErrorCode = 'game_error'): void {
    if (this.protocolVersion === 1) {
      this.send({ type: 'response', requestId, error: { code, message } });
      return;
    }
    this.send({ type: 'error', requestId, payload: { message } });
  }

  /**
   * Protocol v0: sent once. Protocol v1: numbered on this process's stream, kept until Takaro
   * acknowledges it and resent after a reconnect; the oldest is dropped on overflow.
   */
  sendGameEvent(type: GameEventType, data: unknown): void {
    // Identified on version 0: sent once, as before.
    if (this.gameServerId !== null && this.protocolVersion !== 1) {
      this.queueEvent({ type: 'gameEvent', payload: { type, data } });
      return;
    }
    // Version 1, or not identified yet (the socket can be open before identify answers, and the
    // log tail does not wait): numbered and buffered. Identify sends the buffer in whichever
    // version Takaro picked; sending it now would go out without a stream and be rejected.
    const event = { seq: this.nextSeq++, type, data };
    this.buffer.push(event);
    if (this.buffer.length > EVENT_BUFFER_LIMIT) {
      this.buffer.shift();
      this.dropped += 1;
      if (this.dropped === 1 || this.dropped % 100 === 0) logger.warn(`Event buffer full: ${this.dropped} unacknowledged event(s) dropped`);
    }
    if (this.gameServerId !== null) this.sendBuffered(event);
  }

  private sendBuffered(event: BufferedEvent): void {
    this.queueEvent({ type: 'gameEvent', payload: { type: event.type, data: event.data, streamId: this.streamId, seq: event.seq } });
  }

  /** Event frames go out through the token bucket, in order. */
  private queueEvent(frame: WsMessage): void {
    this.outgoing.push(frame);
    this.drainEvents();
  }

  private drainEvents(): void {
    const now = Date.now();
    this.tokens = Math.min(EVENT_BURST, this.tokens + ((now - this.tokensAt) / 1000) * EVENT_RATE_PER_SEC);
    this.tokensAt = now;
    while (this.outgoing.length && this.tokens >= 1) {
      if (!this.send(this.outgoing.shift()!)) {
        this.outgoing.length = 0; // socket gone: v1 resends from the buffer after identify
        break;
      }
      this.tokens -= 1;
    }
    if (this.outgoing.length && !this.pacer) {
      this.pacer = setTimeout(() => {
        this.pacer = null;
        this.drainEvents();
      }, Math.ceil(1000 / EVENT_RATE_PER_SEC));
      this.pacer.unref?.();
    }
  }

  private acknowledge(payload: unknown): void {
    const { streamId, upTo } = (payload ?? {}) as { streamId?: unknown; upTo?: unknown };
    if (streamId !== this.streamId || typeof upTo !== 'number') return;
    while (this.buffer.length && this.buffer[0]!.seq <= upTo) this.buffer.shift();
  }

  /** True while the socket is open (independent of whether identify succeeded). */
  connected(): boolean {
    return this.connectedFlag && this.ws?.readyState === WebSocket.OPEN;
  }

  identified(): boolean {
    return this.gameServerId !== null;
  }

  getGameServerId(): string | null {
    return this.gameServerId;
  }

  private logIdentifyError(error: unknown): void {
    const err = (error ?? {}) as { message?: string; name?: string; code?: string; http?: number; details?: unknown };
    const summary = err.message || JSON.stringify(error);
    logger.error(`Identification failed: ${summary} [name=${err.name ?? err.code ?? '?'} http=${err.http ?? '?'}]`);
    if (err.details) logger.error(`Error details: ${JSON.stringify(err.details)}`);
  }

  private handle(msg: WsMessage): void {
    switch (msg.type) {
      case 'connected':
        logger.info('Server confirmed WebSocket connection');
        break;
      case 'identifyResponse': {
        // Protocol v1 rejections carry a top-level error; v0 puts it in the payload.
        if (msg.error) {
          this.logIdentifyError(msg.error);
          break;
        }
        const p = msg.payload as { gameServerId?: string; protocolVersion?: number; error?: unknown } | undefined;
        if (p?.error) {
          this.logIdentifyError(p.error);
          break;
        }
        if (p?.gameServerId) {
          this.gameServerId = p.gameServerId;
          this.protocolVersion = p.protocolVersion === 1 ? 1 : 0;
          logger.info(`Identified as gameServerId=${p.gameServerId} protocolVersion=${this.protocolVersion}`);
          if (this.protocolVersion === 1) {
            for (const event of this.buffer) this.sendBuffered(event);
          } else {
            // Version 0 has no acknowledgements: what waited for identify goes out once.
            for (const event of this.buffer) this.queueEvent({ type: 'gameEvent', payload: { type: event.type, data: event.data } });
            this.buffer.length = 0;
          }
          this.emit('identified', p.gameServerId);
        }
        break;
      }
      case 'eventAck':
        this.acknowledge(msg.payload);
        break;
      case 'eventError':
        logger.warn(`Takaro rejected an event: ${JSON.stringify({ ...(msg.payload as object), error: msg.error })}`);
        break;
      case 'capabilitiesResponse':
        if (msg.error) logger.error(`Takaro rejected the Capability Manifest: ${JSON.stringify(msg.error)}`);
        else logger.info('Takaro accepted the Capability Manifest');
        break;
      case 'request':
        this.emit('request', msg);
        break;
      case 'error':
        logger.error(`Takaro error: ${JSON.stringify(msg.payload)}`);
        break;
      case 'ping':
        this.send({ type: 'pong' });
        break;
      default:
        logger.debug(`Unhandled WS type: ${msg.type}`);
    }
  }
}
