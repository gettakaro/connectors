/**
 * The sender name on every chat line the connector sends. Dune renders a server-sent chat sender as an empty `[]`
 * (it takes the label from the AMQP `user_id` of a real player account), so the name cannot be a sender field: it
 * goes in front of the text instead, and a player reads `[] <name>: <message>`.
 *
 * Takaro's `senderNameOverride` (filled from the server's `serverChatName` by newer Takaro releases) wins over the
 * configured `TAKARO_SENDER_NAME` / `TAKARO_SERVER_CHAT_NAME`; neither means no name and today's plain text.
 */

/** The name to show, or null when neither the override nor the configured name is set. */
export function resolveSenderName(override: unknown, configured: string | undefined): string | null {
  return clean(override) ?? clean(configured);
}

/** The chat text a player sees; unchanged when no name resolves. */
export function renderChatText(senderName: string | null, message: string): string {
  return senderName === null ? message : `${senderName}: ${message}`;
}

/** Title of the on-screen `ServiceBroadcast` panel, which does show a name; `Takaro` when none resolves. */
export function broadcastTitle(senderName: string | null): string {
  return senderName ?? 'Takaro';
}

function clean(name: unknown): string | null {
  if (typeof name !== 'string') return null;
  const trimmed = name.trim();
  return trimmed === '' ? null : trimmed;
}
