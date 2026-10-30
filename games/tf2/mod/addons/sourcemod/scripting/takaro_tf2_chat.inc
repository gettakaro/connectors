// Chat helpers: the sender name for messages Takaro sends into TF2 chat, and which player chat the
// game really delivered. Kept free of JSON and ConVars so games/tf2/tests/takaro_tf2_chat_test.sp can
// test them directly.

#if defined _takaro_tf2_chat_included
  #endinput
#endif
#define _takaro_tf2_chat_included

#define TAKARO_CHAT_DEFAULT_SENDER "Takaro"
#define MAX_SERVER_CHAT_NAME_LENGTH 64
// A player's say counts as delivered when the game sends its SayText2 within this many seconds. The
// game sends it during the say command itself; chat plugins that rewrite chat send theirs a frame later.
#define CHAT_DELIVERY_WINDOW 1.0

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

// TF2 rate-limits player chat (sv_chat_bucket_size_tier*, sv_chat_seconds_per_msg_tier) and plugins can
// block it, so a say command is not proof that anyone saw the message. A SayText2 whose author has a
// say waiting confirms it. Player chat formats are TF_Chat_All, TF_Chat_Team, ...; engine notices
// such as "#TF_Name_Change" also use SayText2 with the player as author and start with '#'.
stock bool ConfirmsPlayerChat(const char[] format, float saidAt, float now)
{
  return format[0] != '#' && saidAt >= 0.0 && now - saidAt <= CHAT_DELIVERY_WINDOW;
}
