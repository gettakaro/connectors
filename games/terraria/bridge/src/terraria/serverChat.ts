/**
 * The sender name players see on chat Takaro sends, and the text that carries it. Terraria has
 * no sender field for a message that does not come from a player slot, so the name is part of
 * the text. Every send path renders through these helpers so global and directed chat agree.
 */

/** The marker the plugin puts in front of the server log line for each chat line it delivers. */
export const TAKARO_CHAT_MARKER = 'TAKARO_CHAT';

/**
 * `opts.senderNameOverride` (Takaro's per-server chat name), else the configured
 * `serverChatName`, else no name. Both are trimmed; an empty or blank value counts as unset.
 */
export function resolveSenderName(override: unknown, configured: string | undefined): string | null {
  const fromTakaro = typeof override === 'string' ? override.trim() : '';
  if (fromTakaro) return fromTakaro;
  const fromConfig = (configured ?? '').trim();
  return fromConfig || null;
}

/** Global chat: `<name>: <message>`. */
export function renderGlobal(sender: string, message: string): string {
  return `${sender}: ${message}`;
}

/** A message to one player, in TShock's own whisper form: `<From <name>> <message>`. */
export function renderDirected(sender: string, message: string): string {
  return `<From ${sender}> ${message}`;
}
