/**
 * What the bridge logs about a rejected identify. Never the whole object: Takaro's error can
 * carry the request it made on the bridge's behalf, auth header included.
 */
export function identifyErrorSummary(payload: unknown): string {
  if (typeof payload === 'string') return payload;
  if (!payload || typeof payload !== 'object') return String(payload);
  const { name, message } = payload as Record<string, unknown>;
  const code = identifyErrorStatus(payload);
  const text = [name, message].filter((part) => typeof part === 'string').join(': ') || 'unknown error';
  return text + (code === undefined ? '' : ` (http ${code})`);
}

/** The HTTP status Takaro answered with, when the error carries one (409: the name is taken). */
export function identifyErrorStatus(payload: unknown): number | undefined {
  if (!payload || typeof payload !== 'object') return undefined;
  const { http, status } = payload as Record<string, unknown>;
  const code = http ?? status;
  return typeof code === 'number' ? code : undefined;
}

/** The message alone, for the banner. */
export function identifyErrorText(payload: unknown): string {
  const message = (payload as { message?: unknown } | null)?.message;
  return typeof message === 'string' ? message : identifyErrorSummary(payload);
}
