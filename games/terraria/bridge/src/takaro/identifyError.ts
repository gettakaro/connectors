/**
 * What the bridge logs about a rejected identify. Never the whole object: Takaro's error can
 * carry the request it made on the bridge's behalf, auth header included.
 */
export function identifyErrorSummary(payload: unknown): string {
  if (typeof payload === 'string') return payload;
  if (!payload || typeof payload !== 'object') return String(payload);
  const { name, message, http, status } = payload as Record<string, unknown>;
  const code = http ?? status;
  const text = [name, message].filter((part) => typeof part === 'string').join(': ') || 'unknown error';
  return text + (typeof code === 'number' ? ` (http ${code})` : '');
}

/** The message alone, for the banner. */
export function identifyErrorText(payload: unknown): string {
  const message = (payload as { message?: unknown } | null)?.message;
  return typeof message === 'string' ? message : identifyErrorSummary(payload);
}
