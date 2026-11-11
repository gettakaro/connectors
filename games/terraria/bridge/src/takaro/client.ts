import { randomUUID } from 'node:crypto';
import EventEmitter from 'node:events';
import WebSocket from 'ws';
import { CONNECTOR_IDENTITY } from './gameProfile.js';
import type { GameEventType, IdentifyPayload, ProtocolErrorCode, RequestPayload, WsMessage } from './protocol.js';
import { DEFAULT_RECONNECT_TIMING, reconnectDelay, type ReconnectTiming } from './reconnectPolicy.js';

/** Version 1 event buffer: at least the 1,000 the protocol recommends. */
export const EVENT_BUFFER_LIMIT = 2000;

interface BufferedEvent {
  seq: number;
  type: GameEventType;
  data: unknown;
}

export interface ProtocolOptions {
  /** Offer protocol version 1 (with version 0 as fallback). */
  offerV1: boolean;
  /** The Capability Manifest to identify with; asked again on every connect. */
  manifest: () => unknown[];
}

export class TakaroWsClient extends EventEmitter {
  private ws: WebSocket | null = null;
  private gameServerId: string | null = null;
  private reconnectTimer: NodeJS.Timeout | null = null;
  /** Connect attempts that failed since the last stable session; see reconnectPolicy.ts. */
  private failures = 0;
  /** The current socket was identified and stayed up for stableMs. */
  private sessionStable = false;
  private stableTimer: NodeJS.Timeout | null = null;
  private pingTimer: NodeJS.Timeout | null = null;
  private livenessTimer: NodeJS.Timeout | null = null;
  private lastInboundAt = 0;
  private readonly timing: ReconnectTiming;
  private shuttingDown = false;
  private protocolVersion = 0;
  /** The version the last identified socket negotiated; decides whether a gap buffers events. */
  private streamVersion = 0;
  /** The game server the buffered events were written for. */
  private streamServerId: string | null = null;
  // Version 1 delivery: one stream per process, seq strictly increasing across reconnects.
  private readonly streamId = randomUUID();
  private nextSeq = 1;
  private readonly buffer: BufferedEvent[] = [];
  private dropped = 0;

  constructor(
    private url: string,
    private identifyPayload: IdentifyPayload,
    baseReconnectMs = DEFAULT_RECONNECT_TIMING.baseMs,
    maxReconnectMs = DEFAULT_RECONNECT_TIMING.maxMs,
    private readonly protocol: ProtocolOptions = { offerV1: false, manifest: () => [] },
    timing: Partial<ReconnectTiming> = {},
  ) {
    super();
    this.timing = { ...DEFAULT_RECONNECT_TIMING, baseMs: baseReconnectMs, maxMs: maxReconnectMs, ...timing };
  }

  /** The protocol version negotiated on the current connection (0 until identified). */
  getProtocolVersion(): number {
    return this.protocolVersion;
  }

  /** Events sent but not yet acknowledged by Takaro (version 1 only). */
  unacknowledged(): number {
    return this.buffer.length;
  }

  private identifyMessage(): IdentifyPayload {
    // game and connector are accepted on version 0 too; the game profile needs version 1.
    const payload: IdentifyPayload = { ...this.identifyPayload, ...CONNECTOR_IDENTITY };
    if (!this.protocol.offerV1) return payload;
    return { ...payload, protocolVersions: [0, 1], capabilities: this.protocol.manifest() };
  }

  /**
   * Replaces the Capability Manifest on a version 1 connection, for example when the map
   * image gets a new revision or TShock reports another game build. A version 0 connection has nothing to replace.
   */
  sendCapabilities(): boolean {
    if (!this.identified() || this.protocolVersion !== 1) return false;
    return this.send({ type: 'capabilities', requestId: randomUUID(), payload: { capabilities: this.protocol.manifest() } });
  }

  /** Opens the socket, unless there is no registration token to identify with. */
  connect(): void {
    if (this.shuttingDown || this.ws) return;
    if (!this.identifyPayload.registrationToken) return;
    const ws = new WebSocket(this.url, { handshakeTimeout: this.timing.handshakeTimeoutMs });
    this.ws = ws;
    // Every callback checks that its socket is still the current one: a socket dropped by
    // reconfigure() closes later, and its close must not tear down or reschedule the new one.
    ws.on('open', () => {
      if (this.ws !== ws) return;
      // An open socket is not a success: the failure streak resets only once the session
      // is identified and stable, so a peer that accepts and drops at once still backs off.
      this.watchLiveness(ws);
      this.send({ type: 'identify', payload: this.identifyMessage() });
    });
    // Control-frame pongs and pings count as traffic too.
    ws.on('pong', () => {
      if (this.ws === ws) this.lastInboundAt = Date.now();
    });
    ws.on('ping', () => {
      if (this.ws === ws) this.lastInboundAt = Date.now();
    });
    ws.on('message', (data) => {
      if (this.ws !== ws) return;
      this.lastInboundAt = Date.now();
      try {
        this.handle(JSON.parse(data.toString()) as WsMessage);
      } catch (err) {
        this.emit('clientError', err);
      }
    });
    ws.on('close', (code, reason) => {
      if (this.ws !== ws) return;
      this.ws = null;
      const wasStable = this.sessionStable;
      this.clearSessionTimers();
      this.failures = wasStable ? 0 : this.failures + 1;
      this.emit('closed', { code, reason: reason.toString(), stable: wasStable });
      this.markDisconnected();
      this.scheduleReconnect();
    });
    ws.on('error', (err) => {
      if (this.ws !== ws) return;
      this.emit('clientError', err);
    });
  }

  /**
   * New connection settings. When they differ, the current socket is dropped and a new one
   * opened at once, skipping any backoff. An empty registration token leaves it closed.
   */
  reconfigure(url: string, identifyPayload: IdentifyPayload): boolean {
    const same = url === this.url
      && identifyPayload.registrationToken === this.identifyPayload.registrationToken
      && identifyPayload.identityToken === this.identifyPayload.identityToken
      && identifyPayload.name === this.identifyPayload.name;
    if (same) return false;
    this.url = url;
    this.identifyPayload = identifyPayload;
    if (this.shuttingDown) return true;
    this.clearReconnectTimer();
    this.failures = 0;
    this.dropSocket();
    this.connect();
    return true;
  }

  shutdown(): void {
    this.shuttingDown = true;
    this.clearReconnectTimer();
    // Not reported as a disconnect: nothing reconnects, and the log must not say it does.
    this.dropSocket(false);
    this.gameServerId = null;
  }

  /** True when a socket is open or a reconnect is pending. */
  active(): boolean {
    return this.ws !== null || this.reconnectTimer !== null;
  }

  private dropSocket(notify = true): void {
    const ws = this.ws;
    if (!ws) return;
    this.ws = null;
    this.clearSessionTimers();
    ws.removeAllListeners('message');
    // A late 'error' on a socket closed before it opened would otherwise be unhandled.
    ws.on('error', () => undefined);
    try {
      ws.close();
    } catch {
      ws.terminate();
    }
    if (notify) this.markDisconnected();
  }

  private markDisconnected(): void {
    this.gameServerId = null;
    this.protocolVersion = 0;
    this.emit('disconnected');
  }

  private clearReconnectTimer(): void {
    if (this.reconnectTimer) clearTimeout(this.reconnectTimer);
    this.reconnectTimer = null;
  }

  /**
   * A blackholed link keeps the socket "open" and swallows everything written to it, and
   * nothing on the TCP level says so for minutes. So the last inbound frame is tracked, and
   * a socket silent for inboundTimeoutMs is terminated; its close reconnects as usual.
   */
  private watchLiveness(ws: WebSocket): void {
    this.lastInboundAt = Date.now();
    // Checked often enough that a dead link is closed at most a ninth past the timeout.
    const checkEvery = Math.max(20, Math.floor(this.timing.inboundTimeoutMs / 9));
    this.livenessTimer = setInterval(() => {
      if (this.ws !== ws) return;
      const silentMs = Date.now() - this.lastInboundAt;
      if (silentMs < this.timing.inboundTimeoutMs) return;
      this.emit('linkDead', silentMs);
      ws.terminate();
    }, checkEvery);
  }

  /** Called once identify is accepted: start the pings and the stability clock. */
  private startSession(ws: WebSocket): void {
    if (this.pingTimer) clearInterval(this.pingTimer);
    this.pingTimer = setInterval(() => {
      if (this.ws === ws) this.send({ type: 'ping' });
    }, this.timing.pingIntervalMs);
    if (this.stableTimer) clearTimeout(this.stableTimer);
    this.stableTimer = setTimeout(() => {
      this.stableTimer = null;
      if (this.ws !== ws || !this.identified()) return;
      this.sessionStable = true;
      this.failures = 0;
    }, this.timing.stableMs);
  }

  private clearSessionTimers(): void {
    if (this.stableTimer) clearTimeout(this.stableTimer);
    if (this.pingTimer) clearInterval(this.pingTimer);
    if (this.livenessTimer) clearInterval(this.livenessTimer);
    this.stableTimer = null;
    this.pingTimer = null;
    this.livenessTimer = null;
    this.sessionStable = false;
  }

  identified(): boolean {
    return this.gameServerId !== null;
  }

  getGameServerId(): string | null {
    return this.gameServerId;
  }

  send(message: WsMessage): boolean {
    if (!this.ws || this.ws.readyState !== WebSocket.OPEN) return false;
    this.ws.send(JSON.stringify(message));
    return true;
  }

  sendResponse(requestId: string, payload: unknown): void {
    this.send({ type: 'response', requestId, payload });
  }

  /** A failed request: the version 1 error shape, or the version 0 error frame. */
  sendError(requestId: string, message: string, code: ProtocolErrorCode = 'game_error'): void {
    if (this.protocolVersion === 1) {
      this.send({ type: 'response', requestId, error: { code, message } });
      return;
    }
    this.send({ type: 'error', requestId, payload: { message } });
  }

  /**
   * Version 0: sent once, lost when the socket is down. Version 1: numbered, buffered until
   * Takaro acknowledges it and resent after a reconnect; the oldest is dropped on overflow.
   */
  sendGameEvent(type: GameEventType, data: unknown): boolean {
    // Between two version 1 sockets the event is buffered like any unacknowledged one.
    const v1 = this.identified() ? this.protocolVersion === 1 : this.streamVersion === 1;
    if (!v1) return this.send({ type: 'gameEvent', payload: { type, data } });
    const event = { seq: this.nextSeq++, type, data };
    this.buffer.push(event);
    if (this.buffer.length > EVENT_BUFFER_LIMIT) {
      this.buffer.shift();
      this.dropped += 1;
      if (this.dropped === 1 || this.dropped % 100 === 0) this.emit('eventsDropped', this.dropped);
    }
    this.sendBuffered(event);
    return true;
  }

  private sendBuffered(event: BufferedEvent): boolean {
    return this.send({ type: 'gameEvent', payload: { type: event.type, data: event.data, streamId: this.streamId, seq: event.seq } });
  }

  private acknowledge(payload: unknown): void {
    const { streamId, upTo } = (payload ?? {}) as { streamId?: unknown; upTo?: unknown };
    if (streamId !== this.streamId || typeof upTo !== 'number') return;
    while (this.buffer.length && this.buffer[0].seq <= upTo) this.buffer.shift();
  }

  private handle(message: WsMessage): void {
    switch (message.type) {
      case 'connected':
        break;
      case 'identifyResponse': {
        // Version 1 failures carry a top-level error and close the socket (1008); the
        // reconnect backoff keeps that from becoming a tight loop.
        if (message.error) {
          this.emit('identifyError', message.error);
          return;
        }
        const payload = message.payload as { gameServerId?: string; protocolVersion?: number; error?: unknown } | undefined;
        if (payload?.error) {
          this.emit('identifyError', payload.error);
          return;
        }
        if (payload?.gameServerId) {
          this.gameServerId = payload.gameServerId;
          if (this.ws) this.startSession(this.ws);
          this.protocolVersion = payload.protocolVersion === 1 ? 1 : 0;
          this.streamVersion = this.protocolVersion;
          // Events written for one game server are never replayed to another (new token or identity).
          if (this.streamServerId !== null && this.streamServerId !== payload.gameServerId) this.buffer.length = 0;
          this.streamServerId = payload.gameServerId;
          if (this.protocolVersion === 1) {
            for (const event of this.buffer) this.sendBuffered(event);
          } else {
            this.buffer.length = 0;
          }
          this.emit('identified', payload.gameServerId);
        }
        break;
      }
      case 'eventAck':
        this.acknowledge(message.payload);
        break;
      case 'eventError':
        this.emit('eventError', { ...(message.payload as object), error: message.error });
        break;
      case 'capabilitiesResponse':
        if (message.error) this.emit('capabilitiesError', message.error);
        else this.emit('capabilitiesAccepted');
        break;
      case 'request':
        this.emit('request', {
          ...message,
          payload: message.payload as RequestPayload,
        });
        break;
      case 'ping':
        this.send({ type: 'pong' });
        break;
      case 'error':
        this.emit('serverError', message.payload);
        break;
      default:
        break;
    }
  }

  private scheduleReconnect(): void {
    if (this.shuttingDown) return;
    this.clearReconnectTimer();
    const delay = reconnectDelay(this.failures, this.timing);
    this.emit('reconnectScheduled', { delayMs: delay, failures: this.failures });
    this.reconnectTimer = setTimeout(() => {
      this.reconnectTimer = null;
      this.connect();
    }, delay);
  }
}
