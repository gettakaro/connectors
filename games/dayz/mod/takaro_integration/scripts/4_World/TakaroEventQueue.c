// Outbound event queue. Each entry is a pre-built JSON string for one event,
// containing only the fields valid for that event type. We build per-type to
// avoid Enforce JsonSerializer's behavior of emitting every class field
// (including empty strings), which makes Takaro's DTO whitelist validators
// reject events like player-connected when an empty `channel` field is shipped.

class TakaroEventQueue
{
    private ref array<string> m_Pending;
    private int m_MaxQueueLength;
    private int m_DroppedCount;

    void TakaroEventQueue(int maxQueueLength = 1000)
    {
        m_Pending = new array<string>;
        m_MaxQueueLength = maxQueueLength;
        m_DroppedCount = 0;
    }

    void Enqueue(string ev)
    {
        if (ev == "") return;
        if (m_Pending.Count() >= m_MaxQueueLength)
        {
            m_DroppedCount++;
            if (m_DroppedCount % 100 == 1)
                TakaroLog.Warn("Event queue full — dropped " + m_DroppedCount.ToString() + " events total");
            return;
        }
        m_Pending.Insert(ev);
    }

    int Count()
    {
        return m_Pending.Count();
    }

    // Take the first up-to-`max` entries off the queue. The previous
    // implementation called Remove(0) per drained entry, which is O(n) per
    // call and O(n*queueLength) overall. We split in one pass: copy head to a
    // new array, rebuild m_Pending from the tail.
    array<string> Drain(int max)
    {
        array<string> drained = new array<string>;
        int total = m_Pending.Count();
        int n = max;
        if (n > total) n = total;
        if (n <= 0) return drained;

        for (int i = 0; i < n; i++)
            drained.Insert(m_Pending[i]);

        array<string> remaining = new array<string>;
        for (int j = n; j < total; j++)
            remaining.Insert(m_Pending[j]);
        m_Pending = remaining;
        return drained;
    }

    // Push a previously-drained batch back to the front. Rebuilt in one pass
    // (events ++ m_Pending) instead of n InsertAt(0) calls (each O(n)).
    void Requeue(array<string> events)
    {
        if (!events) return;
        int eCount = events.Count();
        if (eCount == 0) return;
        array<string> combined = new array<string>;
        for (int i = 0; i < eCount; i++)
            combined.Insert(events[i]);
        int oldCount = m_Pending.Count();
        for (int j = 0; j < oldCount; j++)
            combined.Insert(m_Pending[j]);
        m_Pending = combined;
    }
}

class TakaroEventFactory
{
    static string NowIso()
    {
        int y, mo, d, h, mi;
        GetGame().GetWorld().GetDate(y, mo, d, h, mi);
        // Seconds always 00 — DayZ's GetDate has minute-resolution. Format
        // each int with width-2 padding via a single string.Format.
        return string.Format("%1-%2-%3T%4:%5:00Z",
            y.ToString(), Pad2(mo), Pad2(d), Pad2(h), Pad2(mi));
    }

    static string Pad2(int v)
    {
        if (v < 10) return "0" + v.ToString();
        return v.ToString();
    }

    // Strip double-quotes from user-controlled strings to keep our naive JSON
    // valid. Same trick the command dispatcher uses; backslash escapes in
    // string literals confuse Enforce's CParser.
    static string Safe(string s)
    {
        string out_s = s;
        string dq = "\"";
        out_s.Replace(dq, "'");
        return out_s;
    }

    // Produce the JSON for an IPlayerReferenceDTO. We fall back to PlayerBase
    // if no identity is available (covers post-disconnect lookups).
    //
    // ID semantics in DayZ:
    //   GetPlainId()  -> Steam64 ("76561198..."), the player's Steam account ID
    //   GetId()       -> BIS hash (base64-ish), the in-game DayZ UID
    // Takaro's platformId pattern is ^[A-Za-z0-9_-]+:[A-Za-z0-9_-]+$, so we
    // strip trailing '=' base64 padding from the BIS hash before composing it.
    static string PlayerJson(PlayerIdentity id, PlayerBase pb)
    {
        if (!id && pb) id = pb.GetIdentity();
        if (!id) return "null";
        string sid = id.GetPlainId();
        string bisid = id.GetId();
        int eqIdx = bisid.IndexOf("=");
        if (eqIdx >= 0) bisid = bisid.Substring(0, eqIdx);
        string name = Safe(TakaroNameCache.Resolve(sid, id.GetName()));
        string q = "\"";
        string s = "{" + q + "gameId" + q + ":" + q + sid + q;
        s += "," + q + "name" + q + ":" + q + name + q;
        s += "," + q + "steamId" + q + ":" + q + sid + q;
        s += "," + q + "platformId" + q + ":" + q + "dayz:" + bisid + q;
        s += "," + q + "ping" + q + ":" + id.GetPingAct().ToString();
        s += "}";
        return s;
    }

    static string Connected(PlayerIdentity id)
    {
        string q = "\"";
        string s = "{" + q + "type" + q + ":" + q + "player-connected" + q;
        s += "," + q + "timestamp" + q + ":" + q + NowIso() + q;
        s += "," + q + "player" + q + ":" + PlayerJson(id, null);
        s += "}";
        return s;
    }

    // For disconnect, identity/player may exist but be in cleanup — accessors
    // can return empty strings. Build the player JSON ourselves: derive the
    // BIS hash from `uid` (which DayZ passes in reliably), and only use the
    // identity for fields that don't break the DTO when missing.
    //
    // Why we don't reuse PlayerJson:
    // - At disconnect, id.GetId() can return "" → platformId becomes "dayz:"
    //   which fails Takaro's ^[A-Za-z0-9_-]+:[A-Za-z0-9_-]+$ regex.
    // - id.GetPlainId() can return "" → empty gameId, breaks downstream
    //   player matching.
    // The `uid` parameter from MissionServer.PlayerDisconnected is the BIS
    // hash (with '=' padding) — reliable even when identity is shedding
    // state.
    // cachedSteam/cachedName come from TakaroBridge's bisHash->steam64 cache
    // populated at connect time. They let us emit the same gameId/name we
    // sent on connect even when PlayerIdentity is null at logout — without
    // them Takaro's resolveRef throws "Platform ID collision detected"
    // because gameId would mismatch the existing POG and the disconnect
    // event silently drops.
    static string Disconnected(PlayerIdentity id, PlayerBase pb, string uid, string cachedSteam = "", string cachedName = "")
    {
        // Strip trailing '=' from BIS uid to satisfy Takaro's platformId regex.
        string bis = uid;
        int eqIdx = bis.IndexOf("=");
        if (eqIdx >= 0) bis = bis.Substring(0, eqIdx);

        // Prefer live identity, then the connect-time cache, then bis as a
        // last resort (for the gameId-non-empty constraint).
        string sid = "";
        string name = "";
        int ping = 0;
        if (!id && pb) id = pb.GetIdentity();
        if (id)
        {
            sid = id.GetPlainId();
            name = id.GetName();
            ping = id.GetPingAct();
        }
        if (sid == "") sid = cachedSteam;
        if (name == "") name = cachedName;
        if (sid == "") sid = bis;
        // Swap "Survivor"/"Survivor (N)" for the persistent real name once
        // we have a steam64 to key on.
        name = Safe(TakaroNameCache.Resolve(sid, name));

        string q = "\"";
        string s = "{" + q + "type" + q + ":" + q + "player-disconnected" + q;
        s += "," + q + "timestamp" + q + ":" + q + NowIso() + q;
        s += "," + q + "player" + q + ":{";
        s += q + "gameId" + q + ":" + q + sid + q;
        s += "," + q + "name" + q + ":" + q + name + q;
        s += "," + q + "steamId" + q + ":" + q + sid + q;
        s += "," + q + "platformId" + q + ":" + q + "dayz:" + bis + q;
        s += "," + q + "ping" + q + ":" + ping.ToString();
        s += "}}";
        return s;
    }

    static string Chat(PlayerIdentity id, string channel, string msg)
    {
        // Concat-built (not string.Format) because the optimizer's prior
        // rewrite to string.Format produced output that Takaro rejected with
        // "channel has failed isEnum" — likely Enforce string.Format losing
        // a substitution when many \" escapes share the format string.
        string q = "\"";
        string s = "{" + q + "type" + q + ":" + q + "chat-message" + q;
        s += "," + q + "timestamp" + q + ":" + q + NowIso() + q;
        s += "," + q + "channel" + q + ":" + q + Safe(channel) + q;
        s += "," + q + "msg" + q + ":" + q + Safe(msg) + q;
        s += "," + q + "player" + q + ":" + PlayerJson(id, null);
        s += "}";
        return s;
    }

    static string Death(PlayerBase victim, EntityAI killer, string weapon)
    {
        // EventPlayerDeath has no `weapon` field (that's on EventEntityKilled).
        // Fold the weapon name into the optional `msg` so it isn't lost.
        string q = "\"";
        string s = "{" + q + "type" + q + ":" + q + "player-death" + q;
        s += "," + q + "timestamp" + q + ":" + q + NowIso() + q;
        s += "," + q + "player" + q + ":" + PlayerJson(null, victim);
        if (killer && killer.IsInherited(PlayerBase))
        {
            PlayerBase kp = PlayerBase.Cast(killer);
            s += "," + q + "attacker" + q + ":" + PlayerJson(null, kp);
        }
        if (victim)
        {
            vector p = victim.GetPosition();
            s += "," + q + "position" + q + ":{";
            s += q + "x" + q + ":" + p[0].ToString();
            s += "," + q + "y" + q + ":" + p[1].ToString();
            s += "," + q + "z" + q + ":" + p[2].ToString();
            s += "}";
        }
        if (weapon != "")
            s += "," + q + "msg" + q + ":" + q + "killed with " + Safe(weapon) + q;
        s += "}";
        return s;
    }

    static string LogLine(string raw)
    {
        // EventLogLine inherits BaseGameEvent.msg — there is no `raw` field.
        string q = "\"";
        string s = "{" + q + "type" + q + ":" + q + "log" + q;
        s += "," + q + "timestamp" + q + ":" + q + NowIso() + q;
        s += "," + q + "msg" + q + ":" + q + Safe(raw) + q;
        s += "}";
        return s;
    }
}
