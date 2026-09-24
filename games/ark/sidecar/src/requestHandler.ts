import type { ArkAdapter } from './adapter.js';
import { logger } from './logger.js';
import { createResponse, GAME_SERVER_ACTIONS, parseTakaroRequest, type WsMessage } from './takaro/protocol.js';

/**
 * Takaro's Generic connector resolves void actions from any matched frame's
 * payload, without inspecting its type or top-level error. A failed action
 * therefore must leave its request unanswered so Takaro's bounded request
 * timeout rejects it. Closing the socket would also reject unrelated work.
 */
export async function handleTakaroRequest(
  message: WsMessage,
  adapter: Pick<ArkAdapter, 'handle'>,
  send: (response: WsMessage) => boolean,
): Promise<void> {
  if (!message.requestId) return;
  let action = 'unknown';
  let phase = 'parse';
  try {
    const request = parseTakaroRequest(message);
    if ((GAME_SERVER_ACTIONS as readonly string[]).includes(request.action)) action = request.action;
    phase = 'action';
    const payload = await adapter.handle(request.action, request.args, request.requestId);
    send(createResponse(request.requestId, payload));
  } catch {
    const requestId = /^[A-Za-z0-9_-]{1,128}$/.test(message.requestId) ? message.requestId : 'invalid';
    logger.warn(JSON.stringify({ event: 'takaro-request-failed', requestId, action,
      category: phase === 'parse' ? 'invalid-request' : 'action-failed', response: 'withheld' }));
  }
}
