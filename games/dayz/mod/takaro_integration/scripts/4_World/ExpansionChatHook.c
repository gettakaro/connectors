// modded class must live in the same script pass as the original class
// definition. ExpansionGlobalChatModule is declared in 4_World, so this hook
// also lives in 4_World. We can't reference TakaroChatRouter (5_Mission) from
// here, so we go through TakaroBridgeAccessor — the same cross-pass holder
// pattern PlayerBaseTakaro.c uses to reach the bridge from 4_World.
#ifdef EXPANSIONMOD
modded class ExpansionGlobalChatModule
{
    override void AddChatMessage_Server(
        PlayerIdentity sender, Object target,
        ParamsReadContext ctx, ExpansionChatMessageEventParams data)
    {
        // Intercept Takaro chat commands BEFORE super broadcasts the message.
        // If the trimmed message starts with '/' we treat it as a command:
        // forward it to Takaro for processing and skip super entirely so it
        // never hits the chat RPC (other players don't see the command text)
        // and never fires g_Game.GetMission().OnEvent (so the vanilla OnEvent
        // hook in MissionServerTakaro doesn't double-forward to Takaro).
        if (sender && data && data.param3 != "")
        {
            string trimmed = data.param3;
            trimmed.TrimInPlace();
            if (trimmed != "" && trimmed.IndexOf("/") == 0)
            {
                // Whisper an immediate ack so the player sees their command
                // was received even if Takaro's response is delayed or the
                // command isn't registered.
                //
                // Delivery is server→client-only via Expansion_Send(true,
                // sender) — no other player ever receives this RPC. Channel
                // is CCDirect so the recipient's chat UI renders it without
                // a "Global:" decoration (PM feel). CCDirect's only
                // client-side gate is GetProfileOption(PLAYER_MESSAGES)
                // in ExpansionChatUIWindow.Add, the same gate that already
                // lets /link PMs through, so we know it passes for our
                // players.
                //
                // Avoid CCTransport: Expansion's client-side
                // AddChatMessage_Client (ExpansionGlobalChatModule.c
                // l.189-196) silently drops CCTransport unless the
                // recipient is in a vehicle whose entity matches the RPC
                // target. CCTeam is similarly party-gated.
                //
                // Color is overridden via param4 ("colorAction" →
                // ActionMessageColor → COLOR_YELLOW). The channel would
                // otherwise dictate color (CCDirect → DirectChatColor =
                // white); there's no "gold" preset reachable outside
                // CCTransport without editing ChatSettings.json. param4
                // is read on the client at
                // ExpansionChatUIWindow.AddInternal → message.SetColorByName.
                int sp = trimmed.IndexOf(" ");
                string verb = trimmed;
                if (sp > 0) verb = trimmed.Substring(0, sp);
                ExpansionGlobalChatModule ackMod;
                if (CF_Modules<ExpansionGlobalChatModule>.Get(ackMod))
                {
                    ExpansionChatMessageEventParams ackData =
                        new ExpansionChatMessageEventParams(
                            ExpansionChatChannels.CCDirect, "", "command received " + verb, "colorAction", "");
                    auto ackRpc = ackMod.Expansion_CreateRPC("RPC_AddChatMessage");
                    ackRpc.Write(ackData);
                    ackRpc.Expansion_Send(true, sender);
                }

                TakaroBridge bridge = TakaroBridge.Cast(TakaroBridgeAccessor.Get());
                if (bridge)
                    bridge.OnChatMessage(sender, "global", trimmed);
                return;
            }
        }

        super.AddChatMessage_Server(sender, target, ctx, data);
    }
}
#endif
