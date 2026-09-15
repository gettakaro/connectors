import EventEmitter from 'node:events';
import WebSocket from 'ws';
import { logger } from '../logger.js';
import {
  createErrorResponse,
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
}

/**
 * Outbound WebSocket to Takaro. Emits: 'identified' (gameServerId|null), 'request' (WsMessage), 'disconnected'.
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

  constructor(
    private readonly url: string,
    private readonly identifyConfig: IdentifyConfig,
    options: TakaroClientOptions = {},
  ) {
    super();
    this.baseReconnectMs = options.baseReconnectMs ?? 2000;
    this.maxReconnectMs = options.maxReconnectMs ?? 60000;
  }

  connect(): void {
    if (this.shuttingDown) return;
    this.reconnectTimer = null;
    logger.info(`Connecting to Takaro at ${this.url}`);
    const ws = new WebSocket(this.url);
    this.ws = ws;

    ws.on('open', () => {
      logger.info('Takaro WebSocket open, sending identify');
      this.send(createIdentify(this.identifyConfig));
    });
    ws.on('message', (data) => this.handleMessage(data.toString()));
    ws.on('error', (err) => logger.error(`Takaro WebSocket error: ${err.message}`));
    ws.on('close', (code, reason) => {
      if (this.ws !== ws) return;
      logger.warn(`Takaro WebSocket closed code=${code} reason=${reason.toString()}`);
      this.ws = null;
      this.isIdentified = false;
      this.gameServerId = null;
      this.emit('disconnected');
      this.scheduleReconnect();
    });
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

  send(message: WsMessage): boolean {
    if (!this.ws || this.ws.readyState !== WebSocket.OPEN) {
      logger.warn(`Cannot send ${message.type}: Takaro WebSocket is not open`);
      return false;
    }
    this.ws.send(JSON.stringify(message));
    return true;
  }

  sendResponse(requestId: string, payload: unknown): boolean {
    return this.send(createResponse(requestId, payload));
  }

  sendError(requestId: string, error: string): boolean {
    return this.send(createErrorResponse(requestId, error));
  }

  sendGameEvent(type: GameEventType, data: unknown): boolean {
    return this.send(createGameEvent(type, data));
  }

  shutdown(): void {
    this.shuttingDown = true;
    if (this.reconnectTimer) clearTimeout(this.reconnectTimer);
    this.reconnectTimer = null;
    const ws = this.ws;
    this.ws = null;
    ws?.close();
  }

  private handleMessage(raw: string): void {
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
        this.send({ type: 'pong' });
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
