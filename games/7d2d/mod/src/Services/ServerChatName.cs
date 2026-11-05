namespace Takaro.Services
{
    /// <summary>
    /// The sender name on chat the connector sends, with no game types so the
    /// contract harness can exercise it. Takaro's senderNameOverride (filled
    /// from the server's serverChatName setting by newer Takaro releases) wins
    /// over Config.xml's ServerChatName; neither means the game's own "Server".
    /// </summary>
    public static class ServerChatName
    {
        /// <summary>The name to show, or null for the game's own "Server".</summary>
        public static string Resolve(string senderNameOverride, string configuredName)
        {
            string name = Clean(senderNameOverride);
            return name ?? Clean(configuredName);
        }

        /// <summary>
        /// The chat text. The client prints no sender label for a message sent
        /// without a sender, so the name goes in front of the text instead; a
        /// player then reads "X: message" (global) or "From X: message" (whisper).
        /// </summary>
        public static string Render(string senderName, string message)
        {
            return senderName == null ? message : senderName + ": " + message;
        }

        private static string Clean(string name)
        {
            if (name == null)
                return null;
            name = name.Trim();
            return name == "" ? null : name;
        }
    }
}
