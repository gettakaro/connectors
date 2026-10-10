import EventEmitter from 'node:events';
import WebSocket from 'ws';
import type { GameEventType, IdentifyPayload, RequestPayload, WsMessage } from './protocol.js';

export class TakaroWsClient extends EventEmitter {
  private ws: WebSocket | null = null;
  private gameServerId: string | null = null;
  private reconnectTimer: NodeJS.Timeout | null = null;
  private reconnectAttempts = 0;
  private shuttingDown = false;

  constructor(
    private url: string,
    private identifyPayload: IdentifyPayload,
    private readonly baseReconnectMs = 3000,
    private readonly maxReconnectMs = 60000,
  ) {
    super();
  }

  /** Opens the socket, unless there is no registration token to identify with. */
  connect(): void {
    if (this.shuttingDown || this.ws) return;
    if (!this.identifyPayload.registrationToken) return;
    const ws = new WebSocket(this.url);
    this.ws = ws;
    // Every callback checks that its socket is still the current one: a socket dropped by
    // reconfigure() closes later, and its close must not tear down or reschedule the new one.
    ws.on('open', () => {
      if (this.ws !== ws) return;
      this.reconnectAttempts = 0;
      this.send({ type: 'identify', payload: this.identifyPayload });
    });
    ws.on('message', (data) => {
      if (this.ws !== ws) return;
      try {
        this.handle(JSON.parse(data.toString()) as WsMessage);
      } catch (err) {
        this.emit('clientError', err);
      }
    });
    ws.on('close', () => {
      if (this.ws !== ws) return;
      this.ws = null;
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
    this.reconnectAttempts = 0;
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
    this.emit('disconnected');
  }

  private clearReconnectTimer(): void {
    if (this.reconnectTimer) clearTimeout(this.reconnectTimer);
    this.reconnectTimer = null;
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

  sendError(requestId: string, message: string): void {
    this.send({ type: 'error', requestId, payload: { message } });
  }

  sendGameEvent(type: GameEventType, data: unknown): boolean {
    return this.send({ type: 'gameEvent', payload: { type, data } });
  }

  private handle(message: WsMessage): void {
    switch (message.type) {
      case 'connected':
        break;
      case 'identifyResponse': {
        const payload = message.payload as { gameServerId?: string; error?: unknown } | undefined;
        if (payload?.error) {
          this.emit('identifyError', payload.error);
          return;
        }
        if (payload?.gameServerId) {
          this.gameServerId = payload.gameServerId;
          this.emit('identified', payload.gameServerId);
        }
        break;
      }
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
    const delay = Math.min(this.maxReconnectMs, this.baseReconnectMs * 2 ** this.reconnectAttempts);
    this.reconnectAttempts += 1;
    this.reconnectTimer = setTimeout(() => {
      this.reconnectTimer = null;
      this.connect();
    }, delay);
  }
}
