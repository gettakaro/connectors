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
    private readonly url: string,
    private readonly identifyPayload: IdentifyPayload,
    private readonly baseReconnectMs = 3000,
    private readonly maxReconnectMs = 60000,
  ) {
    super();
  }

  connect(): void {
    if (this.shuttingDown) return;
    this.ws = new WebSocket(this.url);
    this.ws.on('open', () => {
      this.reconnectAttempts = 0;
      this.send({ type: 'identify', payload: this.identifyPayload });
    });
    this.ws.on('message', (data) => {
      try {
        this.handle(JSON.parse(data.toString()) as WsMessage);
      } catch (err) {
        this.emit('clientError', err);
      }
    });
    this.ws.on('close', () => {
      this.gameServerId = null;
      this.emit('disconnected');
      this.scheduleReconnect();
    });
    this.ws.on('error', (err) => this.emit('clientError', err));
  }

  shutdown(): void {
    this.shuttingDown = true;
    if (this.reconnectTimer) clearTimeout(this.reconnectTimer);
    this.ws?.close();
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
    const delay = Math.min(this.maxReconnectMs, this.baseReconnectMs * 2 ** this.reconnectAttempts);
    this.reconnectAttempts += 1;
    this.reconnectTimer = setTimeout(() => this.connect(), delay);
  }
}
