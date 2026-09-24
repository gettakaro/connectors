import EventEmitter from 'node:events';
import WebSocket from 'ws';
import { logger } from '../logger.js';
import {
  createGameEvent,
  createIdentify,
  createResponse,
  type GameEventType,
  type IdentifyConfig,
  type WsMessage,
} from './protocol.js';

export interface TakaroClientOptions {
  baseReconnectMs?: number;
  maxReconnectMs?: number;
  /** How often to ping Takaro while the socket is open (default 5s). */
  pingIntervalMs?: number;
  /** Terminate the socket when no frame/pong has arrived for this long (default 20s). */
  idleTimeoutMs?: number;
  /** Terminate the socket once this many of our pings are outstanding with no pong (default 2). */
  maxMissedPongs?: number;
  /** Treat a send as undelivered when more than this many bytes are still buffered (default 1 MiB). */
  maxBufferedBytes?: number;
}

/**
 * Outbound WebSocket to Takaro. Emits: 'identified' (gameServerId|null), 'request' (WsMessage), 'disconnected',
 * 'confirmed' (sendId) — every gameEvent up to and including that sendId has transport receipt evidence.
 *
 * Why a pong and not "any inbound frame" (finding F20): during a `rig-outage` our egress is REJECTed, so `ws.send()`
 * still succeeds into the local kernel buffers while nothing reaches the peer, and the peer may still be able to send
 * US frames. An inbound `request`/`identifyResponse` therefore proves only that THEY are alive, never that our bytes
 * arrived. A pong does: the peer only emits it after processing our ping frame, and TCP/WebSocket framing is ordered,
 * so every byte we wrote before that ping was received first. Hence the confirmation anchor is "a pong for a ping that
 * was sent AFTER the event". This does not prove durable Takaro processing; replay after a lost confirmation may
 * duplicate an event, and peer failure after receipt may still lose processing.
 */
export class TakaroWsClient extends EventEmitter {
  private ws: WebSocket | null = null;
  private reconnectTimer: NodeJS.Timeout | null = null;
  private reconnectAttempts = 0;
  private shuttingDown = false;
  private isIdentified = false;
  private gameServerId: string | null = null;
  private readonly baseReconnectMs: number;
  private readonly maxReconnectMs: number;
  private readonly pingIntervalMs: number;
  private readonly idleTimeoutMs: number;
  private readonly maxMissedPongs: number;
  private readonly maxBufferedBytes: number;
  private watchdogTimer: NodeJS.Timeout | null = null;
  private lastActivity = 0;
  /** Monotonic id of the last gameEvent handed to the socket; the bridge keeps events until this id is confirmed. */
  private sendId = 0;
  /** Highest sendId proven delivered (a pong came back for a ping written after it). */
  private confirmedId = 0;
  private pingId = 0;
  /** Pings written but not yet ponged, oldest first. `upTo` = the sendId a pong for this ping would confirm. */
  private outstandingPings: { id: number; upTo: number }[] = [];

  constructor(
    private readonly url: string,
    private readonly identifyConfig: IdentifyConfig,
    options: TakaroClientOptions = {},
  ) {
    super();
    this.baseReconnectMs = options.baseReconnectMs ?? 2000;
    this.maxReconnectMs = options.maxReconnectMs ?? 60000;
    this.pingIntervalMs = options.pingIntervalMs ?? 5_000;
    this.idleTimeoutMs = options.idleTimeoutMs ?? 20_000;
    this.maxMissedPongs = options.maxMissedPongs ?? 2;
    this.maxBufferedBytes = options.maxBufferedBytes ?? 1024 * 1024;
  }

  connect(): void {
    if (this.shuttingDown) return;
    this.reconnectTimer = null;
    logger.info(`Connecting to Takaro at ${this.url}`);
    const ws = new WebSocket(this.url);
    this.ws = ws;

    ws.on('open', () => {
      if (this.ws !== ws) return;
      logger.info('Takaro WebSocket open, sending identify');
      this.lastActivity = Date.now();
      this.outstandingPings = [];
      this.startWatchdog(ws);
      this.sendFrame(ws, createIdentify(this.identifyConfig));
    });
    ws.on('message', (data) => {
      if (this.ws !== ws) return;
      this.lastActivity = Date.now();
      this.handleMessage(data.toString());
    });
    ws.on('pong', (data) => {
      if (this.ws !== ws) return;
      this.lastActivity = Date.now();
      this.notePong(data?.toString?.() ?? '');
    });
    ws.on('error', (err) => { if (this.ws === ws) logger.error(`Takaro WebSocket error: ${err.message}`); });
    ws.on('close', (code, reason) => {
      if (this.ws !== ws) return;
      logger.warn(`Takaro WebSocket closed code=${code} reason=${reason.toString()}`);
      this.stopWatchdog();
      this.outstandingPings = [];
      this.ws = null;
      this.isIdentified = false;
      this.gameServerId = null;
      this.emit('disconnected');
      this.scheduleReconnect();
    });
  }

  /** Id of the last gameEvent written to the socket (0 = none this process). */
  lastSendId(): number {
    return this.sendId;
  }

  /** Highest sendId with transport receipt evidence, not durable processing ack. */
  lastConfirmedId(): number {
    return this.confirmedId;
  }

  /** Register a callback fired when delivery of everything up to `sendId` is proven. */
  onConfirmed(cb: (sendId: number) => void): void {
    this.on('confirmed', cb);
  }

  private notePong(payload: string): void {
    if (!this.outstandingPings.length) return;
    const idx = this.outstandingPings.findIndex((p) => String(p.id) === payload);
    // Unsolicited pongs are legal. Only the exact echoed payload confirms our
    // ping and the preceding ordered WebSocket frames.
    if (idx < 0) return;
    const hit = this.outstandingPings[idx];
    this.outstandingPings = this.outstandingPings.slice(idx + 1);
    if (hit.upTo > this.confirmedId) {
      this.confirmedId = hit.upTo;
      this.emit('confirmed', this.confirmedId);
    }
  }

  identified(): boolean {
    return this.isIdentified;
  }

  getGameServerId(): string | null {
    return this.gameServerId;
  }

  nextReconnectDelay(): number {
    return Math.min(this.maxReconnectMs, this.baseReconnectMs * 2 ** this.reconnectAttempts);
  }

  /**
   * Writes a frame. Returns false when the frame was NOT handed to an open, identified socket, so the caller can keep
   * the event and retry after the next identify (finding F10: frames written to a half-dead socket vanish silently,
   * and the `close` only arrives minutes later as code 1006).
   */
  send(message: WsMessage): boolean {
    const ws = this.ws;
    if (!ws || ws.readyState !== WebSocket.OPEN) {
      logger.warn(`Cannot send ${message.type}: Takaro WebSocket is not open`);
      return false;
    }
    // Only `identify` may go out before Takaro has identified us; everything else is dropped by the server.
    if (!this.isIdentified && message.type !== 'identify' && message.type !== 'pong') {
      logger.warn(`Cannot send ${message.type}: Takaro has not identified this connection yet`);
      return false;
    }
    return this.sendFrame(ws, message);
  }

  private sendFrame(ws: WebSocket, message: WsMessage): boolean {
    const frame = JSON.stringify(message);
    if (ws.bufferedAmount > this.maxBufferedBytes) {
      logger.warn(`Cannot send ${message.type}: ${ws.bufferedAmount} bytes still buffered (socket is not draining)`);
      return false;
    }
    logger.debug(`WS SEND ${redactFrame(frame)}`);
    try {
      ws.send(frame, (err) => {
        if (!err) return;
        // The callback fires after the fact; the frame is lost, so tear the socket down and let the queue re-send.
        logger.error(`Takaro WebSocket send failed (${message.type}): ${err.message}`);
        if (this.ws === ws) ws.terminate();
      });
    } catch (err) {
      logger.error(`Takaro WebSocket send threw (${message.type}): ${(err as Error).message}`);
      return false;
    }
    return true;
  }

  private startWatchdog(ws: WebSocket): void {
    this.stopWatchdog();
    if (this.pingIntervalMs <= 0) return;
    this.watchdogTimer = setInterval(() => {
      if (this.ws !== ws || ws.readyState !== WebSocket.OPEN) return;
      if (this.outstandingPings.length >= this.maxMissedPongs) {
        logger.warn(
          `${this.outstandingPings.length} heartbeat ping(s) unanswered by Takaro; the socket is dead — terminating and reconnecting`,
        );
        ws.terminate();
        return;
      }
      if (Date.now() - this.lastActivity > this.idleTimeoutMs) {
        logger.warn(`No traffic from Takaro for ${Math.round(this.idleTimeoutMs / 1000)}s; terminating the socket and reconnecting`);
        ws.terminate();
        return;
      }
      try {
        this.pingId += 1;
        this.outstandingPings.push({ id: this.pingId, upTo: this.sendId });
        ws.ping(String(this.pingId));
      } catch (err) {
        logger.warn(`Takaro ping failed: ${(err as Error).message}`);
        ws.terminate();
      }
    }, this.pingIntervalMs);
    this.watchdogTimer.unref?.();
  }

  private stopWatchdog(): void {
    if (this.watchdogTimer) clearInterval(this.watchdogTimer);
    this.watchdogTimer = null;
  }

  sendResponse(requestId: string, payload: unknown): boolean {
    return this.send(createResponse(requestId, payload));
  }

  /**
   * Writes a gameEvent. `true` means only that the bytes were accepted by the local socket — NOT that Takaro got them
   * (F20). The caller must keep the event until a 'confirmed' event covers `lastSendId()`.
   */
  sendGameEvent(type: GameEventType, data: unknown): boolean {
    const ok = this.send(createGameEvent(type, data));
    if (ok) this.sendId += 1;
    return ok;
  }

  shutdown(): void {
    this.shuttingDown = true;
    this.stopWatchdog();
    if (this.reconnectTimer) clearTimeout(this.reconnectTimer);
    this.reconnectTimer = null;
    const ws = this.ws;
    this.ws = null;
    ws?.close();
  }

  private handleMessage(raw: string): void {
    logger.debug(`WS RECV ${redactFrame(raw)}`);
    let message: WsMessage;
    try {
      message = JSON.parse(raw) as WsMessage;
    } catch (err) {
      logger.warn(`Ignoring invalid Takaro message: ${(err as Error).message}`);
      return;
    }

    switch (message.type) {
      case 'connected':
        logger.info('Takaro confirmed WebSocket connection');
        break;
      case 'identifyResponse': {
        const payload = (message.payload ?? {}) as { error?: unknown; gameServerId?: string; server?: { id?: string } };
        if (payload.error) {
          logger.error(`Takaro identify failed: ${JSON.stringify(payload.error)}`);
          // Let the socket close / retry naturally; force a reconnect cycle with backoff.
          this.ws?.close();
          break;
        }
        this.reconnectAttempts = 0;
        this.isIdentified = true;
        this.gameServerId = payload.gameServerId ?? payload.server?.id ?? null;
        logger.info(`Identified with Takaro${this.gameServerId ? ` (gameServerId=${this.gameServerId})` : ''}`);
        this.emit('identified', this.gameServerId);
        break;
      }
      case 'request':
        this.emit('request', message);
        break;
      case 'ping':
        if (this.ws) this.sendFrame(this.ws, { type: 'pong' });
        break;
      case 'error':
        logger.error(`Takaro error: ${JSON.stringify(message.payload ?? message.error)}`);
        break;
      default:
        logger.debug(`Unhandled Takaro message type=${message.type}`);
    }
  }

  private scheduleReconnect(): void {
    if (this.shuttingDown || this.reconnectTimer) return;
    const delay = this.nextReconnectDelay();
    this.reconnectAttempts += 1;
    logger.info(`Reconnecting to Takaro in ${delay}ms`);
    this.reconnectTimer = setTimeout(() => this.connect(), delay);
  }
}

/** Never let a registration/identity token or a world password reach the logs. */
function redactFrame(frame: string): string {
  const redacted = frame
    .replace(/("(?:registrationToken|identityToken|token|password)"\s*:\s*")[^"]*"/gi, '$1<redacted>"')
    .replace(/(\?p=)[^\s"&]+/gi, '$1<redacted>');
  return redacted.length > 2000 ? `${redacted.slice(0, 2000)}…` : redacted;
}
