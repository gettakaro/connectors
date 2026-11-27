import EventEmitter from 'node:events';
import WebSocket from 'ws';
import { DEBUG, logger, truncate } from '../logger.js';
import type { GameEventType, IdentifyPayload, WsMessage } from './protocol.js';

export class TakaroWsClient extends EventEmitter {
  private ws: WebSocket | null = null;
  private gameServerId: string | null = null;
  private reconnectMs = 5000;
  private shuttingDown = false;
  private connectedFlag = false;

  constructor(
    private url: string,
    private identify: IdentifyPayload,
  ) {
    super();
  }

  connect(): void {
    logger.info(`WS connect ${this.url}`);
    this.ws = new WebSocket(this.url);

    this.ws.on('open', () => {
      this.connectedFlag = true;
      logger.info('WS open; sending identify');
      this.send({ type: 'identify', payload: this.identify });
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

  send(msg: WsMessage): void {
    if (!this.ws || this.ws.readyState !== WebSocket.OPEN) {
      logger.warn(`Cannot send ${msg.type}: WS not open`);
      return;
    }
    const frame = JSON.stringify(msg);
    if (DEBUG) logger.debug(`WS SEND ${truncate(frame)}`);
    this.ws.send(frame);
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

  private handle(msg: WsMessage): void {
    switch (msg.type) {
      case 'connected':
        logger.info('Server confirmed WebSocket connection');
        break;
      case 'identifyResponse': {
        const p = msg.payload as { gameServerId?: string; error?: unknown } | undefined;
        if (p?.error) {
          const err = p.error as { message?: string; name?: string; http?: number; details?: unknown };
          const summary = err.message || JSON.stringify(p.error);
          logger.error(`Identification failed: ${summary} [name=${err.name ?? '?'} http=${err.http ?? '?'}]`);
          if (err.details) logger.error(`Error details: ${JSON.stringify(err.details)}`);
          break;
        }
        if (p?.gameServerId) {
          this.gameServerId = p.gameServerId;
          logger.info(`Identified as gameServerId=${p.gameServerId}`);
          this.emit('identified', p.gameServerId);
        }
        break;
      }
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
