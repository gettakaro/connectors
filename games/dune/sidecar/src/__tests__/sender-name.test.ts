import { describe, expect, it } from 'vitest';
import { loadConfig } from '../dune/config.js';
import { MemoryCursorStore } from '../dune/cursorStore.js';
import { EventPoller } from '../dune/eventPoller.js';
import { mapPluginEvent } from '../dune/mapping.js';
import { buildChatPayload, redactChat } from '../dune/rmq.js';
import type { PluginEventsResponse } from '../dune/types.js';
import { harness } from './helpers.js';

const FLS = '6FF6498F4074E3DE';

describe('sender name', () => {
  it('falls back TAKARO_SENDER_NAME → TAKARO_SERVER_NAME → "Takaro", and a per-message override wins', async () => {
    const h = await harness();
    expect(h.adapter.senderName()).toBe('Takaro');
    expect(h.adapter.senderName('Shop')).toBe('Shop');
    // An explicit JSON null override must not blank out the configured name.
    expect(h.adapter.senderName(null)).toBe('Takaro');
  });

  it('the configured sender name is what a player sees as the chat sender', async () => {
    const h = await harness();
    await h.adapter.handleAction('sendMessage', { message: 'hi', opts: { recipient: { gameId: FLS } } });
    const body = JSON.parse(h.battlegroup.publishes.at(-1)!.body) as Record<string, string>;
    const inner = JSON.parse(body.Content) as Record<string, unknown>;
    expect(inner.m_bUseSpoofedUserName).toBe(true);
    expect((inner.m_SpoofedUserNameFrom as Record<string, string>).m_UnlocalizedName).toBe('Takaro');
    // The recipient's character name goes in m_UserNameTo, which the client renders on the whisper.
    expect(inner.m_UserNameTo).toBe('Tester');
  });

  it('a broadcast is titled with the sender name, not with a hard-coded string', async () => {
    const h = await harness({ globalMessageMode: 'broadcast' });
    await h.adapter.handleAction('sendMessage', { message: 'to everyone' });
    const payload = h.gmSent().at(-1)!.BroadcastPayload as { LocalizedText: { Title: string; Body: string }[] };
    expect(payload.LocalizedText[0]).toEqual({ Key: 'en', Title: 'Takaro', Body: 'to everyone' });
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

  it('the default sender name is Takaro, not the indistinguishable "Server"', () => {
    expect(loadConfig({ DUNE_PG_URL: 'postgres://x/y' }).senderName).toBe('Takaro');
    expect(loadConfig({ DUNE_PG_URL: 'postgres://x/y', TAKARO_SENDER_NAME: 'Arrakis' }).senderName).toBe('Arrakis');
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
