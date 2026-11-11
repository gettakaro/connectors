import { logger } from '../logger.js';

/**
 * The lines that say what the Takaro websocket is doing.
 *
 * They are not decoration. `takaro-maint verify` reads this bridge's log for
 * "Identified successfully" to prove the handshake, and counts it a second time to prove
 * a reconnect, so the leading text of both lines is a contract with the harness and not
 * something to reword. Everything else the bridge logs is about one request.
 */
export const IDENTIFIED_PREFIX = 'Identified successfully with Takaro';
export const DISCONNECTED_LINE = 'Takaro connection closed; reconnecting';

export const identifiedLine = (gameServerId?: string): string =>
  `${IDENTIFIED_PREFIX} (gameServerId=${gameServerId ?? '<unknown>'})`;

export interface ClosedInfo {
  code: number;
  reason: string;
  stable: boolean;
}

export interface ReconnectInfo {
  delayMs: number;
  failures: number;
}

/** Anything that reports the connection state the way the Takaro client does. */
export interface ConnectionEvents {
  on(event: 'identified', listener: (gameServerId?: string) => void): unknown;
  on(event: 'disconnected', listener: () => void): unknown;
  on(event: 'closed', listener: (info: ClosedInfo) => void): unknown;
  on(event: 'reconnectScheduled', listener: (info: ReconnectInfo) => void): unknown;
  on(event: 'linkDead', listener: (silentMs: number) => void): unknown;
}

export const closedLine = ({ code, reason, stable }: ClosedInfo): string =>
  `Takaro socket closed (code ${code}${reason ? `, ${reason}` : ''}, ${stable ? 'after a stable session' : 'before the session was stable'})`;

export const reconnectLine = ({ delayMs, failures }: ReconnectInfo): string =>
  `Takaro reconnect in ${delayMs} ms (${failures === 0 ? 'fast retry after a stable session' : `failure streak ${failures}`})`;

export const linkDeadLine = (silentMs: number): string =>
  `Takaro link silent for ${Math.round(silentMs / 1000)} s; closing it as dead`;

/** Wires the connection lines onto a live client. Added beside the bridge's own handlers. */
export function logConnectionState(takaro: ConnectionEvents, log = logger): void {
  takaro.on('identified', (gameServerId?: string) => log.info(identifiedLine(gameServerId)));
  takaro.on('closed', (info: ClosedInfo) => log.warn(closedLine(info)));
  takaro.on('disconnected', () => log.warn(DISCONNECTED_LINE));
  takaro.on('reconnectScheduled', (info: ReconnectInfo) => log.info(reconnectLine(info)));
  takaro.on('linkDead', (silentMs: number) => log.warn(linkDeadLine(silentMs)));
}
