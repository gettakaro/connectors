// TakaroBridge — top-level controller.
// Owned by MissionServer. Created once at server boot, runs forever until shutdown.
//
// Responsibilities:
//   1. Load config and bootstrap auth (registration token -> identity token).
//   2. Hold the HTTP client and event queue.
//   3. Drive two timers: event flush and command poll.
//   4. Route gameplay events into the queue and operations into the dispatcher.

class TakaroBridge
{
    static const string VERSION = "0.1.14";

    ref TakaroHttpClient m_Http;
    ref TakaroEventQueue m_Queue;
    ref TakaroCommandDispatcher m_Dispatcher;

    bool m_Initialized;
    bool m_Registered;          // true once we have a GameServerId and identity token
    float m_AccumFlush;
    float m_AccumPoll;
    int m_FlushIntervalSec;
    int m_PollIntervalSec;

    // bisHash (no '=' padding) -> Steam64. Captured on connect so that at
    // disconnect — when identity may be shedding state — we can still send
    // the same gameId we sent on connect. Without this Takaro's resolveRef
    // throws BadRequestError("Platform ID collision detected") and silently
    // drops the disconnect event.
    ref map<string, string> m_BisToSteam;
    ref map<string, string> m_BisToName;

    void TakaroBridge()
    {
        m_Initialized = false;
        m_Registered = false;
        m_AccumFlush = 0;
        m_AccumPoll = 0;
        m_BisToSteam = new map<string, string>();
        m_BisToName = new map<string, string>();
    }

    void Initialize()
    {
        TakaroLog.Info("Bridge v" + VERSION + " starting");
        TakaroConfig.Load();
        TakaroConfigData cfg = TakaroConfig.Get();

        if (!TakaroConfig.IsConfigured())
        {
            TakaroLog.Warn("Bridge is unconfigured. Edit $profile:TakaroIntegration/config.json (TakaroApiUrl + IdentityToken or RegistrationToken) and restart.");
            return;
        }

        m_Http = new TakaroHttpClient(cfg.TakaroApiUrl, cfg.IdentityToken, cfg.RequestTimeoutSeconds);
        m_Queue = new TakaroEventQueue(2000);
        m_Dispatcher = new TakaroCommandDispatcher(m_Http);

        // Convert ms intervals to seconds for OnUpdate accumulators.
        m_FlushIntervalSec = Math.Max(1, cfg.EventBatchIntervalMs / 1000);
        m_PollIntervalSec = Math.Max(1, cfg.CommandPollIntervalMs / 1000);

        m_Initialized = true;

        if (cfg.IdentityToken != "" && cfg.GameServerId != "")
        {
            m_Registered = true;
            TakaroLog.Info("Already registered (gameserver=" + cfg.GameServerId + ")");
        }
        else if (cfg.RegistrationToken != "")
        {
            BootstrapRegistration();
        }
        else
        {
            TakaroLog.Warn("Have IdentityToken but no GameServerId — set GameServerId in config.json after registering this server in the Takaro dashboard.");
        }
    }

    // ---- Registration -------------------------------------------------

    private ref TakaroRegistrationCallback m_RegCallback;

    void BootstrapRegistration()
    {
        TakaroConfigData cfg = TakaroConfig.Get();
        TakaroLog.Info("Bootstrapping with registration token...");

        TakaroRegistrationRequest req = new TakaroRegistrationRequest();
        req.registrationToken = cfg.RegistrationToken;
        req.serverName = "DayZ Server";
        req.gameType = "dayz";
        req.bridgeVersion = VERSION;

        string body;
        JsonSerializer js = new JsonSerializer;
        js.WriteToString(req, false, body);

        m_RegCallback = new TakaroRegistrationCallback("register");
        m_RegCallback.m_Bridge = this;
        m_Http.Post("/gameserver/register", body, m_RegCallback);
    }

    void OnRegistrationComplete(bool ok, string body)
    {
        if (!ok)
        {
            TakaroLog.Error("Registration failed — staying offline. Body: " + body);
            return;
        }
        TakaroRegistrationResponse resp = new TakaroRegistrationResponse();
        string err;
        JsonSerializer js = new JsonSerializer;
        if (!js.ReadFromString(resp, body, err))
        {
            TakaroLog.Error("Could not parse registration response: " + err);
            return;
        }
        TakaroConfigData cfg = TakaroConfig.Get();
        cfg.IdentityToken = resp.identityToken;
        cfg.GameServerId = resp.gameServerId;
        cfg.RegistrationToken = "";
        TakaroConfig.Save();
        m_Http.UpdateAuth(resp.identityToken);
        m_Registered = true;
        TakaroLog.Info("Registration complete (gameserver=" + resp.gameServerId + ")");
    }

    // ---- Gameplay events ---------------------------------------------

    void OnPlayerConnected(PlayerBase player, PlayerIdentity identity)
    {
        if (!m_Initialized) return;
        if (!identity) return;
        m_Queue.Enqueue(TakaroEventFactory.Connected(identity));
        TakaroLog.Debug("event: player-connected " + identity.GetName());

        // Cache identity bits so disconnect can re-emit the same gameId/name
        // even when PlayerIdentity is null at logout time. Resolve the name
        // through TakaroNameCache so a bogus "Survivor" doesn't poison the
        // session cache when the persistent cache has the real name.
        string bisKey = identity.GetId();
        int eqIdx = bisKey.IndexOf("=");
        if (eqIdx >= 0) bisKey = bisKey.Substring(0, eqIdx);
        m_BisToSteam.Set(bisKey, identity.GetPlainId());
        m_BisToName.Set(bisKey, TakaroNameCache.Resolve(identity.GetPlainId(), identity.GetName()));
    }

    // identity + uid come straight from MissionServer.PlayerDisconnected;
    // player may already be null/deleted by the time we're called.
    void OnPlayerDisconnected(PlayerBase player, PlayerIdentity identity, string uid)
    {
        if (!m_Initialized) return;

        // Strip '=' padding from uid so it matches the cache key we wrote on
        // connect.
        string bisKey = uid;
        int eqIdx = bisKey.IndexOf("=");
        if (eqIdx >= 0) bisKey = bisKey.Substring(0, eqIdx);

        string cachedSteam = "";
        string cachedName = "";
        if (m_BisToSteam.Contains(bisKey)) cachedSteam = m_BisToSteam.Get(bisKey);
        if (m_BisToName.Contains(bisKey)) cachedName = m_BisToName.Get(bisKey);

        m_Queue.Enqueue(TakaroEventFactory.Disconnected(identity, player, uid, cachedSteam, cachedName));

        if (identity)
            TakaroLog.Info("event: player-disconnected " + identity.GetName());
        else
            TakaroLog.Info("event: player-disconnected uid=" + uid + " (cached steam=" + cachedSteam + ")");

        m_BisToSteam.Remove(bisKey);
        m_BisToName.Remove(bisKey);
    }

    void OnChatMessage(PlayerIdentity sender, string channel, string text)
    {
        if (!m_Initialized) return;
        if (!sender) return;
        m_Queue.Enqueue(TakaroEventFactory.Chat(sender, channel, text));
    }

    void OnPlayerKilled(PlayerBase victim, EntityAI killer, string weapon)
    {
        if (!m_Initialized) return;
        if (!victim) return;
        m_Queue.Enqueue(TakaroEventFactory.Death(victim, killer, weapon));
    }

    void OnRawLogLine(string line)
    {
        if (!m_Initialized) return;
        m_Queue.Enqueue(TakaroEventFactory.LogLine(line));
    }

    // ---- Tick --------------------------------------------------------

    void OnUpdate(float timeslice)
    {
        if (!m_Initialized) return;
        if (!m_Registered) return;

        m_AccumFlush += timeslice;
        m_AccumPoll += timeslice;

        if (m_AccumFlush >= m_FlushIntervalSec)
        {
            m_AccumFlush = 0;
            FlushEvents();
        }

        if (m_AccumPoll >= m_PollIntervalSec)
        {
            m_AccumPoll = 0;
            PollOperations();
        }
    }

    private ref TakaroFlushCallback m_FlushCallback;
    private ref array<string> m_InFlightBatch;

    void FlushEvents()
    {
        if (m_Queue.Count() == 0) return;
        if (m_FlushCallback && !m_FlushCallback.m_Done) return; // back-pressure

        TakaroConfigData cfg = TakaroConfig.Get();
        m_InFlightBatch = m_Queue.Drain(cfg.MaxEventsPerBatch);
        if (m_InFlightBatch.Count() == 0) return;

        // Each entry is already a complete JSON object for one event. Stitch
        // them into the batch envelope by hand so we don't lose per-type
        // field shape (JsonSerializer would re-emit empty fields).
        string body = "{\"events\":[";
        for (int i = 0; i < m_InFlightBatch.Count(); i++)
        {
            if (i > 0) body += ",";
            body += m_InFlightBatch[i];
        }
        body += "]}";

        if (cfg.DryRun)
        {
            TakaroLog.Info("[dry-run] Would POST " + m_InFlightBatch.Count().ToString() + " events");
            m_InFlightBatch = null;
            return;
        }

        m_FlushCallback = new TakaroFlushCallback("flush");
        m_FlushCallback.m_Bridge = this;

        string path = "/gameserver/" + cfg.GameServerId + "/events";
        m_Http.Post(path, body, m_FlushCallback);
    }

    void OnFlushComplete(bool ok)
    {
        if (!ok && m_InFlightBatch && m_InFlightBatch.Count() > 0)
        {
            TakaroLog.Warn("Flush failed — requeueing " + m_InFlightBatch.Count().ToString() + " events");
            m_Queue.Requeue(m_InFlightBatch);
        }
        m_InFlightBatch = null;
    }

    private ref TakaroPollCallback m_PollCallback;

    void PollOperations()
    {
        if (m_PollCallback && !m_PollCallback.m_Done) return;

        TakaroConfigData cfg = TakaroConfig.Get();
        if (cfg.GameServerId == "") return;

        m_PollCallback = new TakaroPollCallback("poll");
        m_PollCallback.m_Bridge = this;

        string path = "/gameserver/" + cfg.GameServerId + "/poll";
        m_Http.Get(path, m_PollCallback);
    }

    void OnPollComplete(bool ok, string body)
    {
        if (!ok || body == "") return;

        TakaroOperationsResponse resp = new TakaroOperationsResponse();
        string err;
        JsonSerializer js = new JsonSerializer;
        if (!js.ReadFromString(resp, body, err))
        {
            TakaroLog.Warn("Could not parse poll response: " + err);
            return;
        }
        if (resp.operations.Count() == 0) return;

        TakaroLog.Debug("Received " + resp.operations.Count().ToString() + " operations");
        for (int i = 0; i < resp.operations.Count(); i++)
            m_Dispatcher.Dispatch(resp.operations[i]);
    }
}

// ---- Registration request/response shapes -----------------------------

class TakaroRegistrationRequest
{
    string registrationToken;
    string serverName;
    string gameType;
    string bridgeVersion;
}

class TakaroRegistrationResponse
{
    string identityToken;
    string gameServerId;
}

class TakaroRegistrationCallback : TakaroHttpCallback
{
    TakaroBridge m_Bridge;

    void TakaroRegistrationCallback(string tag = "register")
    {
        m_Tag = tag;
    }

    override void OnComplete()
    {
        if (m_Bridge)
            m_Bridge.OnRegistrationComplete(m_Ok, m_ResponseBody);
    }
}

class TakaroFlushCallback : TakaroHttpCallback
{
    TakaroBridge m_Bridge;

    void TakaroFlushCallback(string tag = "flush")
    {
        m_Tag = tag;
    }

    override void OnComplete()
    {
        if (m_Bridge)
            m_Bridge.OnFlushComplete(m_Ok);
    }
}

class TakaroPollCallback : TakaroHttpCallback
{
    TakaroBridge m_Bridge;

    void TakaroPollCallback(string tag = "poll")
    {
        m_Tag = tag;
    }

    override void OnComplete()
    {
        if (m_Bridge)
            m_Bridge.OnPollComplete(m_Ok, m_ResponseBody);
    }
}
