// Host-side unit tests for everything that does not need a game process: the bridge protocol,
// the ChatRpcData layout, text conversion, HTTP framing and the ELF build-id reader.
#include "common.h"
#include "proto.h"

#include <cstdio>
#include <cstring>
#include <string>

static int g_failed = 0, g_ran = 0;

#define CHECK(cond, ...)                                           \
    do {                                                           \
        g_ran++;                                                   \
        if (!(cond)) {                                             \
            g_failed++;                                            \
            printf("FAIL %s:%d: %s\n     ", __FILE__, __LINE__, #cond); \
            printf(__VA_ARGS__);                                   \
            printf("\n");                                          \
        }                                                          \
    } while (0)
#define EQ(a, b) CHECK((a) == (b), "got '%s' want '%s'", std::string(a).c_str(), std::string(b).c_str())

static void TestParsePoll() {
    PollCommand c;
    std::string err;
    CHECK(ParsePoll("{\"hasCommand\":false}", c, err) && !c.has, "empty poll");

    CHECK(ParsePoll("{\"hasCommand\":true,\"command\":{\"requestId\":\"r1\",\"action\":\"sendMessage\",\"args\":"
                    "{\"message\":\"hi \\u00e9\",\"recipient\":\"76561198000000001\"}}}",
                    c, err),
          "%s", err.c_str());
    CHECK(c.has, "has");
    EQ(c.requestId, "r1");
    EQ(c.action, "sendMessage");
    EQ(c.message, "hi \xc3\xa9");
    EQ(c.recipient, "76561198000000001");
    EQ(c.sender, "Takaro");

    CHECK(ParsePoll("{\"hasCommand\":true,\"command\":{\"requestId\":\"r2\",\"action\":\"sendMessage\",\"args\":"
                    "{\"message\":\"m\",\"recipient\":\"platform:steam:76561198000000002\","
                    "\"senderNameOverride\":\"  Discord Bob \"}}}",
                    c, err),
          "%s", err.c_str());
    EQ(c.recipient, "76561198000000002");
    EQ(c.sender, "Discord Bob");

    CHECK(ParsePoll("{\"hasCommand\":true,\"command\":{\"requestId\":\"r3\",\"action\":\"sendMessage\",\"args\":"
                    "{\"message\":\"m\",\"recipient\":\" werwerwer \",\"senderNameOverride\":\"   \"}}}",
                    c, err),
          "%s", err.c_str());
    EQ(c.recipient, "werwerwer");
    EQ(c.sender, "Takaro");

    CHECK(ParsePoll("{\"hasCommand\":true,\"command\":{\"requestId\":\"r4\",\"action\":\"kickPlayer\"}}", c, err) &&
              c.has && c.message.empty() && c.recipient.empty(),
          "command without args");

    CHECK(!ParsePoll("not json", c, err), "malformed");
    CHECK(!ParsePoll("{}", c, err), "no hasCommand");
    CHECK(!ParsePoll("{\"hasCommand\":true}", c, err), "no command");
    CHECK(!ParsePoll("{\"hasCommand\":true,\"command\":{\"action\":\"sendMessage\"}}", c, err), "no requestId");
}

static void TestResultBody() {
    ChatOutcome ok;
    ok.success = true;
    ok.delivered = 2;
    JsonValue v;
    CHECK(JsonParse(ResultBody("id-1", ok), v), "valid json");
    EQ(v.get("requestId")->str, "id-1");
    const JsonValue* r = v.get("result");
    CHECK(r && r->get("success")->b && !r->get("error"), "success shape");
    EQ(r->get("delivered")->str, "2");
    EQ(r->get("transport")->str, "TakaroConan-native");

    ChatOutcome bad;
    bad.error = "Recipient \"x\" is not online";
    CHECK(JsonParse(ResultBody("id-2", bad), v), "valid json");
    r = v.get("result");
    CHECK(r && !r->get("success")->b, "failure");
    EQ(r->get("error")->str, "Recipient \"x\" is not online");
}

static void TestRecipient() {
    CHECK(IsSteam64("76561198000000001"), "steam64");
    CHECK(!IsSteam64("7656119800000000"), "short");
    CHECK(!IsSteam64("86561198000000001"), "prefix");
    CHECK(!IsSteam64("7656119800000000a"), "digits");
    EQ(NormalizeRecipient("steam:76561198000000001"), "76561198000000001");
    EQ(NormalizeRecipient(" 76561198000000001 "), "76561198000000001");
    EQ(NormalizeRecipient("some:name"), "some:name");
    EQ(NormalizeRecipient(""), "");
    CHECK(EqualsIgnoreCase("WerWer", "werwer") && !EqualsIgnoreCase("werwer", "werwe"), "ignore case");
}

static void TestText() {
    CHECK(ChatText("hello") == u"hello", "ascii");
    CHECK(ChatText("h\xc3\xa9llo") == u"h\u00e9llo", "2-byte");
    CHECK(ChatText("\xe2\x82\xac") == u"\u20ac", "3-byte");
    CHECK(ChatText("\xf0\x9f\x98\x80") == u"\U0001F600", "4-byte -> surrogate pair");
    CHECK(ChatText("a\xffz") == u"a\uFFFDz", "invalid byte");
    CHECK(ChatText("a\xc3") == u"a\uFFFD", "truncated sequence");
    CHECK(ChatText("\xc0\xaf") == u"\uFFFD\uFFFD", "overlong");
    CHECK(ChatText("\xed\xa0\x80") == u"\uFFFD\uFFFD\uFFFD", "encoded surrogate");
    CHECK(ChatText("a\nb\tc\rd") == u"a b c d", "whitespace");
    CHECK(ChatText(std::string("a\x01" "b\x7f" "c")) == u"abc", "control chars dropped");
    CHECK(ChatText(std::string(5000, 'x')).size() == kMaxMessageChars, "capped");
    CHECK(ChatText("abc\xf0\x9f\x98\x80", 4) == u"abc", "no half surrogate at the cap");
    EQ(Utf16To8(u"h\u00e9\u20ac\U0001F600"), "h\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80");
    EQ(Utf16To8(std::u16string(1, (char16_t)0xD800)), "\xef\xbf\xbd");
}

static void TestChatRpc() {
    CHECK(FileTimeTicks(0, 0) == 116444736000000000ULL, "unix epoch");
    CHECK(FileTimeTicks(1, 500) == 116444736000000000ULL + 10000000ULL + 5, "1s + 500ns");
    // The spike captured a real player message at 0x01dd529b80c4e16f on 2026-10-02 (UTC).
    uint64_t real = 0x01dd529b80c4e16fULL;
    int64_t sec = (int64_t)((real - FileTimeTicks(0, 0)) / 10000000ULL);
    CHECK(sec >= 1790899200 && sec < 1790985600, "captured timestamp is 2026-10-02: %lld", (long long)sec);

    std::u16string user = u"Takaro", channel = u"Global", msg = u"hello";
    alignas(16) uint8_t buf[ChatRpc::kSize];
    memset(buf, 0xAB, sizeof buf);
    PackChatRpc(buf, 0x1122334455667788ULL, user, channel, msg);
    uint64_t ts;
    memcpy(&ts, buf, 8);
    CHECK(ts == 0x1122334455667788ULL, "timestamp");
    for (size_t i = 8; i < ChatRpc::kUserName; i++) CHECK(buf[i] == 0, "ids zeroed at %zu", i);
    struct { size_t off; const std::u16string* s; } fields[] = {
        {ChatRpc::kUserName, &user}, {ChatRpc::kChannel, &channel}, {ChatRpc::kMessage, &msg}};
    for (auto& f : fields) {
        const char16_t* p;
        int32_t num, max;
        memcpy(&p, buf + f.off, 8);
        memcpy(&num, buf + f.off + 8, 4);
        memcpy(&max, buf + f.off + 12, 4);
        CHECK(p == f.s->c_str(), "FString data at %zx", f.off);
        CHECK(num == (int32_t)f.s->size() + 1 && max == num, "FString Num/Max include NUL at %zx", f.off);
        CHECK(p[num - 1] == 0, "NUL terminated");
    }
    CHECK(buf[ChatRpc::kGenerated] == 0, "generated false");
    for (size_t i = ChatRpc::kGenerated + 1; i < ChatRpc::kSize; i++) CHECK(buf[i] == 0, "tail zeroed");
}

static void TestUrl() {
    HttpUrl u;
    CHECK(ParseHttpUrl("http://127.0.0.1:3010", u) && u.host == "127.0.0.1" && u.port == 3010 && u.basePath.empty(),
          "host:port");
    CHECK(ParseHttpUrl("http://bridge/base/", u) && u.host == "bridge" && u.port == 80 && u.basePath == "/base",
          "default port and base path");
    CHECK(!ParseHttpUrl("https://127.0.0.1:3010", u), "https refused");
    CHECK(!ParseHttpUrl("http://:3010", u), "no host");
    CHECK(!ParseHttpUrl("http://h:99999", u), "port range");
    CHECK(!ParseHttpUrl("http://h:12a", u), "port digits");
}

static void TestHttpResponse() {
    int status = 0;
    std::string body;
    CHECK(ParseHttpResponse("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 5\r\n\r\nhelloEXTRA",
                            status, body) &&
              status == 200 && body == "hello",
          "content-length");
    CHECK(ParseHttpResponse("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n2;x=y\r\nde\r\n0\r\n\r\n",
                            status, body) &&
              body == "abcde",
          "chunked");
    CHECK(ParseHttpResponse("HTTP/1.1 404 Not Found\r\nconnection: close\r\n\r\n{\"error\":1}", status, body) &&
              status == 404 && body == "{\"error\":1}",
          "close-delimited");
    CHECK(!ParseHttpResponse("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nshort", status, body), "short body");
    CHECK(!ParseHttpResponse("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nab", status, body),
          "short chunk");
    CHECK(!ParseHttpResponse("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabcXY0\r\n\r\n", status, body),
          "chunk data not followed by CRLF");
    CHECK(!ParseHttpResponse("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3zz\r\nabc\r\n0\r\n\r\n", status, body),
          "garbage after chunk size");
    CHECK(!ParseHttpResponse("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n0\r\n", status, body),
          "truncated after the last chunk");
    CHECK(!ParseHttpResponse("garbage\r\n\r\n", status, body), "not http");
    CHECK(!ParseHttpResponse("HTTP/1.1 200 OK\r\n", status, body), "no header end");
}

static void TestBuildId() {
    std::string id = ReadElfBuildId("/proc/self/exe");
    CHECK(id.size() == 40 && id.find_first_not_of("0123456789abcdef") == std::string::npos, "own build-id '%s'",
          id.c_str());
    CHECK(ReadElfBuildId("/nonexistent").empty(), "missing file");
    CHECK(ReadElfBuildId("/proc/self/status").empty(), "not ELF");
}

int main() {
    TestParsePoll();
    TestResultBody();
    TestRecipient();
    TestText();
    TestChatRpc();
    TestUrl();
    TestHttpResponse();
    TestBuildId();
    printf("%d/%d checks passed\n", g_ran - g_failed, g_ran);
    return g_failed ? 1 : 0;
}
