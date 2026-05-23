// Hooks into MissionServer's lifecycle and the vanilla connect/disconnect path.
// We extend the existing MissionServer rather than replace it so other mods
// (CF, VPP, Expansion) keep working.

modded class MissionServer
{
    ref TakaroBridge m_TakaroBridge;
    static const float TAKARO_TICK_INTERVAL = 1.0;
    float m_TakaroTickAccum;

    override void OnInit()
    {
        super.OnInit();

        if (!m_TakaroBridge)
        {
            m_TakaroBridge = new TakaroBridge();
            m_TakaroBridge.Initialize();
            TakaroBridgeAccessor.Set(m_TakaroBridge);
            TakaroLog.Info("Bridge attached to MissionServer.OnInit");
        }
    }

    override void OnUpdate(float timeslice)
    {
        super.OnUpdate(timeslice);

        // We don't want to spam the bridge every frame; the bridge itself only
        // does work when its internal accumulators trip, so passing timeslice
        // every tick is fine. But we throttle here to be defensive.
        m_TakaroTickAccum += timeslice;
        if (m_TakaroTickAccum >= TAKARO_TICK_INTERVAL)
        {
            if (m_TakaroBridge)
                m_TakaroBridge.OnUpdate(m_TakaroTickAccum);
            m_TakaroTickAccum = 0;
        }
    }

    override void InvokeOnConnect(PlayerBase player, PlayerIdentity identity)
    {
        super.InvokeOnConnect(player, identity);
        if (m_TakaroBridge && identity)
            m_TakaroBridge.OnPlayerConnected(player, identity);
    }

    // PlayerDisconnected is the reliable hook — InvokeOnDisconnect is called
    // by it but only when the player character still exists, which is often
    // not the case once the logout timer expires. Hook the outer call.
    override void PlayerDisconnected(PlayerBase player, PlayerIdentity identity, string uid)
    {
        if (m_TakaroBridge)
            m_TakaroBridge.OnPlayerDisconnected(player, identity, uid);
        super.PlayerDisconnected(player, identity, uid);
    }

    // Vanilla chat hook. Server-side OnEvent fires for ChatMessageEventTypeID
    // when a player sends chat (any channel). param1=channel, param2=name,
    // param3=text, param4=color tag.
    override void OnEvent(EventType eventTypeId, Param params)
    {
        super.OnEvent(eventTypeId, params);
        if (eventTypeId != ChatMessageEventTypeID) return;
        if (!m_TakaroBridge) return;

        ChatMessageEventParams cm = ChatMessageEventParams.Cast(params);
        if (!cm) return;
        string channelName = ChannelToString(cm.param1);
        string senderName = cm.param2;
        string text = cm.param3;
        if (text == "") return;

        // Find sender's PlayerIdentity by name (vanilla chat event doesn't
        // give us the identity directly).
        PlayerIdentity sender = FindIdentityByName(senderName);
        if (sender)
            m_TakaroBridge.OnChatMessage(sender, channelName, text);
    }

    private string ChannelToString(int channel)
    {
        // DayZ channels: CCDirect=0, CCGlobal=1, CCSystem=2, CCAdmin=3,
        // CCRadio=4, CCTransmitter=5. Takaro's ChatChannel enum is strictly
        // global|team|friends|whisper — anything else fails IsEnum. Direct
        // (proximity) chat is closest to "global" semantically; collapse all
        // DayZ channels to "global" so events pass DTO validation.
        return "global";
    }

    private PlayerIdentity FindIdentityByName(string name)
    {
        array<Man> all = new array<Man>;
        GetGame().GetPlayers(all);
        for (int i = 0; i < all.Count(); i++)
        {
            PlayerBase pb = PlayerBase.Cast(all[i]);
            if (!pb) continue;
            PlayerIdentity id = pb.GetIdentity();
            if (id && id.GetName() == name) return id;
        }
        return null;
    }

    // Helper accessor so other modules can route events into the bridge.
    static TakaroBridge GetTakaroBridge()
    {
        MissionServer ms = MissionServer.Cast(GetGame().GetMission());
        if (!ms) return null;
        return ms.m_TakaroBridge;
    }
}
