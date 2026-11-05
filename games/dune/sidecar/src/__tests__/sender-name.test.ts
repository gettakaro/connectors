import { describe, expect, it } from 'vitest';
import { renderChatText, resolveSenderName } from '../dune/chatSender.js';
import { loadConfig } from '../dune/config.js';
import { MemoryCursorStore } from '../dune/cursorStore.js';
import { EventPoller } from '../dune/eventPoller.js';
import { mapPluginEvent } from '../dune/mapping.js';
import { buildChatPayload, parseChatMessage, redactChat } from '../dune/rmq.js';
import { MockBattlegroup } from '../testing/mockBattlegroup.js';
import type { PluginEventsResponse } from '../dune/types.js';
import { harness } from './helpers.js';

const FLS = '6FF6498F4074E3DE';

function chatOf(h: Awaited<ReturnType<typeof harness>>): Record<string, unknown> {
  const body = JSON.parse(h.battlegroup.publishes.at(-1)!.body) as Record<string, string>;
  return JSON.parse(body.Content) as Record<string, unknown>;
}

/** The chat line's text wherever the wire variant nests it. */
function textOf(inner: Record<string, unknown>): string {
  return JSON.stringify(inner.m_Message);
}

describe('server chat name resolution', () => {
  it('senderNameOverride wins, then the configured name, then no name; empty or whitespace is unset', () => {
    expect(resolveSenderName('Shop', 'Arrakis')).toBe('Shop');
    expect(resolveSenderName('  Shop  ', 'Arrakis')).toBe('Shop');
    expect(resolveSenderName(undefined, 'Arrakis')).toBe('Arrakis');
    // Takaro modules send explicit JSON null for optional arguments.
    expect(resolveSenderName(null, 'Arrakis')).toBe('Arrakis');
    expect(resolveSenderName('', 'Arrakis')).toBe('Arrakis');
    expect(resolveSenderName('   ', ' Arrakis ')).toBe('Arrakis');
    expect(resolveSenderName(undefined, '')).toBeNull();
    expect(resolveSenderName('  ', '   ')).toBeNull();
    expect(resolveSenderName(42, undefined)).toBeNull();
  });

  it('renders "<name>: <message>" with a name and the message unchanged without one', () => {
    expect(renderChatText('Arrakis', 'hello')).toBe('Arrakis: hello');
    expect(renderChatText(null, 'hello')).toBe('hello');
  });

  it('the config value comes from TAKARO_SENDER_NAME or TAKARO_SERVER_CHAT_NAME, trimmed, and is empty by default', () => {
    const base = { DUNE_PG_URL: 'postgres://x/y' };
    expect(loadConfig(base).senderName).toBe('');
    expect(loadConfig({ ...base, TAKARO_SENDER_NAME: ' Arrakis ' }).senderName).toBe('Arrakis');
    expect(loadConfig({ ...base, TAKARO_SERVER_CHAT_NAME: 'Arrakis' }).senderName).toBe('Arrakis');
    expect(loadConfig({ ...base, TAKARO_SENDER_NAME: '  ', TAKARO_SERVER_CHAT_NAME: 'Arrakis' }).senderName).toBe('Arrakis');
  });
});

describe('server chat name on every send path', () => {
  it('a global chat line carries the name in front of the text', async () => {
    const h = await harness({ senderName: 'Arrakis' });
    await h.adapter.handleAction('sendMessage', { message: 'to everyone' });
    expect(textOf(chatOf(h))).toContain('Arrakis: to everyone');
  });

  it('a whisper carries the name in front of the text, and the recipient stays in m_UserNameTo', async () => {
    const h = await harness({ senderName: 'Arrakis' });
    const out = await h.adapter.handleAction('sendMessage', { message: 'hi', opts: { recipient: { gameId: FLS } } });
    expect(out).toMatchObject({ channel: 'whisper', sender: 'Arrakis' });
    const inner = chatOf(h);
    expect(textOf(inner)).toContain('Arrakis: hi');
    expect(inner.m_UserNameTo).toBe('Tester');
    expect((inner.m_SpoofedUserNameFrom as Record<string, string>).m_UnlocalizedName).toBe('Arrakis');
  });

  it('senderNameOverride wins over the configured name on chat and whisper', async () => {
    const h = await harness({ senderName: 'Arrakis' });
    await h.adapter.handleAction('sendMessage', { message: 'deal', opts: { senderNameOverride: 'Shop' } });
    expect(textOf(chatOf(h))).toContain('Shop: deal');
    await h.adapter.handleAction('sendMessage', { message: 'psst', opts: { recipient: { gameId: FLS }, senderNameOverride: 'Shop' } });
    expect(textOf(chatOf(h))).toContain('Shop: psst');
    expect(textOf(chatOf(h))).not.toContain('Arrakis');
  });

  it('with no name the text is exactly the message, as before', async () => {
    const h = await harness();
    const out = await h.adapter.handleAction('sendMessage', { message: 'plain', opts: { senderNameOverride: '  ' } });
    expect(out).toMatchObject({ sender: null });
    const inner = chatOf(h);
    expect(textOf(inner)).toContain('"plain"');
    expect(textOf(inner)).not.toContain(': plain');
    expect((inner.m_SpoofedUserNameFrom as Record<string, string>).m_UnlocalizedName).toBe('Takaro');
  });

  it('a broadcast panel is titled with the name and keeps the body plain; without a name it is titled Takaro', async () => {
    const h = await harness({ globalMessageMode: 'broadcast', senderName: 'Arrakis' });
    await h.adapter.handleAction('sendMessage', { message: 'to everyone' });
    const titled = h.gmSent().at(-1)!.BroadcastPayload as { LocalizedText: { Title: string; Body: string }[] };
    expect(titled.LocalizedText[0]).toEqual({ Key: 'en', Title: 'Arrakis', Body: 'to everyone' });

    const plain = await harness({ globalMessageMode: 'broadcast' });
    await plain.adapter.handleAction('sendMessage', { message: 'to everyone' });
    const untitled = plain.gmSent().at(-1)!.BroadcastPayload as { LocalizedText: { Title: string; Body: string }[] };
    expect(untitled.LocalizedText[0]).toEqual({ Key: 'en', Title: 'Takaro', Body: 'to everyone' });
  });

  it('our own named lines coming back on chat.intercept are not echoed as chat-message', async () => {
    const h = await harness({ senderName: 'Arrakis' });
    await h.adapter.handleAction('sendMessage', { message: 'loop?' });
    const echoed = parseChatMessage({
      content: Buffer.from(JSON.stringify(MockBattlegroup.chatBody(chatOf(h))), 'utf8'),
      fields: { routingKey: 'chat.map' },
      properties: { userId: 'fls' },
    });
    expect(echoed?.msg).toBe('Arrakis: loop?');
    expect(h.rmq.isOwnMessage(echoed!)).toBe(true);
  });
});

/**
 * The optional native plugin is the only event source with a monotonic seq, so it is the only one with a persisted
 * cursor. These are the rules that keep a sidecar restart from either replaying or silently dropping its events.
 */
describe('plugin event cursor', () => {
  function poller(responses: PluginEventsResponse[], emit: (type: string, data: unknown, seq?: number) => boolean | 'queued') {
    const store = new MemoryCursorStore();
    let call = 0;
    const p = new EventPoller({
      getEvents: async () => responses[Math.min(call++, responses.length - 1)],
      emit: (type, data, seq) => emit(type, data, seq),
      store,
    });
    return { p, store };
  }

  it('advances the persisted cursor only past DELIVERED events, never past a queued one', async () => {
    const seen: number[] = [];
    let deliver = true;
    const { p, store } = poller(
      [
        {
          bootId: 'boot-1',
          seq: 3,
          events: [
            { seq: 1, type: 'entity-killed', data: { player: { flsId: FLS, characterName: 'H' }, entity: 'Sandworm' } },
            { seq: 2, type: 'entity-killed', data: { player: { flsId: FLS, characterName: 'H' }, entity: 'Sandworm' } },
            { seq: 3, type: 'entity-killed', data: { player: { flsId: FLS, characterName: 'H' }, entity: 'Sandworm' } },
          ],
        },
      ],
      (_t, _d, seq) => {
        seen.push(seq ?? -1);
        return deliver ? true : 'queued';
      },
    );
    deliver = false;
    await p.pollOnce();
    expect(seen).toEqual([1, 2, 3]);
    expect(p.scanCursor()).toBe(3); // read from the ring
    expect(p.cursor()).toBe(0); // but nothing is PROVEN delivered
    expect(store.state.seq).toBe(0);

    p.markDelivered(2);
    expect(p.cursor()).toBe(2);
    expect(store.state.seq).toBe(2);
    // The cursor never goes backwards.
    p.markDelivered(1);
    expect(p.cursor()).toBe(2);
  });

  it('a new bootId means the map process restarted: start over from its beginning', async () => {
    const seen: number[] = [];
    const { p } = poller(
      [
        { bootId: 'boot-1', seq: 1, events: [{ seq: 1, type: 'log', data: { msg: 'first process' } }] },
        { bootId: 'boot-2', seq: 1, events: [{ seq: 1, type: 'log', data: { msg: 'second process' } }] },
        { bootId: 'boot-2', seq: 1, events: [{ seq: 1, type: 'log', data: { msg: 'second process' } }] },
      ],
      (_t, _d, seq) => {
        seen.push(seq ?? -1);
        return true;
      },
    );
    await p.pollOnce();
    expect(p.cursor()).toBe(1);
    await p.pollOnce(); // bootId changed → reset and re-read
    expect(seen).toEqual([1, 1]);
    expect(p.cursor()).toBe(1);
  });

  it('entity-killed always carries a string weapon — Takaro drops the event when it is absent', () => {
    const mapped = mapPluginEvent({ type: 'entity-killed', data: { player: { flsId: FLS, characterName: 'H' }, entity: 'Sandworm' } });
    expect(mapped?.data).toMatchObject({ entity: 'Sandworm', weapon: '' });
    expect(typeof mapped?.data.weapon).toBe('string');
  });

  it('a plugin death with a real attacker keeps it; without one it explains the cause instead', () => {
    const pvp = mapPluginEvent({
      type: 'player-death',
      data: { player: { flsId: FLS, characterName: 'H' }, attacker: { flsId: 'AAAA1111BBBB2222', characterName: 'Chani' } },
    });
    expect(pvp?.data.attacker).toMatchObject({ gameId: 'AAAA1111BBBB2222', name: 'Chani' });
    expect(pvp?.data.msg).toBeUndefined();

    const pve = mapPluginEvent({ type: 'player-death', data: { player: { flsId: FLS, characterName: 'H' }, killerEntity: 'Sandworm' } });
    expect(pve?.data.msg).toBe('H was killed by Sandworm');

    const fall = mapPluginEvent({ type: 'player-death', data: { player: { flsId: FLS, characterName: 'H' }, cause: 'falling' } });
    expect(fall?.data.attacker).toBeUndefined();
    expect(fall?.data.msg).toBe('H died (falling)');
  });

  it('an unknown plugin event type is dropped, not forwarded as garbage', () => {
    expect(mapPluginEvent({ type: 'spice-harvested', data: {} })).toBeNull();
  });
});

/**
 * 2026-09-21, from the client: our global lines DO render in the in-game chat window, but the sender read as an empty
 * `[]`. The payload carried `m_UserNameTo` and nothing for the sender, so the field the client formats the prefix from
 * was absent.
 */
describe('the sender name is actually carried on the wire', () => {
  const wire = {
    timestampField: 'm_TimeStamp',
    timestampFormat: 'ue' as const,
    channelEnumForm: 'short' as const,
    contentKey: 'Content' as const,
    contentType: 'Content',
    amqpType: 'text_chat',
    deliveryMode: 1 as const,
    bodyType: 'TextChat',
    senderNameField: 'm_UserNameFrom',
    whisperChannelType: 'Map',
  };

  it('m_UserNameFrom is sent alongside the spoof pair, as the counterpart of m_UserNameTo', () => {
    const payload = buildChatPayload({ msg: 'hello', senderName: 'Takaro', senderFuncomId: 'ADMIN#00001', channel: 'Map' }, wire);
    expect(payload.m_UserNameFrom).toBe('Takaro');
    expect(payload.m_UserNameTo).toBe('');
    // The spoof pair stays: it is the other candidate and costs nothing to send.
    expect(payload.m_bUseSpoofedUserName).toBe(true);
    expect(payload.m_SpoofedUserNameFrom).toEqual({ m_TableId: '', m_Key: '', m_UnlocalizedName: 'Takaro' });
  });

  it('the field name is a switch, so a captured real payload can rename it without a rebuild', () => {
    const renamed = buildChatPayload({ msg: 'x', senderName: 'Takaro', senderFuncomId: 'A#1', channel: 'Map' }, { ...wire, senderNameField: 'm_CharacterNameFrom' });
    expect(renamed.m_CharacterNameFrom).toBe('Takaro');
    expect(renamed.m_UserNameFrom).toBeUndefined();

    const omitted = buildChatPayload({ msg: 'x', senderName: 'Takaro', senderFuncomId: 'A#1', channel: 'Map' }, { ...wire, senderNameField: '' });
    expect('m_UserNameFrom' in omitted).toBe(false);
  });

  it('the sender-name field stays off by default and is a switch', () => {
    // Measured live 2026-09-21 (L6b-3): a real player-authored frame has NO sender-name field, and sending
    // `m_UserNameFrom` left the in-game prefix an empty `[]` exactly as omitting it does. So the default is now
    // "send nothing", and the switch survives only so a future game build can be retried without a rebuild.
    expect(loadConfig({ DUNE_PG_URL: 'postgres://x/y' }).chatWire.senderNameField).toBe('');
    expect(
      loadConfig({ DUNE_PG_URL: 'postgres://x/y', DUNE_CHAT_SENDER_NAME_FIELD: 'm_UserNameFrom' }).chatWire.senderNameField,
    ).toBe('m_UserNameFrom');
  });

  it('a wire dump of an inbound frame never leaks an auth token', () => {
    const raw = JSON.stringify({ AuthToken: 'super-secret-value', Content: '{"m_Message":{}}' });
    expect(redactChat(raw)).not.toMatch(/super-secret-value/);
    expect(redactChat(raw)).toMatch(/<redacted>/);
  });
});
