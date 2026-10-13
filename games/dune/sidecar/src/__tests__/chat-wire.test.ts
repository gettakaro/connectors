import { describe, expect, it, vi } from 'vitest';
import { loadConfig } from '../dune/config.js';
import { buildChatBody, buildChatPayload, DuneRmq, normaliseOrigin, parseChatMessage } from '../dune/rmq.js';
import type { DuneChatMessage } from '../dune/types.js';
import { MockBattlegroup } from '../testing/mockBattlegroup.js';

const wire = loadConfig({}).chatWire;

function message(content: unknown, properties: Record<string, unknown> = {}, routingKey = 'chat.map'): Parameters<typeof parseChatMessage>[0] {
  return {
    content: Buffer.from(typeof content === 'string' ? content : JSON.stringify(content), 'utf8'),
    fields: { routingKey },
    properties,
  };
}

const INNER = {
  m_Id: 'abc-123',
  m_ChannelType: 'Map',
  m_FuncomIdFrom: 'PLAYER#12345',
  m_Message: { m_UnlocalizedMessage: 'hello arrakis' },
  m_OriginLocation: { X: 100.5, Y: 200.25, Z: 30 },
};

describe('chat.intercept parsing', () => {
  it('reads the inner JSON string under `content` (inbound spelling)', () => {
    const parsed = parseChatMessage(message(MockBattlegroup.chatBody(INNER), { userId: '6FF6498F4074E3DE' }));
    expect(parsed).toMatchObject({
      msg: 'hello arrakis',
      senderFuncomId: 'PLAYER#12345',
      senderFlsId: '6FF6498F4074E3DE',
      channelType: 'Map',
      messageId: 'abc-123',
    });
    expect(parsed?.originLocation).toEqual({ x: 100.5, y: 200.25, z: 30 });
  });

  it('reads it under `Content` too (outbound spelling seen on some builds)', () => {
    const parsed = parseChatMessage(message(MockBattlegroup.chatBody(INNER, { contentKey: 'Content' })));
    expect(parsed?.msg).toBe('hello arrakis');
  });

  it('accepts both the short and the fully qualified channel enum', () => {
    expect(parseChatMessage(message(MockBattlegroup.chatBody({ ...INNER, m_ChannelType: 'Whispers' })))?.channelType).toBe('Whispers');
    expect(
      parseChatMessage(message(MockBattlegroup.chatBody({ ...INNER, m_ChannelType: 'ETextChatChannelType::Whispers' })))?.channelType,
    ).toBe('Whispers');
  });

  it('survives a body that is not a chat payload at all', () => {
    expect(parseChatMessage(message('not json'))).toBeNull();
    expect(parseChatMessage(message({ content: 'not json either' }))).toBeNull();
    expect(parseChatMessage(message(MockBattlegroup.chatBody({})))?.msg).toBe('');
  });

  it('treats an unfilled (0,0,0) origin as absent rather than as the map origin', () => {
    expect(normaliseOrigin({ X: 0, Y: 0, Z: 0 })).toBeNull();
    expect(normaliseOrigin(undefined)).toBeNull();
    expect(normaliseOrigin({ X: 1, Y: 0, Z: 0 })).toEqual({ x: 1, y: 0, z: 0 });
  });
});

describe('outbound chat payload', () => {
  it('carries the spoofed sender name and the confirmed timestamp spelling', () => {
    const payload = buildChatPayload(
      { msg: 'hi', senderName: 'Takaro', senderFuncomId: 'ADMIN#00001', channel: 'Whispers', userNameTo: 'Tester', now: new Date(Date.UTC(2026, 4, 21, 2, 43, 11)) },
      wire,
    );
    expect(payload).toMatchObject({
      m_ChannelType: 'Whispers',
      m_bUseSpoofedUserName: true,
      m_SpoofedUserNameFrom: { m_TableId: '', m_Key: '', m_UnlocalizedName: 'Takaro' },
      m_FuncomIdFrom: 'ADMIN#00001',
      m_UserNameTo: 'Tester',
      m_HasSeenMessage: false,
      m_TimeStamp: '2026.05.21-02.43.11',
    });
    expect((payload.m_Message as Record<string, unknown>).m_UnlocalizedMessage).toBe('hi');
    const body = JSON.parse(buildChatBody(payload, wire).toString('utf8'));
    expect(typeof body.Content).toBe('string');
    expect(body.Type).toBe('TextChat');
  });

  it('every build-sensitive field is switchable by config, with no code change', () => {
    const variant = { ...wire, timestampField: 'm_Timestamp', timestampFormat: 'rfc3339' as const, channelEnumForm: 'qualified' as const, contentKey: 'content' as const };
    const payload = buildChatPayload({ msg: 'x', senderName: 'T', senderFuncomId: 'A#1', channel: 'Map', now: new Date(0) }, variant);
    expect(payload.m_Timestamp).toBe('1970-01-01T00:00:00.000Z');
    expect(payload.m_TimeStamp).toBeUndefined();
    expect(payload.m_ChannelType).toBe('ETextChatChannelType::Map');
    expect(JSON.parse(buildChatBody(payload, variant).toString('utf8')).content).toBeDefined();
  });
});

describe('DuneRmq', () => {
  function makeRmq(battlegroup: MockBattlegroup, onChat?: (m: DuneChatMessage) => void): DuneRmq {
    return new DuneRmq({
      url: 'amqps://test/',
      tlsInsecure: true,
      connect: battlegroup.connect,
      chatQueue: 'takaro_chat_intercept',
      interceptExchange: 'chat.intercept',
      interceptRoutingKey: '#',
      whisperExchange: 'chat.whispers',
      mapExchange: 'chat.map',
      wire,
      senderName: 'Takaro',
      announcerFuncomId: 'ADMIN#00001',
      onChat,
    });
  }

  it('keeps retrying when the broker refuses the first connect, then binds the chat consumer', async () => {
    const battlegroup = new MockBattlegroup();
    let refusals = 2;
    const rmq = new DuneRmq({
      url: 'amqps://test/',
      tlsInsecure: true,
      // The sidecar and the brokers start together; the first attempts hit a broker that is not listening yet.
      connect: () => (refusals-- > 0 ? Promise.reject(new Error('connect ECONNREFUSED 172.30.0.4:5672')) : battlegroup.connect()),
      reconnectMs: 5,
      chatQueue: 'takaro_chat_intercept',
      interceptExchange: 'chat.intercept',
      interceptRoutingKey: '#',
      whisperExchange: 'chat.whispers',
      mapExchange: 'chat.map',
      wire,
      senderName: 'Takaro',
      announcerFuncomId: 'ADMIN#00001',
    });
    await expect(rmq.start()).rejects.toThrow('ECONNREFUSED');
    expect(rmq.connected()).toBe(false);
    await vi.waitFor(() => expect(rmq.chatConsumerBound()).toBe(true), { timeout: 1000 });
    expect(rmq.error()).toBeNull();
    await rmq.close();
  });

  it('binds a durable intercept queue with `#` and forwards messages', async () => {
    const battlegroup = new MockBattlegroup();
    const seen: DuneChatMessage[] = [];
    const rmq = makeRmq(battlegroup, (m) => seen.push(m));
    await rmq.start();
    expect(battlegroup.binds[0]).toEqual({ action: 'bind', queue: 'takaro_chat_intercept', exchange: 'chat.intercept', routingKey: '#' });
    expect(rmq.chatConsumerBound()).toBe(true);

    battlegroup.deliverChat(MockBattlegroup.chatBody(INNER), { userId: '6FF6498F4074E3DE' });
    expect(seen).toHaveLength(1);
    expect(seen[0].msg).toBe('hello arrakis');
    // The origin is banked as a live location hint for that player.
    expect(rmq.originHint('6FF6498F4074E3DE')).toEqual({ x: 100.5, y: 200.25, z: 30 });
    expect(rmq.originHint('someone-else')).toBeNull();
  });

  it('noEcho: our own injected messages come back on chat.intercept and are dropped', async () => {
    const battlegroup = new MockBattlegroup();
    const seen: DuneChatMessage[] = [];
    const rmq = makeRmq(battlegroup, (m) => seen.push(m));
    await rmq.start();

    await rmq.broadcastChat('a message from Takaro');
    const published = JSON.parse(battlegroup.publishes.at(-1)!.body) as Record<string, string>;
    const inner = JSON.parse(published.Content) as Record<string, unknown>;

    // The broker echoes it straight back to every consumer, including ours.
    battlegroup.deliverChat(MockBattlegroup.chatBody(inner), { userId: 'fls' });
    expect(seen).toHaveLength(0);

    // Matched by sender identity too, for a build that rewrites the id.
    battlegroup.deliverChat(MockBattlegroup.chatBody({ ...INNER, m_Id: 'other', m_FuncomIdFrom: 'ADMIN#00001' }));
    expect(seen).toHaveLength(0);

    // A real player's message still gets through.
    battlegroup.deliverChat(MockBattlegroup.chatBody(INNER), { userId: '6FF6498F4074E3DE' });
    expect(seen).toHaveLength(1);
  });

  it('whisper publishes on the funcom-id routing key and binds nothing', async () => {
    const battlegroup = new MockBattlegroup();
    const rmq = makeRmq(battlegroup);
    await rmq.start();
    // MEASURED on the live rig: the client itself binds `<FLS>_queue` to chat.whispers under its FUNCOM id, and the
    // `fls` broker user may not write to that queue at all (403 ACCESS_REFUSED), so the connector must not bind.
    const result = await rmq.whisper('Tester#41350', 'private hello', 'Tester');
    expect(result).toMatchObject({ ok: true, routingKey: 'Tester#41350' });
    expect(battlegroup.binds.filter((b) => b.exchange === 'chat.whispers')).toEqual([]);
    expect(battlegroup.publishes[0]).toMatchObject({ exchange: 'chat.whispers', routingKey: 'Tester#41350' });
    expect(battlegroup.publishes[0].options).toMatchObject({ contentType: 'Content', type: 'text_chat', deliveryMode: 1 });
  });

  it('a whisper is typed Map by default, because client 25611704 renders a Whispers-typed line empty', async () => {
    const battlegroup = new MockBattlegroup();
    const rmq = makeRmq(battlegroup);
    await rmq.start();
    await rmq.whisper('Tester#41350', 'private hello', 'Tester');
    const inner = JSON.parse((JSON.parse(battlegroup.publishes[0].body) as Record<string, string>).Content) as Record<string, unknown>;
    expect(inner.m_ChannelType).toBe('Map');
    expect(inner.m_UserNameTo).toBe('Tester');
    expect((inner.m_Message as Record<string, unknown>).m_UnlocalizedMessage).toBe('private hello');
    // Still private: the routing key is the recipient's own chat.whispers binding.
    expect(battlegroup.publishes[0]).toMatchObject({ exchange: 'chat.whispers', routingKey: 'Tester#41350' });
  });

  it('DUNE_CHAT_WHISPER_CHANNEL_TYPE switches the whisper channel back for a build that renders Whispers', () => {
    expect(loadConfig({}).chatWire.whisperChannelType).toBe('Map');
    expect(loadConfig({ DUNE_CHAT_WHISPER_CHANNEL_TYPE: 'Whispers' }).chatWire.whisperChannelType).toBe('Whispers');
    expect(loadConfig({ DUNE_CHAT_WHISPER_CHANNEL_TYPE: '  ' }).chatWire.whisperChannelType).toBe('Map');
  });

  it('a global message is one fan-out publish on chat.map', async () => {
    const battlegroup = new MockBattlegroup();
    const rmq = makeRmq(battlegroup);
    await rmq.start();
    // No management API in the mock, so the binding keys cannot be consulted and the historical empty key is used.
    await rmq.broadcastChat('server-wide');
    expect(battlegroup.publishes.at(-1)).toMatchObject({ exchange: 'chat.map', routingKey: '' });
    // An explicitly configured key is published verbatim — this is the shape the live rig needs (`HaggaBasin.0`).
    await rmq.broadcastChat('server-wide', 'HaggaBasin.0');
    expect(battlegroup.publishes.at(-1)).toMatchObject({ exchange: 'chat.map', routingKey: 'HaggaBasin.0' });
    expect(battlegroup.binds.filter((b) => b.exchange === 'chat.map')).toHaveLength(0);
  });
});
