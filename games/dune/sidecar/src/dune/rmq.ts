import { randomUUID } from 'node:crypto';
import { logger } from '../logger.js';
import { asRecord } from '../takaro/protocol.js';
import type { ChatWireVariant } from './config.js';
import { str } from './identity.js';
import { formatUeTimestamp, maybePosition, shortChannel } from './mapping.js';
import type { DuneChatMessage, Position } from './types.js';

/**
 * The slice of amqplib this connector uses. Everything is behind this interface so `testing/mockBattlegroup` can
 * record binds/publishes and inject `chat.intercept` deliveries without a broker.
 */
export interface AmqpChannel {
  assertQueue(queue: string, options?: Record<string, unknown>): Promise<unknown>;
  bindQueue(queue: string, exchange: string, routingKey: string): Promise<unknown>;
  unbindQueue(queue: string, exchange: string, routingKey: string): Promise<unknown>;
  consume(queue: string, onMessage: (msg: AmqpMessage | null) => void, options?: Record<string, unknown>): Promise<{ consumerTag: string }>;
  /**
   * Passive `exchange.declare`. It answers without creating anything, and on a missing exchange the broker replies
   * 404 and **kills the channel** — which is exactly why this is only ever called on a throwaway channel.
   */
  checkExchange?(exchange: string): Promise<unknown>;
  ack(msg: AmqpMessage): void;
  publish(exchange: string, routingKey: string, content: Buffer, options?: Record<string, unknown>): boolean;
  prefetch?(count: number): Promise<unknown> | void;
  close?(): Promise<void>;
}

export interface AmqpMessage {
  content: Buffer;
  fields: { routingKey: string; deliveryTag?: number; exchange?: string };
  properties: { userId?: string | null; messageId?: string | null; appId?: string | null; type?: string | null };
}

export interface AmqpConnection {
  createChannel(): Promise<AmqpChannel>;
  close(): Promise<void>;
  on(event: 'error' | 'close', listener: (err?: Error) => void): unknown;
}

export type AmqpConnector = (url: string, options?: Record<string, unknown>) => Promise<AmqpConnection>;

// ---------------------------------------------------------------------------
// Inbound: chat.intercept
// ---------------------------------------------------------------------------

/**
 * Decodes one `chat.intercept` delivery.
 *
 * The body is `{"content"|"Content": "<json string>", "Type": …}` — the inner chat payload is a STRING, and the outer
 * key's capitalisation differs between the inbound (`content`) and the outbound (`Content`) direction on the builds
 * seen so far, so both are accepted. The sender's FLS id is not in the payload at all: it is the AMQP `user_id`
 * property the broker stamps on, which is what makes `chat.intercept` trustworthy as an identity source.
 */
export function parseChatMessage(msg: AmqpMessage): DuneChatMessage | null {
  // The full raw frame, once, at debug level: the ONLY way to learn the real field shapes (which field carries the
  // sender's display name, how `m_FuncomIdFrom` is formatted) is to read a payload the GAME built. Redacted, because
  // the envelope can carry an auth token.
  logger.debug(`CHAT RECV raw rk=${msg.fields.routingKey} user_id=${msg.properties.userId ?? '-'} body=${redactChat(msg.content.toString('utf8'))}`);
  let outer: Record<string, unknown>;
  try {
    outer = asRecord(JSON.parse(msg.content.toString('utf8')));
  } catch (err) {
    logger.debug(`Ignoring non-JSON chat.intercept delivery: ${(err as Error).message}`);
    return null;
  }
  let inner: Record<string, unknown> = asRecord(outer.content ?? outer.Content ?? outer);
  if (typeof (outer.content ?? outer.Content) === 'string') {
    try {
      inner = asRecord(JSON.parse(String(outer.content ?? outer.Content)));
    } catch {
      return null;
    }
  }
  const message = asRecord(inner.m_Message);
  const text = str(message.m_UnlocalizedMessage) ?? str(asRecord(message.m_LocalizedMessage).m_Key) ?? '';
  const out: DuneChatMessage = {
    msg: text,
    routingKey: msg.fields.routingKey,
  };
  const funcom = str(inner.m_FuncomIdFrom);
  if (funcom) out.senderFuncomId = funcom;
  const fls = str(msg.properties.userId);
  if (fls) out.senderFlsId = fls;
  const channel = str(inner.m_ChannelType);
  if (channel) out.channelType = shortChannel(channel);
  const id = str(inner.m_Id) ?? str(msg.properties.messageId);
  if (id) out.messageId = id;
  out.originLocation = normaliseOrigin(inner.m_OriginLocation);
  return out;
}

/** Never let an auth token or a password out of a wire dump. */
export function redactChat(raw: string): string {
  const out = raw.replace(/("(?:AuthToken|Token|Password|Secret)"\s*:\s*")[^"]*"/gi, '$1<redacted>"');
  return out.length > 4000 ? `${out.slice(0, 4000)}…` : out;
}

/**
 * `m_OriginLocation` is the sender's position at the moment of the message — the only LIVE position available without
 * the native plugin. `(0,0,0)` is what the field carries when the server did not fill it in, and reporting the map
 * origin as a player's location would be worse than reporting nothing, so it is treated as absent.
 */
export function normaliseOrigin(raw: unknown): Position | null {
  const pos = maybePosition(raw);
  if (!pos) return null;
  if (pos.x === 0 && pos.y === 0 && pos.z === 0) return null;
  return pos;
}

// ---------------------------------------------------------------------------
// Outbound: chat.whispers / chat.map
// ---------------------------------------------------------------------------

export interface OutboundChatArgs {
  msg: string;
  /** Name shown in game as the sender (`TAKARO_SENDER_NAME`), carried by the spoofed-username fields. */
  senderName: string;
  senderFuncomId: string;
  /** `m_ChannelType`: `ChatWireVariant.whisperChannelType` for a private reply, `Map` for a server-wide one. */
  channel: string;
  /** Recipient's character name (`m_UserNameTo`); empty for a broadcast. */
  userNameTo?: string;
  now?: Date;
  messageId?: string;
}

/** Builds the inner chat payload in whichever spelling the configured wire variant asks for. */
export function buildChatPayload(args: OutboundChatArgs, wire: ChatWireVariant): Record<string, unknown> {
  const now = args.now ?? new Date();
  const payload: Record<string, unknown> = {
    m_Id: args.messageId ?? randomUUID(),
    m_ChannelType: wire.channelEnumForm === 'qualified' ? `ETextChatChannelType::${args.channel}` : args.channel,
    m_SubChannelId: '',
    m_bUseSpoofedUserName: true,
    m_SpoofedUserNameFrom: { m_TableId: '', m_Key: '', m_UnlocalizedName: args.senderName },
    m_FuncomIdFrom: args.senderFuncomId,
    /**
     * 2026-09-21, SETTLED with a captured real frame and four A/B publishes against a live client with the chat
     * window open: our lines DO render in the in-game chat window, but always with an empty `[]` sender, and
     * **nothing in this payload can change that**. A player-authored frame carries no `m_UserNameFrom` at all;
     * publishing with the spoof pair, with `m_UserNameFrom`, or with a REAL existing `m_FuncomIdFrom`
     * (`Tester#41350`) all rendered `[]` identically, while the same client renders its own lines `[TakaroTest]`.
     * The only field that differs is the AMQP `user_id` property, which carries the sender's FLS id and which
     * RabbitMQ forces to equal the publishing credential — so the connector cannot set it.
     *
     * Consequence, and it belongs in the README: the CHAT leg is inherently nameless, and the named path is the
     * GM `ServiceBroadcast` HUD panel, whose title does render `Takaro`. Hence `DUNE_GLOBAL_MESSAGE_MODE=both`.
     * The field stays a switch (`DUNE_CHAT_SENDER_NAME_FIELD`, empty by default) purely so a future game build
     * can be re-tested without a rebuild.
     */
    ...(wire.senderNameField ? { [wire.senderNameField]: args.senderName } : {}),
    m_UserNameTo: args.userNameTo ?? '',
    m_Message: {
      m_UnlocalizedMessage: args.msg,
      m_LocalizedMessage: { m_TableId: '', m_Key: '', m_FormatArgs: [] },
    },
    m_OriginLocation: { X: 0.0, Y: 0.0, Z: 0.0 },
    m_HasSeenMessage: false,
  };
  payload[wire.timestampField] = wire.timestampFormat === 'ue' ? formatUeTimestamp(now) : now.toISOString();
  return payload;
}

/** The AMQP body: the inner payload as a JSON *string* under `Content`/`content`, plus the `Type` discriminator. */
export function buildChatBody(payload: Record<string, unknown>, wire: ChatWireVariant): Buffer {
  const body: Record<string, unknown> = { [wire.contentKey]: JSON.stringify(payload), Type: wire.bodyType };
  return Buffer.from(JSON.stringify(body), 'utf8');
}

export interface RmqClientOptions {
  url: string;
  tlsInsecure: boolean;
  connect: AmqpConnector;
  chatQueue: string;
  interceptExchange: string;
  interceptRoutingKey: string;
  whisperExchange: string;
  mapExchange: string;
  /** Fixed `chat.map` routing keys; empty = discover the live bindings from the broker's management API. */
  mapRoutingKeys?: string[];
  mapRoutingKeyTtlMs?: number;
  /** Base URL of the broker's management API; defaults to `http://<amqp host>:15672`. */
  managementUrl?: string;
  wire: ChatWireVariant;
  senderName: string;
  announcerFuncomId: string;
  /** Called for every inbound chat message that is not one of ours. */
  onChat?: (message: DuneChatMessage) => void;
  reconnectMs?: number;
  /** Fixed clock for tests. */
  now?: () => Date;
}

/**
 * Owns the connection to the GAME RabbitMQ: the durable `chat.intercept` consumer, the whisper/global publishers, and
 * (optionally) the channel the AMQP GM publisher writes through.
 */
export class DuneRmq {
  private connection: AmqpConnection | null = null;
  private channel: AmqpChannel | null = null;
  private consumerTag: string | null = null;
  private closing = false;
  private reconnectTimer: NodeJS.Timeout | null = null;
  private connecting: Promise<AmqpChannel> | null = null;
  /**
   * Message ids and the announcer's funcom id we have published ourselves. `chat.intercept` sees every message on the
   * server, including the ones we inject, and forwarding those to Takaro makes a hook that replies to chat loop
   * against itself (banked from the Dragonwilds run).
   */
  private readonly ownMessageIds = new Set<string>();
  /** Live position per player from the last message they sent, the only live location without the plugin. */
  private readonly originHints = new Map<string, { position: Position; at: number }>();
  /** Passive exchange-declare results, by exchange name. See `exchangeExists`. */
  private readonly exchangeCache = new Map<string, { exists: boolean; at: number }>();
  /** Cached `chat.map` binding keys; see `mapRoutingKeys`. */
  private mapKeyCache: { keys: string[]; consulted: boolean; at: number } | null = null;
  private lastError: string | null = null;

  constructor(private readonly options: RmqClientOptions) {}

  connected(): boolean {
    return this.channel !== null;
  }

  chatConsumerBound(): boolean {
    return this.consumerTag !== null;
  }

  error(): string | null {
    return this.lastError;
  }

  /** Last known live position for a player, by FLS id or funcom id, from their most recent chat message. */
  originHint(id: string, maxAgeMs = 10 * 60_000, now = Date.now()): Position | null {
    const hit = this.originHints.get(id.toLowerCase());
    if (!hit) return null;
    return now - hit.at <= maxAgeMs ? hit.position : null;
  }

  async start(): Promise<void> {
    await this.ensureChannel();
  }

  /** Shared with the AMQP GM publisher so both directions ride one connection. */
  async ensureChannel(): Promise<AmqpChannel> {
    if (this.channel) return this.channel;
    if (this.connecting) return this.connecting;
    this.connecting = this.open().finally(() => {
      this.connecting = null;
    });
    return this.connecting;
  }

  private async open(): Promise<AmqpChannel> {
    if (!this.options.url) throw new Error('DUNE_RMQ_URL is not set; the chat bridge and the AMQP GM publisher need it');
    const socketOptions = this.options.tlsInsecure
      ? // The broker ships a self-signed certificate on the rig; `rejectUnauthorized:false` is opt-in via
        // DUNE_RMQ_TLS_INSECURE and is the documented default for a self-hosted battlegroup.
        { rejectUnauthorized: false }
      : {};
    const connection = await this.options.connect(this.options.url, socketOptions);
    connection.on('error', (err) => logger.warn(`Game RabbitMQ error: ${err?.message ?? 'unknown'}`));
    connection.on('close', () => this.onClosed());
    const channel = await connection.createChannel();
    await channel.prefetch?.(16);
    this.connection = connection;
    this.channel = channel;
    this.lastError = null;
    logger.info('Connected to the game RabbitMQ');
    await this.bindChatConsumer(channel);
    return channel;
  }

  /**
   * A DURABLE, named queue bound with `#`: durable so that messages published while the sidecar is restarting are
   * still there when it comes back, and named (not exclusive) so a restart re-attaches to the same queue instead of
   * leaving orphans behind on the broker.
   */
  private async bindChatConsumer(channel: AmqpChannel): Promise<void> {
    // Bound even when no chat forwarder is configured: the consumer is also what keeps the per-player live-position
    // hints (`m_OriginLocation`) up to date, which `getPlayerLocation` falls back to.
    await channel.assertQueue(this.options.chatQueue, { durable: true, autoDelete: false });
    await channel.bindQueue(this.options.chatQueue, this.options.interceptExchange, this.options.interceptRoutingKey);
    const { consumerTag } = await channel.consume(this.options.chatQueue, (msg) => {
      if (!msg) return;
      try {
        this.handleChat(msg);
      } catch (err) {
        logger.warn(`Dropping malformed chat delivery: ${(err as Error).message}`);
      } finally {
        channel.ack(msg);
      }
    });
    this.consumerTag = consumerTag;
    logger.info(`Listening on ${this.options.interceptExchange} (queue ${this.options.chatQueue}, rk ${this.options.interceptRoutingKey})`);
  }

  private handleChat(msg: AmqpMessage): void {
    const parsed = parseChatMessage(msg);
    if (!parsed) return;
    if (this.isOwnMessage(parsed)) {
      logger.debug(`noEcho: dropped our own chat message ${parsed.messageId ?? '(no id)'}`);
      return;
    }
    if (parsed.originLocation) {
      const at = Date.now();
      for (const key of [parsed.senderFlsId, parsed.senderFuncomId]) {
        if (key) this.originHints.set(key.toLowerCase(), { position: parsed.originLocation, at });
      }
    }
    this.options.onChat?.(parsed);
  }

  /** Our own injected messages come back on `chat.intercept`; they are matched by id first, sender identity second. */
  isOwnMessage(message: DuneChatMessage): boolean {
    if (message.messageId && this.ownMessageIds.has(message.messageId)) return true;
    return Boolean(message.senderFuncomId && message.senderFuncomId === this.options.announcerFuncomId);
  }

  private noteOwnMessage(id: string): void {
    this.ownMessageIds.add(id);
    // Bounded: only the most recent ids can still be in flight on the broker.
    if (this.ownMessageIds.size > 512) {
      const oldest = this.ownMessageIds.values().next().value;
      if (oldest !== undefined) this.ownMessageIds.delete(oldest);
    }
  }

  /**
   * A private message to one player.
   *
   * MEASURED on the live rig (2026-09-21): the client binds its own queue `<FLS>_queue` to the DIRECT exchange
   * `chat.whispers` under its **funcom id** (`Tester#41350`), not under its FLS id — and the `fls` broker user has no
   * `write` permission on that queue, so the previous temporary-bind approach failed outright with
   * `403 ACCESS_REFUSED ... queue '<FLS>_queue'`. The binding the client makes is permanent for the session, so the
   * correct route is simply to publish on the funcom id and bind nothing.
   */
  async whisper(routingKeyId: string, msg: string, userNameTo?: string): Promise<{ ok: boolean; routingKey: string; queue: string }> {
    const channel = await this.ensureChannel();
    const routingKey = routingKeyId;
    const queue = `${routingKeyId}_queue`;
    const id = randomUUID();
    const payload = buildChatPayload(
      {
        msg,
        senderName: this.options.senderName,
        senderFuncomId: this.options.announcerFuncomId,
        channel: this.options.wire.whisperChannelType,
        userNameTo,
        messageId: id,
        now: this.options.now?.(),
      },
      this.options.wire,
    );
    this.noteOwnMessage(id);
    const ok = channel.publish(this.options.whisperExchange, routingKey, buildChatBody(payload, this.options.wire), this.publishOptions(id));
    return { ok, routingKey, queue };
  }

  /**
   * Does this exchange exist on the broker right now?
   *
   * `chat.map` / `chat.proximity` / `chat.party` / `chat.guild` are declared by the **map server**, not by
   * text-router, so on a battlegroup whose map process has not joined the broker yet they are simply absent (that is
   * exactly what the rig looked like at first boot). Publishing to a missing exchange without `mandatory` is
   * silently dropped, and declaring it actively would need `configure` rights we deliberately do not have.
   *
   * So it is probed PASSIVELY, on a **throwaway channel**: a 404 closes that channel and nothing else. The main
   * channel — which carries the chat consumer and the GM publisher — is never put at risk. Results are cached for
   * `ttlMs` so a chatty module does not open a channel per message, and a positive result is cached for good
   * because the game never deletes these.
   */
  async exchangeExists(exchange: string, ttlMs = 30_000, now = Date.now()): Promise<boolean> {
    const cached = this.exchangeCache.get(exchange);
    if (cached && (cached.exists || now - cached.at < ttlMs)) return cached.exists;
    let exists = false;
    let probe: AmqpChannel | null = null;
    try {
      const connection = this.connection ?? ((await this.ensureChannel()), this.connection);
      if (!connection) return false;
      probe = await connection.createChannel();
      if (typeof probe.checkExchange !== 'function') {
        // A driver without passive declare cannot tell us; assume present rather than silently disabling chat.
        this.exchangeCache.set(exchange, { exists: true, at: now });
        return true;
      }
      await probe.checkExchange(exchange);
      exists = true;
    } catch (err) {
      logger.debug(`Exchange ${exchange} is not declared on the broker: ${(err as Error).message}`);
      exists = false;
    } finally {
      // The channel is already dead after a 404; closing a dead channel throws and that is not news.
      try {
        await probe?.close?.();
      } catch {
        /* expected after a failed passive declare */
      }
    }
    this.exchangeCache.set(exchange, { exists, at: now });
    return exists;
  }

  /** Forgets the cached exchange probes (used when the connection is re-established). */
  resetExchangeCache(): void {
    this.exchangeCache.clear();
    this.mapKeyCache = null;
  }

  /**
   * A server-wide chat line: one publish onto the map chat exchange, which fans out to every online player's queue.
   *
   * It refuses instead of publishing into the void when the exchange is absent, and the caller
   * (`adapter.sendMessage`) then falls back to a whisper fan-out or `ServiceBroadcast` per
   * `DUNE_GLOBAL_MESSAGE_MODE`. Answering `{ok:true}` for bytes nobody will ever route is the kind of claim this
   * campaign does not make.
   */
  async broadcastChat(msg: string, routingKey?: string): Promise<{ ok: boolean; reason?: string; routingKeys?: string[] }> {
    if (!(await this.exchangeExists(this.options.mapExchange))) {
      return { ok: false, reason: `exchange '${this.options.mapExchange}' is not declared on the broker` };
    }
    // `chat.map` is a DIRECT exchange and the clients bind under `<MapRegion>.<dimension>` (measured: `HaggaBasin.0`).
    // Publishing on the empty routing key — which is what this did before — matches no binding at all, so the broker
    // discards the message and the connector reports a delivery nobody could ever have seen.
    const discovered = routingKey !== undefined ? { keys: [routingKey], consulted: true } : await this.mapRoutingKeys();
    // A broker we could ASK, that answered "nothing is bound", means nobody would receive the line — say so rather
    // than publishing into the void. A broker we could NOT ask leaves us with the historical empty key: a guess, but
    // a better one than silence, and it is reported as such.
    if (discovered.keys.length === 0 && discovered.consulted) {
      return { ok: false, reason: `nothing is bound to '${this.options.mapExchange}', so a global chat line would reach nobody` };
    }
    const keys = discovered.keys.length ? discovered.keys : [''];
    const channel = await this.ensureChannel();
    let ok = false;
    for (const key of keys) {
      const id = randomUUID();
      const payload = buildChatPayload(
        {
          msg,
          senderName: this.options.senderName,
          senderFuncomId: this.options.announcerFuncomId,
          channel: 'Map',
          messageId: id,
          now: this.options.now?.(),
        },
        this.options.wire,
      );
      this.noteOwnMessage(id);
      ok = channel.publish(this.options.mapExchange, key, buildChatBody(payload, this.options.wire), this.publishOptions(id)) || ok;
    }
    return { ok, routingKeys: keys };
  }

  /**
   * The routing keys a global chat line has to be published on, newest knowledge first:
   * the configured override, else the live bindings of `chat.map` read from the broker's management API.
   *
   * Discovery beats a hard-coded key because the key is `<MapRegion>.<dimension>` and a battlegroup may run several
   * maps or dimensions. Results are cached briefly; a failure is cached too so a broken management endpoint cannot
   * turn every `sendMessage` into an HTTP round trip.
   */
  async mapRoutingKeys(now = Date.now()): Promise<{ keys: string[]; consulted: boolean }> {
    if (this.options.mapRoutingKeys?.length) return { keys: this.options.mapRoutingKeys, consulted: true };
    if (this.mapKeyCache && now - this.mapKeyCache.at < (this.options.mapRoutingKeyTtlMs ?? 15_000)) return this.mapKeyCache;
    let keys: string[] = [];
    let consulted = false;
    const mgmt = this.managementUrl();
    if (mgmt) {
      try {
        const url = `${mgmt.base}/api/exchanges/${encodeURIComponent(mgmt.vhost)}/${encodeURIComponent(this.options.mapExchange)}/bindings/source`;
        const res = await fetch(url, { headers: { Authorization: `Basic ${Buffer.from(`${mgmt.user}:${mgmt.pass}`).toString('base64')}` } });
        if (res.ok) {
          const body = (await res.json()) as Array<{ routing_key?: unknown }>;
          keys = [...new Set(body.map((b) => str(b.routing_key)).filter((k): k is string => Boolean(k)))];
          consulted = true;
        } else {
          logger.debug(`Management API refused the ${this.options.mapExchange} binding list: ${res.status}`);
        }
      } catch (err) {
        logger.debug(`Could not read ${this.options.mapExchange} bindings from the management API: ${(err as Error).message}`);
      }
    }
    this.mapKeyCache = { keys, consulted, at: now };
    return this.mapKeyCache;
  }

  /** Management endpoint derived from the AMQP URL (same host and credentials, HTTP port 15672) unless configured. */
  private managementUrl(): { base: string; vhost: string; user: string; pass: string } | null {
    try {
      const amqp = new URL(this.options.url);
      const user = decodeURIComponent(amqp.username || '');
      const pass = decodeURIComponent(amqp.password || '');
      if (!user) return null;
      const vhost = amqp.pathname && amqp.pathname !== '/' ? decodeURIComponent(amqp.pathname.slice(1)) : '/';
      const base = this.options.managementUrl?.replace(/\/$/, '') ?? `http://${amqp.hostname}:15672`;
      return { base, vhost, user, pass };
    } catch {
      return null;
    }
  }

  private publishOptions(id: string): Record<string, unknown> {
    return {
      contentType: this.options.wire.contentType,
      type: this.options.wire.amqpType,
      deliveryMode: this.options.wire.deliveryMode,
      messageId: id,
    };
  }

  private onClosed(): void {
    this.channel = null;
    this.connection = null;
    this.consumerTag = null;
    // The map server may have joined (or left) the broker while we were away, and it owns `chat.map`.
    this.exchangeCache.clear();
    if (this.closing) return;
    this.lastError = 'connection closed';
    if (this.reconnectTimer) return;
    const delay = this.options.reconnectMs ?? 5000;
    logger.warn(`Game RabbitMQ connection closed; reconnecting in ${delay}ms`);
    this.reconnectTimer = setTimeout(() => {
      this.reconnectTimer = null;
      void this.ensureChannel().catch((err) => {
        this.lastError = (err as Error).message;
        logger.warn(`Game RabbitMQ reconnect failed: ${(err as Error).message}`);
        this.onClosed();
      });
    }, delay);
    this.reconnectTimer.unref?.();
  }

  async close(): Promise<void> {
    this.closing = true;
    if (this.reconnectTimer) clearTimeout(this.reconnectTimer);
    this.reconnectTimer = null;
    try {
      await this.channel?.close?.();
      await this.connection?.close();
    } catch {
      /* already gone */
    }
    this.channel = null;
    this.connection = null;
  }
}
