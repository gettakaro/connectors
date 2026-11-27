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
    // Takaro links players across servers and games only through steamId /
    // epicOnlineServicesId / xboxLiveId / platformId, compared verbatim (it
    // never parses platformId). DayZ PC players are Steam players, so:
    //   steamId    = the bare SteamID64 (GetPlainId), never the BIS hash
    //   platformId = "steam:<SteamID64>", the form every other connector uses;
    //                "dayz:<BIS hash>" only when no SteamID64 is known.
    static bool IsSteamId64(string sid)
    {
        if (sid.Length() != 17) return false;
        if (sid.Substring(0, 4) != "7656") return false;
        string digits = "0123456789";
        for (int i = 0; i < 17; i++)
        {
            if (digits.IndexOf(sid.Substring(i, 1)) < 0) return false;
        }
        return true;
    }

    static string PlatformIdFor(string sid, string bis)
    {
        if (IsSteamId64(sid)) return "steam:" + sid;
        int eqIdx = bis.IndexOf("=");
        if (eqIdx >= 0) bis = bis.Substring(0, eqIdx);
        if (bis != "") return "dayz:" + bis;
        return "dayz:" + sid;
    }

    static string NowIso()
    {
        // M1: was World.GetDate (in-game time, not UTC) formatted with
        // string.Format, which rendered e.g. '26-09-20T11:26:00Z'. Use the
        // real UTC wall clock and plain concatenation.
        int y, mo, d, h, mi, sec;
        GetYearMonthDayUTC(y, mo, d);
        GetHourMinuteSecondUTC(h, mi, sec);
        return y.ToString() + "-" + Pad2(mo) + "-" + Pad2(d) + "T" + Pad2(h) + ":" + Pad2(mi) + ":" + Pad2(sec) + "Z";
    }

    static string Pad2(int v)
    {
        if (v < 10) return "0" + v.ToString();
        return v.ToString();
    }

    // F7 — real JSON escaping for user-controlled strings (player names, chat
    // text, item classnames) instead of the old "replace \" with '" mangling.
    //
    // CParser gotcha: a lone `"\\"` literal breaks Enforce's parser, so we
    // cannot write a single backslash directly. `"\\n"` DOES parse (the lexer
    // reads `\\` as one escaped backslash followed by a plain `n`, yielding the
    // 2-char string backslash+n), so we take its first character to obtain a
    // literal backslash without ever writing the forbidden literal.
    //
    // Order matters: backslashes first, then quotes, otherwise the backslash
    // we inject in front of a quote would itself get escaped.
    static string Backslash()
    {
        string seed = "\\n";
        return seed.Substring(0, 1);
    }

    static string Safe(string s)
    {
        string out_s = s;
        string bs = Backslash();
        out_s.Replace(bs, bs + bs);
        string dq = "\"";
        out_s.Replace(dq, bs + dq);
        string nl = "\n";
        out_s.Replace(nl, "\\n");
        string cr = "\r";
        out_s.Replace(cr, "\\r");
        string tab = "\t";
        out_s.Replace(tab, "\\t");
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
        string name = Safe(TakaroNameCache.Resolve(sid, id.GetName()));
        string q = "\"";
        string s = "{" + q + "gameId" + q + ":" + q + sid + q;
        s += "," + q + "name" + q + ":" + q + name + q;
        if (IsSteamId64(sid)) s += "," + q + "steamId" + q + ":" + q + sid + q;
        s += "," + q + "platformId" + q + ":" + q + PlatformIdFor(sid, id.GetId()) + q;
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
        if (IsSteamId64(sid)) s += "," + q + "steamId" + q + ":" + q + sid + q;
        s += "," + q + "platformId" + q + ":" + q + PlatformIdFor(sid, bis) + q;
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

    // F4 — `entity-killed`. Takaro's EventEntityKilled carries the player who
    // made the kill, the victim entity classname, the weapon and an optional
    // position. Built by concatenation, same as the other factories.
    static string EntityKilled(PlayerBase player, string entityType, string weapon, vector pos)
    {
        string q = "\"";
        string s = "{" + q + "type" + q + ":" + q + "entity-killed" + q;
        s += "," + q + "timestamp" + q + ":" + q + NowIso() + q;
        s += "," + q + "player" + q + ":" + PlayerJson(null, player);
        s += "," + q + "entity" + q + ":" + q + Safe(entityType) + q;
        s += "," + q + "weapon" + q + ":" + q + Safe(weapon) + q;
        // M3 fix: Takaro's EventEntityKilled has NO position property; sending one
        // failed whitelistValidation and the event was dropped ("property position
        // has failed the following constraints: whitelistValidation"). `pos` is kept
        // in the signature for callers/logging only.
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
