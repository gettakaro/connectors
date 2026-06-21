import EventEmitter from 'node:events';
import WebSocket from 'ws';
import { logger } from '../logger.js';
import type { GameEventType, IdentifyPayload, WsMessage } from './protocol.js';

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
    logger.info(`Connecting to Takaro at ${this.url}`);
    this.ws = new WebSocket(this.url);

    this.ws.on('open', () => {
      this.reconnectAttempts = 0;
      this.send({ type: 'identify', payload: this.identifyPayload });
    });

    this.ws.on('message', (data) => {
      try {
        this.handle(JSON.parse(data.toString()) as WsMessage);
      } catch (err) {
        logger.warn(`Ignoring invalid Takaro message: ${(err as Error).message}`);
      }
    });

    this.ws.on('error', (err) => {
      logger.error(`Takaro WebSocket error: ${err.message}`);
    });

    this.ws.on('close', (code, reason) => {
      logger.warn(`Takaro WebSocket closed code=${code} reason=${reason.toString()}`);
      this.gameServerId = null;
      this.emit('disconnected');
      this.scheduleReconnect();
    });
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
    if (!this.ws || this.ws.readyState !== WebSocket.OPEN) {
      logger.warn(`Cannot send ${message.type}: Takaro WebSocket is not open`);
      return false;
    }
    this.ws.send(JSON.stringify(message));
    return true;
  }

  sendResponse(requestId: string, payload: unknown): void {
    this.send({ type: 'response', requestId, payload });
  }

  sendError(requestId: string, message: string): void {
    this.send({ type: 'error', requestId, payload: { message } });
  }

  sendGameEvent(type: GameEventType, data: unknown): void {
    this.send({ type: 'gameEvent', payload: { type, data } });
  }

  private scheduleReconnect(): void {
    if (this.shuttingDown) return;
    const delay = Math.min(this.maxReconnectMs, this.baseReconnectMs * 2 ** this.reconnectAttempts);
    this.reconnectAttempts += 1;
    this.reconnectTimer = setTimeout(() => this.connect(), delay);
  }

  private handle(message: WsMessage): void {
    switch (message.type) {
      case 'connected':
        logger.info('Takaro confirmed WebSocket connection');
        break;
      case 'identifyResponse': {
        const payload = message.payload as { gameServerId?: string; error?: unknown } | undefined;
        if (payload?.error) {
          logger.error(`Takaro identify failed: ${JSON.stringify(payload.error)}`);
          break;
        }
        if (payload?.gameServerId) {
          this.gameServerId = payload.gameServerId;
          logger.info(`Identified with Takaro as gameServerId=${payload.gameServerId}`);
          this.emit('identified', payload.gameServerId);
        }
        break;
      }
      case 'request':
        this.emit('request', message);
        break;
      case 'ping':
        this.send({ type: 'pong' });
        break;
      case 'error':
        logger.error(`Takaro error: ${JSON.stringify(message.payload)}`);
        break;
      default:
        logger.debug(`Unhandled Takaro message type=${message.type}`);
    }
  }
}
