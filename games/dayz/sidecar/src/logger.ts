import { createLogger, format, transports } from 'winston';

/** DEBUG=1 turns on frame-level logging of every WS SEND/RECV and HTTP request. */
export const DEBUG = /^(1|true|yes|on)$/i.test(process.env.DEBUG || '');

export const logger = createLogger({
  level: process.env.LOG_LEVEL || (DEBUG ? 'debug' : 'info'),
  format: format.combine(
    format.timestamp({ format: 'HH:mm:ss.SSS' }),
    format.printf(({ timestamp, level, message }) => `[${timestamp}] ${level} ${message}`),
  ),
  transports: [new transports.Console()],
});

const SECRET_KEYS = ['registrationToken', 'identityToken', 'password', 'token', 'RConPassword'];

/** Replaces secret values with *** so DEBUG frame logs never leak credentials. */
export function redact(s: string): string {
  let out = s;
  for (const key of SECRET_KEYS) {
    out = out.replace(new RegExp(`("${key}"\\s*:\\s*")(?:[^"\\\\]|\\\\.)*(")`, 'gi'), '$1***$2');
  }
  return out;
}

/** Truncates a body for DEBUG frame logging so a 5 MB inventory dump cannot flood the log. */
export function truncate(value: unknown, max = 500): string {
  let s: string;
  if (typeof value === 'string') {
    s = value;
  } else {
    try {
      s = JSON.stringify(value);
    } catch {
      s = String(value);
    }
  }
  if (s === undefined) s = String(value);
  s = redact(s);
  return s.length > max ? `${s.slice(0, max)}…(${s.length} bytes)` : s;
}
