// Research fixture for gettakaro/connectors#197: a "foreign" plugin that owns the chat
// command /probedup, so the probe's duplicate-root handling can be observed, and a sink
// that logs every chat and command hook a chat bridge could listen to. Never deployed by
// any script.

using ConVar;

namespace Oxide.Plugins
{
    [Info("TakaroCollisionFixture", "Takaro", "0.0.1")]
    [Description("Research fixture for issue #197: owns /probedup and logs chat/command hooks.")]
    public class TakaroCollisionFixture : RustPlugin
    {
        private const string Tag = "[Fixture]";

        private void Init()
        {
            cmd.AddChatCommand("probedup", this, (player, command, args) =>
            {
                Puts($"{Tag} probedup ran for {player?.UserIDString}");
                player?.ChatMessage($"{Tag} FIXTURE-OK /probedup still belongs to the fixture");
            });
            Puts($"{Tag} registered /probedup");
        }

        // A chat bridge (Discord, the Takaro connector's chat-message event) listens here.
        private object OnPlayerChat(BasePlayer player, string message, Chat.ChatChannel channel)
        {
            Puts($"{Tag} sink OnPlayerChat player={player?.UserIDString} channel={channel} msg={message}");
            return null;
        }

        // Bridges that log commands listen here; these hooks see command input before dispatch.
        private object OnPlayerCommand(BasePlayer player, string command, string[] args)
        {
            Puts($"{Tag} sink OnPlayerCommand player={player?.UserIDString} command={command} args={string.Join("|", args ?? new string[0])}");
            return null;
        }
    }
}
