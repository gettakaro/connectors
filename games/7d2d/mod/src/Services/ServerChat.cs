using Takaro.Config;

namespace Takaro.Services
{
    /// <summary>
    /// Sends chat as the server, with the sender name from ServerChatName. Every
    /// connector message that a player sees as server chat goes through here.
    /// Call on the game main thread.
    /// </summary>
    public static class ServerChat
    {
        public static void SendGlobal(string message, string senderNameOverride)
        {
            string senderName = ResolveName(senderNameOverride);
            string rendered = ServerChatName.Render(senderName, message);
            // The game logs the line as non-player Global chat; the guard keeps
            // that log line from coming back to Takaro.
            ServerMessageEchoGuard.Instance.Record(rendered);
            GameManager.Instance.ChatMessageServer(
                null,
                EChatType.Global,
                -1,
                rendered,
                null,
                SenderType(senderName)
            );
        }

        public static void SendWhisper(
            ClientInfo recipient,
            string message,
            string senderNameOverride
        )
        {
            string senderName = ResolveName(senderNameOverride);
            recipient.SendPackage(
                NetPackageManager
                    .GetPackage<NetPackageChat>()
                    .Setup(
                        EChatType.Whisper,
                        -1,
                        ServerChatName.Render(senderName, message),
                        null,
                        SenderType(senderName),
                        GeneratedTextManager.BbCodeSupportMode.Supported
                    )
            );
        }

        private static string ResolveName(string senderNameOverride)
        {
            return ServerChatName.Resolve(
                senderNameOverride,
                ConfigManager.Instance.ServerChatName
            );
        }

        // Server makes the client print its own "Server" label; None prints no
        // label, so the name in the text reads as the sender.
        private static EMessageSender SenderType(string senderName)
        {
            return senderName == null ? EMessageSender.Server : EMessageSender.None;
        }
    }
}
