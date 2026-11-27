// M3 — Takaro ban list, enforced by the mod.
//
// Root cause of the M3 ban failure: the old code appended Steam64 IDs to
// "ban.txt" / "battleye/bans.txt" via OpenFile() with a bare relative path.
// Enforce only resolves prefixed paths ($profile:, $saves:, ...), so nothing was
// ever written, listBans always returned [], and a banned player could rejoin.
// Vanilla ban.txt also expects the 44-char DayZ player ID, not a Steam64.
//
// Now: bans live in $profile:TakaroIntegration/bans.txt, one per line
//   <steam64>|<expiresAt ISO or "-" = permanent>|<reason or "-">
// and MissionServer.InvokeOnConnect kicks a banned player (with a notification).
class TakaroBanStore
{
    static const string BAN_FILE = "$profile:TakaroIntegration/bans.txt";

    static void Load(out array<string> ids, out array<string> expires, out array<string> reasons)
    {
        FileHandle f = OpenFile(BAN_FILE, FileMode.READ);
        if (f == 0) return;
        string line;
        while (FGets(f, line) >= 0)
        {
            line.TrimInPlace();
            if (line == "" || line.IndexOf("//") == 0) continue;
            array<string> parts = new array<string>;
            line.Split("|", parts);
            if (parts.Count() < 1 || parts[0] == "") continue;
            ids.Insert(parts[0]);
            // Enforce's string.Split drops empty tokens, so "permanent" is stored as "-".
            string exp = "";
            if (parts.Count() >= 2 && parts[1] != "-") exp = parts[1];
            expires.Insert(exp);
            string reason = "";
            if (parts.Count() >= 3)
            {
                reason = parts[2];
                if (reason == "-") reason = "";
                for (int i = 3; i < parts.Count(); i++) reason += "|" + parts[i];
            }
            reasons.Insert(reason);
        }
        CloseFile(f);
    }

    static void Save(array<string> ids, array<string> expires, array<string> reasons)
    {
        MakeDirectory("$profile:TakaroIntegration");
        FileHandle f = OpenFile(BAN_FILE, FileMode.WRITE);
        if (f == 0) { TakaroLog.Warn("TakaroBanStore: cannot write " + BAN_FILE); return; }
        FPrintln(f, "// Takaro bans: steam64|expiresAt(ISO, - = permanent)|reason");
        for (int i = 0; i < ids.Count(); i++)
        {
            string e = expires[i];
            if (e == "" || e == "null") e = "-";
            string r = reasons[i];
            if (r == "") r = "-";
            FPrintln(f, ids[i] + "|" + e + "|" + r);
        }
        CloseFile(f);
    }

    static bool IsExpired(string expiresAt)
    {
        if (expiresAt == "" || expiresAt == "null") return false;
        string now = TakaroEventFactory.NowIso();
        string a = expiresAt;
        if (a.Length() > 19) a = a.Substring(0, 19);
        string b = now.Substring(0, 19);
        return a <= b;
    }

    static void Add(string steamId, string expiresAt, string reason)
    {
        array<string> ids = new array<string>;
        array<string> ex = new array<string>;
        array<string> rs = new array<string>;
        Load(ids, ex, rs);
        reason.Replace("|", "/");
        reason.Replace("\n", " ");
        int idx = ids.Find(steamId);
        if (idx >= 0) { ex[idx] = expiresAt; rs[idx] = reason; }
        else { ids.Insert(steamId); ex.Insert(expiresAt); rs.Insert(reason); }
        Save(ids, ex, rs);
    }

    static bool Remove(string steamId)
    {
        array<string> ids = new array<string>;
        array<string> ex = new array<string>;
        array<string> rs = new array<string>;
        Load(ids, ex, rs);
        int idx = ids.Find(steamId);
        if (idx < 0) return false;
        ids.Remove(idx); ex.Remove(idx); rs.Remove(idx);
        Save(ids, ex, rs);
        return true;
    }

    static bool IsBanned(string steamId, out string reason)
    {
        array<string> ids = new array<string>;
        array<string> ex = new array<string>;
        array<string> rs = new array<string>;
        Load(ids, ex, rs);
        int idx = ids.Find(steamId);
        if (idx < 0) return false;
        if (IsExpired(ex[idx])) return false;
        reason = rs[idx];
        return true;
    }
}

// Notify, emit player-disconnected, then disconnect after a short delay so the
// client can read the reason. Instance-based so CallLater has a live target.
class TakaroKicker
{
    static ref TakaroKicker s_Instance;
    static const int KICK_DELAY_MS = 2500;

    static TakaroKicker Get()
    {
        if (!s_Instance) s_Instance = new TakaroKicker();
        return s_Instance;
    }

    void Kick(PlayerBase pb, PlayerIdentity id, string title, string reason)
    {
        if (!id) return;
        string body = reason;
        if (body == "") body = "No reason given";
        if (pb)
            NotificationSystem.SendNotificationToPlayerExtended(pb, 5.0, title, body, "");
        TakaroBridge bridge = TakaroBridge.Cast(TakaroBridgeAccessor.Get());
        if (bridge) bridge.OnPlayerKickedByTakaro(pb, id);
        TakaroLog.Info(title + " " + id.GetPlainId() + ": " + body);
        GetGame().GetCallQueue(CALL_CATEGORY_SYSTEM).CallLater(this.DoDisconnect, KICK_DELAY_MS, false, id);
    }

    void DoDisconnect(PlayerIdentity id)
    {
        if (id) GetGame().DisconnectPlayer(id);
    }
}
