import { randomUUID } from 'node:crypto';
import EventEmitter from 'node:events';
import WebSocket from 'ws';
import { logger } from '../logger.js';
import {
  createErrorResponse,
  createGameEvent,
  createIdentify,
  createResponse,
  errorForVersion,
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
  /**
   * The Capability Manifest. When set, identify offers protocol versions [0, 1] with it; Takaro picks the highest it
   * speaks, so an older Takaro keeps the connection on version 0. Unset offers version 0 only.
   */
  manifest?: () => unknown[];
}

/**
 * Outbound WebSocket to Takaro. Emits: 'identified' (gameServerId|null), 'request' (WsMessage), 'disconnected',
 * 'rejected' (reason — Takaro refused a frame we pushed, i.e. an event lost to DTO validation), 'confirmed' (sendId) — every gameEvent up to and including that sendId is PROVEN to have reached Takaro's kernel.
 *
 * Why a pong and not "any inbound frame" (finding F20): during a `rig-outage` our egress is REJECTed, so `ws.send()`
 * still succeeds into the local kernel buffers while nothing reaches the peer, and the peer may still be able to send
 * US frames. An inbound `request`/`identifyResponse` therefore proves only that THEY are alive, never that our bytes
 * arrived. A pong does: the peer only emits it after processing our ping frame, and TCP/WebSocket framing is ordered,
 * so every byte we wrote before that ping was received first. Hence the confirmation anchor is "a pong for a ping that
 * was sent AFTER the event".
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
  private readonly manifest: (() => unknown[]) | null;
  /** The version Takaro chose in identifyResponse (0 until identified). */
  private protocolVersion = 0;
  /**
   * Version 1 event stream: one per process. `seq` is the sendId, which only ever grows, so it stays strictly
   * increasing across reconnects as the protocol requires.
   */
  private readonly streamId = randomUUID();

  private configHint: ConfigHint = { file: '', hasFile: false };

  constructor(
    private url: string,
    private identifyConfig: IdentifyConfig,
    options: TakaroClientOptions = {},
  ) {
    super();
    this.baseReconnectMs = options.baseReconnectMs ?? 2000;
    this.maxReconnectMs = options.maxReconnectMs ?? 60000;
    this.pingIntervalMs = options.pingIntervalMs ?? 5_000;
    this.idleTimeoutMs = options.idleTimeoutMs ?? 20_000;
    this.maxMissedPongs = options.maxMissedPongs ?? 2;
    this.maxBufferedBytes = options.maxBufferedBytes ?? 1024 * 1024;
    this.manifest = options.manifest ?? null;
  }

  /** The protocol version negotiated on the current connection (0 until identified, and on an older Takaro). */
  getProtocolVersion(): number {
    return this.protocolVersion;
  }

  /** Where the operator edits the settings; named in the banners. */
  setConfigHint(hint: ConfigHint): void {
    this.configHint = hint;
  }

  connect(): void {
    if (this.shuttingDown) return;
    this.reconnectTimer = null;
    // Without a registration token Takaro rejects every identify, so do not even open a socket; the settings watcher
    // calls `reconfigure` the moment a token is saved.
    if (!this.identifyConfig.registrationToken) {
      logBanner(logger.warn, [
        'TAKARO_REGISTRATION_TOKEN not set, the server is not connected to Takaro.',
        ...this.whereToFix('Paste the registration token from Takaro into'),
      ]);
      return;
    }
    logger.info(`Connecting to Takaro at ${this.url}`);
    const ws = new WebSocket(this.url);
    this.ws = ws;

    ws.on('open', () => {
      if (this.ws !== ws) return;
      logger.info('Takaro WebSocket open, sending identify');
      this.lastActivity = Date.now();
      this.outstandingPings = [];
      this.startWatchdog(ws);
      const v1 = this.manifest ? { protocolVersions: [0, 1], capabilities: this.manifest() } : undefined;
      this.sendFrame(ws, createIdentify(this.identifyConfig, v1));
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
    ws.on('error', (err) => logger.error(`Takaro WebSocket error: ${err.message}`));
    ws.on('close', (code, reason) => {
      if (this.ws !== ws) return;
      logger.warn(`Takaro WebSocket closed code=${code} reason=${reason.toString()}`);
      this.stopWatchdog();
      this.outstandingPings = [];
      this.ws = null;
      this.isIdentified = false;
      this.gameServerId = null;
      this.protocolVersion = 0;
      this.emit('disconnected');
      this.scheduleReconnect();
    });
  }

  /** Id of the last gameEvent written to the socket (0 = none this process). */
  lastSendId(): number {
    return this.sendId;
  }

  /** Highest sendId proven to have reached Takaro. */
  lastConfirmedId(): number {
    return this.confirmedId;
  }

  /** Register a callback fired when delivery of everything up to `sendId` is proven. */
  onConfirmed(cb: (sendId: number) => void): void {
    this.on('confirmed', cb);
  }

  private notePong(payload: string): void {
    if (!this.outstandingPings.length) return;
    // On version 1 Takaro acknowledges events itself (eventAck), which also proves it accepted them. A pong only
    // proves the bytes arrived, so it must not release an event Takaro may still reject.
    if (this.protocolVersion === 1) {
      this.outstandingPings = [];
      return;
    }
    const idx = this.outstandingPings.findIndex((p) => String(p.id) === payload);
    // A server that does not echo the ping payload still proves the oldest outstanding ping arrived: match FIFO, which
    // confirms the SMALLEST upTo and is therefore never optimistic.
    const hit = this.outstandingPings[idx >= 0 ? idx : 0];
    this.outstandingPings = this.outstandingPings.slice((idx >= 0 ? idx : 0) + 1);
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
    if (message.type === 'response' && message.error !== undefined) {
      message = { ...message, error: errorForVersion(message.error, this.protocolVersion) };
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

  sendError(requestId: string, error: string): boolean {
    return this.send(createErrorResponse(requestId, error));
  }

  /**
   * Replaces the Capability Manifest on a version 1 connection. A version 0 connection has none to replace, and an
   * identify always carries the current manifest.
   */
  sendCapabilities(): boolean {
    if (!this.manifest || !this.isIdentified || this.protocolVersion !== 1) return false;
    return this.send({ type: 'capabilities', requestId: randomUUID(), payload: { capabilities: this.manifest() } });
  }

  /**
   * Writes a gameEvent. `true` means only that the bytes were accepted by the local socket — NOT that Takaro got them
   * (F20). The caller must keep the event until a 'confirmed' event covers `lastSendId()`.
   */
  sendGameEvent(type: GameEventType, data: unknown): boolean {
    const frame = createGameEvent(type, data);
    if (this.protocolVersion === 1) {
      // Version 1: numbered on this process's stream; Takaro acknowledges up to a seq (eventAck).
      frame.payload = { ...(frame.payload as object), streamId: this.streamId, seq: this.sendId + 1 };
    }
    const ok = this.send(frame);
    if (ok) this.sendId += 1;
    return ok;
  }

  /**
   * Applies changed settings: drops the current socket (if any) and connects again at once, skipping whatever backoff
   * a rejected token had built up. Safe against the old socket's late `close`/`message` callbacks (they check
   * `this.ws !== ws`) and against a pending reconnect timer (cleared here) and shutdown (checked first).
   */
  reconfigure(url: string, identifyConfig: IdentifyConfig): void {
    if (this.shuttingDown) return;
    this.url = url;
    this.identifyConfig = identifyConfig;
    if (this.reconnectTimer) clearTimeout(this.reconnectTimer);
    this.reconnectTimer = null;
    this.reconnectAttempts = 0;
    const old = this.ws;
    if (old) {
      this.ws = null;
      this.stopWatchdog();
      this.outstandingPings = [];
      this.isIdentified = false;
      this.gameServerId = null;
      this.protocolVersion = 0;
      try {
        old.close(1000, 'Takaro settings changed');
      } catch {
        old.terminate();
      }
      // The old socket's own `close` is ignored (it is no longer `this.ws`), so report the drop here.
      this.emit('disconnected');
    }
    logger.info('Takaro settings changed, reconnecting now');
    this.connect();
  }

  private whereToFix(lead: string): string[] {
    if (this.configHint.hasFile) {
      return [`${lead} ${this.configHint.file}`, 'and save it. The sidecar connects within a few seconds, no restart needed.'];
    }
    return [
      `${lead} TAKARO_REGISTRATION_TOKEN in ${this.configHint.file || 'the .env file'}`,
      '(that file does not exist yet, or set the variable and restart the sidecar).',
      'Once the file exists, saving it is enough: no restart needed.',
    ];
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
        const payload = (message.payload ?? {}) as { error?: unknown; gameServerId?: string; server?: { id?: string }; protocolVersion?: number };
        // Version 1 puts a refused identify in a top-level error and then closes the socket (1008).
        const identifyError = payload.error ?? message.error;
        if (identifyError) {
          const reason = describeTakaroError(identifyError);
          if (isNameConflict(identifyError, reason)) {
            logBanner(logger.error, [
              `Takaro rejected identify: ${reason}.`,
              `Takaro already has a game server named '${this.identifyConfig.serverName ?? this.identifyConfig.identityToken}'.`,
              ...this.whereToFix('Set a unique TAKARO_SERVER_NAME in'),
              'Until then the sidecar retries with backoff.',
            ]);
            this.ws?.close();
            break;
          }
          logBanner(logger.error, [
            `Takaro rejected identify: ${reason}.`,
            ...this.whereToFix('Check TAKARO_REGISTRATION_TOKEN (and TAKARO_IDENTITY_TOKEN) in'),
            'Until then the sidecar retries with backoff.',
          ]);
          // Takaro keeps a rejected socket open; close it and retry with backoff. A saved fix skips the backoff.
          this.ws?.close();
          break;
        }
        this.reconnectAttempts = 0;
        this.isIdentified = true;
        this.gameServerId = payload.gameServerId ?? payload.server?.id ?? null;
        this.protocolVersion = payload.protocolVersion === 1 ? 1 : 0;
        // On version 1 the heartbeat no longer confirms delivery, so pings already out must not either.
        if (this.protocolVersion === 1) this.outstandingPings = [];
        logger.info(
          `Identified with Takaro${this.gameServerId ? ` (gameServerId=${this.gameServerId})` : ''}, protocol version ${this.protocolVersion}`,
        );
        this.emit('identified', this.gameServerId);
        break;
      }
      case 'request':
        this.emit('request', message);
        break;
      case 'eventAck':
        this.acknowledge(message.payload);
        break;
      case 'eventError': {
        // Takaro refused one numbered event (DTO validation). It is acknowledged too, so it is not resent.
        const reason = JSON.stringify({ ...(message.payload as object), error: message.error });
        logger.error(`Takaro rejected event: ${describeTakaroError(message.error ?? message.payload ?? {})} ${reason.slice(0, 300)}`);
        this.emit('rejected', reason);
        break;
      }
      case 'capabilitiesResponse':
        if (message.error) logger.error(`Takaro rejected the capability manifest: ${describeTakaroError(message.error)}`);
        else logger.info('Takaro accepted the capability manifest');
        break;
      case 'ping':
        if (this.ws) this.sendFrame(this.ws, { type: 'pong' });
        break;
      case 'error': {
        // An `error` frame with no `requestId` is Takaro refusing something WE pushed — in practice a gameEvent that
        // failed DTO validation. It is not a response to a request, so nothing surfaces it unless we count it here;
        // a silently rejected event is exactly how the `entityCode` whitelist bug survived a live kill.
        const reason = JSON.stringify(message.payload ?? message.error ?? {});
        logger.error(`Takaro error: ${describeTakaroError(message.payload ?? message.error ?? {})}`);
        if (!message.requestId) this.emit('rejected', reason);
        break;
      }
      default:
        logger.debug(`Unhandled Takaro message type=${message.type}`);
    }
  }

  /** Version 1 `eventAck { streamId, upTo }`: every event up to that seq reached Takaro and was processed. */
  private acknowledge(payload: unknown): void {
    const { streamId, upTo } = (payload ?? {}) as { streamId?: unknown; upTo?: unknown };
    if (streamId !== this.streamId || typeof upTo !== 'number') return;
    const confirmed = Math.min(upTo, this.sendId);
    if (confirmed > this.confirmedId) {
      this.confirmedId = confirmed;
      this.emit('confirmed', this.confirmedId);
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

export interface ConfigHint {
  /** The file the operator edits, as they see it (the host path in Docker). */
  file: string;
  /** Whether that file exists, i.e. whether saving it takes effect without a restart. */
  hasFile: boolean;
}

const BANNER_RULE = '*************************************************************************';

/** A config problem that stops the connection, so it must stand out in `docker logs` / a panel console. */
export function logBanner(log: (message: string) => void, lines: string[]): void {
  log(BANNER_RULE);
  for (const line of lines) log(`  ${line}`);
  log(BANNER_RULE);
}

/**
 * Only the name, message and HTTP status of a Takaro error. Takaro's identify errors can carry its internal request,
 * including an `x-takaro-token` JWT; none of that is ever logged.
 */
export function describeTakaroError(error: unknown): string {
  if (typeof error === 'string') return clip(error);
  if (!error || typeof error !== 'object') return clip(String(error));
  const e = error as Record<string, unknown>;
  const nested = (e.response ?? {}) as Record<string, unknown>;
  const status = e.status ?? e.statusCode ?? e.httpStatus ?? nested.status;
  const parts = [e.name, e.message ?? e.error ?? e.code]
    .filter((v) => typeof v === 'string' && v.trim() !== '')
    .map((v) => String(v));
  if (typeof status === 'number' || typeof status === 'string') parts.push(`HTTP ${status}`);
  return clip(parts.length ? parts.join(': ') : 'no reason given');
}

/** A 409: the server NAME is taken in this domain (names are unique, and stay reserved a while after a delete). */
export function isNameConflict(error: unknown, described: string): boolean {
  const e = (error && typeof error === 'object' ? error : {}) as Record<string, unknown>;
  const status = e.status ?? e.statusCode ?? e.httpStatus;
  return String(status) === '409' || /conflict|already exists|unique/i.test(described);
}

function clip(text: string): string {
  const cleaned = text.replace(/eyJ[\w-]+\.[\w-]+\.[\w-]+/g, '<redacted-jwt>');
  return cleaned.length > 300 ? `${cleaned.slice(0, 300)}…` : cleaned;
}

/** Never let a registration/identity token or a world password reach the logs. */
function redactFrame(frame: string): string {
  const redacted = frame
    .replace(/("(?:registrationToken|identityToken|token|password)"\s*:\s*")[^"]*"/gi, '$1<redacted>"')
    .replace(/(\?p=)[^\s"&]+/gi, '$1<redacted>');
  return redacted.length > 2000 ? `${redacted.slice(0, 2000)}…` : redacted;
}
