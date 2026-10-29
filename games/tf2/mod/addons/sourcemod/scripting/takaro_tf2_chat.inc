// Sender name for messages Takaro sends into TF2 chat. Kept free of JSON and ConVars so
// games/tf2/tests/takaro_tf2_chat_test.sp can test it directly.

#if defined _takaro_tf2_chat_included
  #endinput
#endif
#define _takaro_tf2_chat_included

#define TAKARO_CHAT_DEFAULT_SENDER "Takaro"
#define MAX_SERVER_CHAT_NAME_LENGTH 64

// Takaro's opts.senderNameOverride wins, then the takaro_tf2_server_chat_name cvar. Both are
// trimmed; an empty or whitespace-only value counts as unset. Returns false when neither is set.
stock bool ResolveServerChatName(const char[] senderNameOverride, const char[] configuredName, char[] buffer, int maxlen)
{
  strcopy(buffer, maxlen, senderNameOverride);
  TrimString(buffer);
  if (buffer[0] != '\0') {
    return true;
  }

  strcopy(buffer, maxlen, configuredName);
  TrimString(buffer);
  return buffer[0] != '\0';
}

// The one renderer for every Takaro chat line, global and private: "[<sender>] <message>" with the
// sender in green. Without a resolved name the sender stays "Takaro".
stock void FormatServerChatMessage(const char[] senderName, const char[] message, char[] buffer, int maxlen)
{
  if (senderName[0] == '\0') {
    Format(buffer, maxlen, "\x04[%s]\x01 %s", TAKARO_CHAT_DEFAULT_SENDER, message);
  } else {
    Format(buffer, maxlen, "\x04[%s]\x01 %s", senderName, message);
  }
}
