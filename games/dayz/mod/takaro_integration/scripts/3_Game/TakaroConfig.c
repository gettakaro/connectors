// Persistent configuration for the Takaro bridge.
// Stored at: $profile:TakaroIntegration/config.json
// Loaded on server start, regenerated with defaults if missing.

class TakaroConfigData
{
    string TakaroApiUrl;            // local DLL bridge URL — script POSTs events here, DLL forwards over WS
    string IdentityToken;           // unused at script level (DLL talks to Takaro); kept for compat
    string RegistrationToken;       // unused at script level (DLL talks to Takaro); kept for compat
    string GameServerId;            // server identity used in /gameserver/<id>/... paths to the DLL
    int CommandPollIntervalMs;      // How often to poll for outbound commands
    int EventBatchIntervalMs;       // How often to flush queued events
    int MaxEventsPerBatch;          // Max events sent per flush
    int RequestTimeoutSeconds;      // HTTP request timeout
    bool LogVerbose;                // Verbose logging to RPT
    bool DryRun;                    // If true, log outbound calls instead of sending

    void TakaroConfigData()
    {
        TakaroApiUrl = "http://localhost:8089";
        IdentityToken = "";
        RegistrationToken = "";
        GameServerId = "local";
        CommandPollIntervalMs = 2000;
        EventBatchIntervalMs = 1000;
        MaxEventsPerBatch = 50;
        RequestTimeoutSeconds = 10;
        LogVerbose = false;
        DryRun = false;
    }
}

class TakaroConfig
{
    static const string CONFIG_DIR = "$profile:TakaroIntegration";
    static const string CONFIG_FILE = "$profile:TakaroIntegration/config.json";

    static ref TakaroConfigData m_Data;

    static TakaroConfigData Get()
    {
        if (!m_Data)
            Load();
        return m_Data;
    }

    static void Load()
    {
        if (!FileExist(CONFIG_DIR))
            MakeDirectory(CONFIG_DIR);

        if (FileExist(CONFIG_FILE))
        {
            m_Data = new TakaroConfigData();
            JsonFileLoader<TakaroConfigData>.JsonLoadFile(CONFIG_FILE, m_Data);
            TakaroLog.Info("Loaded config from " + CONFIG_FILE);
        }
        else
        {
            m_Data = new TakaroConfigData();
            Save();
            TakaroLog.Warn("No config found — wrote defaults to " + CONFIG_FILE + ". Edit it and restart.");
        }
    }

    static void Save()
    {
        if (!m_Data)
            return;
        JsonFileLoader<TakaroConfigData>.JsonSaveFile(CONFIG_FILE, m_Data);
    }

    static bool IsConfigured()
    {
        TakaroConfigData d = Get();
        if (!d) return false;
        if (d.TakaroApiUrl == "") return false;
        // Either an identity token or a registration token is required to bootstrap.
        if (d.IdentityToken == "" && d.RegistrationToken == "") return false;
        return true;
    }
}

class TakaroLog
{
    static void Info(string msg)
    {
        Print("[Takaro] " + msg);
    }

    static void Warn(string msg)
    {
        Print("[Takaro][WARN] " + msg);
    }

    static void Error(string msg)
    {
        Print("[Takaro][ERROR] " + msg);
    }

    static void Debug(string msg)
    {
        TakaroConfigData d = TakaroConfig.Get();
        if (d && d.LogVerbose)
            Print("[Takaro][DBG] " + msg);
    }
}

// Global accessor that lives in 3_Game so any layer can read/write the bridge
// reference without creating a circular include between 4_World and 5_Mission.
class TakaroBridgeAccessor
{
    private static ref Class s_BridgeRef;

    static void Set(Class bridge)
    {
        s_BridgeRef = bridge;
    }

    static Class Get()
    {
        return s_BridgeRef;
    }
}

// Persistent steam64 -> real-name cache. DayZ's identity.GetName() returns
// "Survivor" (or "Survivor (N)" when several collide) whenever the Steam/BE
// handshake fails to resolve the player's display name at connect — happens
// intermittently across sessions of the same player. Without this cache,
// "Survivor" leaks into Takaro events and overwrites the player record;
// modules then see data.player.name === "Survivor".
class TakaroNameEntry
{
    string steamId;
    string name;
}

class TakaroNameCacheData
{
    ref array<ref TakaroNameEntry> entries;

    void TakaroNameCacheData()
    {
        entries = new array<ref TakaroNameEntry>;
    }
}

class TakaroNameCache
{
    static const string CACHE_FILE = "$profile:TakaroIntegration/names.json";

    static ref map<string, string> s_Names;
    static bool s_Loaded;

    static void EnsureLoaded()
    {
        if (s_Loaded) return;
        s_Names = new map<string, string>;
        if (FileExist(CACHE_FILE))
        {
            TakaroNameCacheData data = new TakaroNameCacheData;
            JsonFileLoader<TakaroNameCacheData>.JsonLoadFile(CACHE_FILE, data);
            int count = 0;
            if (data.entries) count = data.entries.Count();
            for (int i = 0; i < count; i++)
            {
                TakaroNameEntry e = data.entries[i];
                if (e && e.steamId != "" && e.name != "")
                    s_Names.Set(e.steamId, e.name);
            }
        }
        s_Loaded = true;
    }

    static void Save()
    {
        if (!s_Names) return;
        TakaroNameCacheData data = new TakaroNameCacheData;
        for (int i = 0; i < s_Names.Count(); i++)
        {
            TakaroNameEntry e = new TakaroNameEntry;
            e.steamId = s_Names.GetKey(i);
            e.name = s_Names.GetElement(i);
            data.entries.Insert(e);
        }
        JsonFileLoader<TakaroNameCacheData>.JsonSaveFile(CACHE_FILE, data);
    }

    // DayZ default-name patterns we never want to ship to Takaro.
    static bool IsBogus(string name)
    {
        if (name == "" || name == "Survivor") return true;
        // "Survivor (2)", "Survivor (3)", ... when multiple unauthenticated
        // sessions collide.
        if (name.IndexOf("Survivor (") == 0) return true;
        return false;
    }

    // Given a steam64 and the raw name DayZ reports, return the name we
    // should send to Takaro. Real names update the on-disk cache; bogus
    // names get swapped for the cached real name when one is known.
    static string Resolve(string steam64, string raw)
    {
        EnsureLoaded();
        if (steam64 == "") return raw;

        if (!IsBogus(raw))
        {
            string current = "";
            if (s_Names.Contains(steam64)) current = s_Names.Get(steam64);
            if (current != raw)
            {
                s_Names.Set(steam64, raw);
                Save();
            }
            return raw;
        }

        if (s_Names.Contains(steam64))
            return s_Names.Get(steam64);
        return raw;
    }
}
