import { describe, expect, it } from 'vitest';
import { CONNECTOR_IDENTITY } from '../takaro/gameProfile.js';
import { createErrorResponse, createGameEvent, createIdentify, createResponse, normalizeArgs, parseTakaroRequest } from '../takaro/protocol.js';

describe('protocol', () => {
  it('normalizes args [] / {} / JSON string / empty / invalid', () => {
    expect(normalizeArgs([])).toEqual({});
    expect(normalizeArgs({})).toEqual({});
    expect(normalizeArgs(null)).toEqual({});
    expect(normalizeArgs('')).toEqual({});
    expect(normalizeArgs('[]')).toEqual({});
    expect(normalizeArgs('{}')).toEqual({});
    expect(normalizeArgs('{"gameId":"1"}')).toEqual({ gameId: '1' });
    expect(normalizeArgs('not json')).toEqual({});
    expect(normalizeArgs({ a: 1 })).toEqual({ a: 1 });
  });

  it('parses requests and rejects malformed ones', () => {
    expect(parseTakaroRequest({ type: 'request', requestId: 'r1', payload: { action: 'getPlayers', args: '[]' } })).toEqual({ requestId: 'r1', action: 'getPlayers', args: {} });
    expect(() => parseTakaroRequest({ type: 'request', payload: { action: 'x' } })).toThrow(/requestId/);
    expect(() => parseTakaroRequest({ type: 'request', requestId: 'r', payload: {} })).toThrow(/action/);
    expect(() => parseTakaroRequest({ type: 'ping' })).toThrow(/not a request/);
  });

  it('builds frames', () => {
    const identity = { game: 'dune', connector: CONNECTOR_IDENTITY };
    expect(createIdentify({ identityToken: 'id', registrationToken: 'reg', serverName: 'S' })).toEqual({
      type: 'identify',
      payload: { identityToken: 'id', registrationToken: 'reg', name: 'S', ...identity },
    });
    expect(createIdentify({ identityToken: 'id', registrationToken: '' })).toEqual({ type: 'identify', payload: { identityToken: 'id', ...identity } });
    // getPlayer for an unknown player answers an explicit JSON null; `{}` fails Takaro's IGamePlayer validation (F7).
    expect(createResponse('r', null)).toEqual({ type: 'response', requestId: 'r', payload: null });
    expect(createResponse('r', undefined)).toEqual({ type: 'response', requestId: 'r', payload: {} });
    expect(createResponse('r', [1])).toEqual({ type: 'response', requestId: 'r', payload: [1] });
    expect(createErrorResponse('r', 'boom')).toEqual({ type: 'response', requestId: 'r', error: 'boom' });
    expect(createGameEvent('log', { msg: 'x' })).toEqual({ type: 'gameEvent', payload: { type: 'log', data: { msg: 'x' } } });
  });
});
