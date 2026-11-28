// Best-effort chat capture.
//
// Vanilla DayZ does not expose a clean server-side "chat sent" hook in script —
// chat is routed through the engine's ChatPlayer / chat RPC pipeline.
// CommunityFramework adds a global event we can subscribe to; if CF isn't
// loaded we fall back to a no-op.
//
// We declare a TakaroChatRouter that other code can call directly. The CF
// listener (if active) will hand chat lines here. Mods that already have a
// handle on chat (Expansion, VPP) can also call TakaroChatRouter.Route().
//
// The class is server-side only; it's compiled into the missionScriptModule
// which runs on the server when this mod is loaded with -serverMod=.

class TakaroChatRouter
{
    static void Route(PlayerIdentity sender, string channel, string text)
    {
        if (!sender) return;
        if (text == "") return;

        TakaroBridge bridge = TakaroBridge.Cast(TakaroBridgeAccessor.Get());
        if (!bridge) return;

        bridge.OnChatMessage(sender, channel, text);
    }
}
