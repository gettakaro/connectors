#include "http.h"

#include "actions.h"
#include "admin.h"
#include "events.h"
#include "gamethread.h"
#include "native_bridge.h"
#include "perf.h"
#include "hooks.h"
#include "reflect.h"
#include "state.h"
#include "resolve.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <memory>

namespace {

// Monotonic plugin start, so /health.uptimeMs is the plugin's age, not the host's.
uint64_t StartMs() {
    static const uint64_t t0 = NowMs();
    return t0;
}

int g_port = 18890;
std::string g_token;
std::atomic<uint64_t> g_requests{0};
std::atomic<uint64_t> g_unauthorized{0};
std::atomic<uint64_t> g_errors{0};

struct Request {
    std::string method, path, query, body;
    std::map<std::string, std::string> headers;  // lower-case keys
};
struct Response {
    int status = 200;
    std::string body;
};

std::string Lower(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

std::string UrlDecode(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size()) {
            o += (char)strtol(s.substr(i + 1, 2).c_str(), nullptr, 16);
            i += 2;
        } else if (s[i] == '+') {
            o += ' ';
        } else {
            o += s[i];
        }
    }
    return o;
}

std::string QueryParam(const std::string& q, const std::string& name) {
    size_t pos = 0;
    while (pos <= q.size()) {
        size_t amp = q.find('&', pos);
        std::string kv = q.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        size_t eq = kv.find('=');
        if (UrlDecode(kv.substr(0, eq)) == name) return eq == std::string::npos ? "" : UrlDecode(kv.substr(eq + 1));
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return "";
}

const char* Reason(int s) {
    switch (s) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 409: return "Conflict";
        case 413: return "Payload Too Large";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 503: return "Service Unavailable";
        default: return "OK";
    }
}

Response Err(int status, const std::string& msg) { return {status, "{\"error\":" + JsonStr(msg) + "}"}; }

bool ReadRequest(int c, Request& r, int& errStatus) {
    std::string buf;
    char tmp[4096];
    size_t headerEnd = std::string::npos;
    while (headerEnd == std::string::npos) {
        ssize_t n = recv(c, tmp, sizeof tmp, 0);
        if (n <= 0) return false;
        buf.append(tmp, (size_t)n);
        headerEnd = buf.find("\r\n\r\n");
        if (buf.size() > 64 * 1024 && headerEnd == std::string::npos) { errStatus = 413; return false; }
    }
    std::string head = buf.substr(0, headerEnd);
    size_t lineEnd = head.find("\r\n");
    std::string reqLine = head.substr(0, lineEnd);
    size_t s1 = reqLine.find(' '), s2 = reqLine.rfind(' ');
    if (s1 == std::string::npos || s2 == s1) { errStatus = 400; return false; }
    r.method = reqLine.substr(0, s1);
    std::string target = reqLine.substr(s1 + 1, s2 - s1 - 1);
    size_t q = target.find('?');
    r.path = target.substr(0, q);
    if (q != std::string::npos) r.query = target.substr(q + 1);
    size_t pos = lineEnd == std::string::npos ? head.size() : lineEnd + 2;
    while (pos < head.size()) {
        size_t e = head.find("\r\n", pos);
        std::string line = head.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
        size_t colon = line.find(':');
        if (colon != std::string::npos) {
            size_t vs = colon + 1;
            while (vs < line.size() && line[vs] == ' ') vs++;
            r.headers[Lower(line.substr(0, colon))] = line.substr(vs);
        }
        if (e == std::string::npos) break;
        pos = e + 2;
    }
    size_t contentLength = 0;
    auto it = r.headers.find("content-length");
    if (it != r.headers.end()) contentLength = (size_t)strtoull(it->second.c_str(), nullptr, 10);
    if (contentLength > 1024 * 1024) { errStatus = 413; return false; }
    r.body = buf.substr(headerEnd + 4);
    while (r.body.size() < contentLength) {
        ssize_t n = recv(c, tmp, sizeof tmp, 0);
        if (n <= 0) return false;
        r.body.append(tmp, (size_t)n);
    }
    if (r.body.size() > contentLength) r.body.resize(contentLength);
    return true;
}

void Send(int c, const Response& resp) {
    std::string out = "HTTP/1.1 " + std::to_string(resp.status) + " " + Reason(resp.status) +
                      "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(resp.body.size()) +
                      "\r\nConnection: close\r\n\r\n" + resp.body;
    size_t sent = 0;
    while (sent < out.size()) {
        ssize_t n = send(c, out.data() + sent, out.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) break;
        sent += (size_t)n;
    }
}

bool TokenOk(const Request& r) {
    if (g_token.empty()) return false;
    auto it = r.headers.find("authorization");
    if (it == r.headers.end()) return false;
    const std::string& v = it->second;
    if (v.size() < 7 || Lower(v.substr(0, 7)) != "bearer ") return false;
    std::string t = v.substr(7);
    while (!t.empty() && (t.back() == ' ' || t.back() == '\r')) t.pop_back();
    if (t.size() != g_token.size()) return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < t.size(); i++) diff |= (unsigned char)(t[i] ^ g_token[i]);
    return diff == 0;
}

std::vector<std::string> Split(const std::string& path) {
    std::vector<std::string> parts;
    size_t pos = 0;
    while (pos < path.size()) {
        size_t s = path.find('/', pos);
        std::string seg = path.substr(pos, s == std::string::npos ? std::string::npos : s - pos);
        if (!seg.empty()) parts.push_back(UrlDecode(seg));
        if (s == std::string::npos) break;
        pos = s + 1;
    }
    return parts;
}

// ---- handlers ---------------------------------------------------------------------------------

Response Health() {
    auto& st = PluginState::Get();
    std::string selfChecks = "[";
    bool allOk = true;
    auto checks = Resolve::SelfChecks();
    for (size_t i = 0; i < checks.size(); i++) {
        if (i) selfChecks += ",";
        selfChecks += "{\"check\":" + JsonStr(checks[i].first) + ",\"ok\":" + (checks[i].second ? "true" : "false") + "}";
        allOk = allOk && checks[i].second;
    }
    selfChecks += "]";

    std::string status = "ok";
    if (!allOk || st.Capability("reflection") != "ok") status = "degraded";
    const std::string nativeHealth = NativeBridge::HealthJson();
    JsonValue native;
    if (!JsonParse(nativeHealth, native)) status = "degraded";
    else {
        const JsonValue* connection = native.get("connection");
        const JsonValue* identified = connection ? connection->get("identified") : nullptr;
        const JsonValue* persistenceError = native.get("persistenceLastError");
        const JsonValue* recovery = native.get("banRecoveryPending");
        const JsonValue* banMetadataError = native.get("banMetadataError");
        const JsonValue* behavior = native.get("behavior");
        const JsonValue* logError = behavior ? behavior->get("logParserError") : nullptr;
        if (!identified || identified->type != JsonValue::Bool || !identified->b ||
            (persistenceError && persistenceError->isStr() && !persistenceError->str.empty()) ||
            (recovery && recovery->type == JsonValue::Bool && recovery->b) ||
            (banMetadataError && banMetadataError->isStr() && !banMetadataError->str.empty()) ||
            (logError && logError->isStr() && !logError->str.empty()))
            status = "degraded";
    }
    if (!state::BanPersistenceError().empty()) status = "degraded";

    StartMs();
    std::string o = "{\"status\":" + JsonStr(status) + ",\"version\":\"" TAKARO_PLUGIN_VERSION "\"" +
                    ",\"bootId\":" + JsonStr(BootId()) + ",\"pid\":" + std::to_string(getpid()) +
                    ",\"gameBuild\":" + JsonStr(Reflect::GameBuild()) +
                    ",\"engineVersion\":" + JsonStr(Reflect::EngineVersion()) +
                    ",\"buildId\":" + JsonStr(Elf().buildId) +
                    ",\"uptimeMs\":" + std::to_string(NowMs() - StartMs()) +
                    ",\"capabilities\":" + st.CapabilitiesJson() +
                    ",\"capabilityDetails\":" + st.CapabilityDetailsJson() +
                    ",\"symCache\":" + Resolve::CacheJson() +
                    ",\"native\":" + nativeHealth +
                    ",\"diagnostics\":{\"resolve\":" + Resolve::StatsJson() + ",\"selfChecks\":" + selfChecks +
                    ",\"reflect\":" + Reflect::LayoutJson() + ",\"gameThread\":" + GameThread::StatsJson() +
                    ",\"perf\":" + Perf::Json() +
                    ",\"http\":" + Http::StatsJson() +
                    ",\"eventSources\":" + Events::DiagnosticsJson() +
                    ",\"events\":{\"buffered\":" + std::to_string(st.Buffered()) +
                    ",\"latestSeq\":" + std::to_string(st.LatestSeq()) + "}" +
                    ",\"admins\":" + Admin::DiagnosticsJson() +
                    ",\"hooksInstalled\":" + std::to_string(Hooks::InstalledCount()) +
                    ",\"resolved\":" + Hooks::ResolvedJson() + "}}";
    return {200, o};
}

Response DebugGameThread() {
    if (!GameThread::Alive() && GameThread::TickCount() == 0)
        return Err(503, "the game thread pump has not ticked yet");
    uint64_t t0 = NowMs();
    auto ranOn = std::make_shared<long>(0);
    bool ok = GameThread::Run([ranOn] { *ranOn = (long)syscall(SYS_gettid); }, 5000);
    if (!ok) return Err(503, "game-thread job timed out");
    return {200, "{\"ranOnThreadId\":" + std::to_string(*ranOn) + ",\"httpThreadId\":" +
                     std::to_string((long)syscall(SYS_gettid)) + ",\"latencyMs\":" + std::to_string(NowMs() - t0) +
                     ",\"stats\":" + GameThread::StatsJson() + "}"};
}

Response DebugSymbols() {
    return {200, "{\"elf\":{\"exe\":" + JsonStr(ExePath()) + ",\"loadBase\":" + std::to_string(Elf().loadBase) +
                     ",\"buildId\":" + JsonStr(Elf().buildId) + "},\"symCache\":" + Resolve::CacheJson() +
                     ",\"stats\":" + Resolve::StatsJson() + ",\"symbols\":" + Hooks::ResolvedJson() + "}"};
}

Response DebugObject(const Request& r) {
    std::string path = QueryParam(r.query, "path");
    std::string ptr = QueryParam(r.query, "ptr");
    if (path.empty() && ptr.empty()) return Err(400, "pass ?path=/Script/Pkg.Name or ?ptr=0x...");
    void* target = nullptr;
    if (!ptr.empty()) {
        char* end = nullptr;
        unsigned long long v = strtoull(ptr.c_str(), &end, 0);
        if (!v || (end && *end)) return Err(400, "ptr must be a hex or decimal address");
        target = (void*)(uintptr_t)v;
        if (!MemReadable(target, 0x40)) return Err(400, "ptr is not inside a readable mapping");
    }
    auto snapshot = std::make_shared<Reflect::DebugSnapshot>();
    bool ok = GameThread::Run(
        [target, path, snapshot] {
            void* obj = target;
            if (!obj) {
                size_t dot = path.find('.');
                if (path.size() < 2 || path[0] != '/' || dot == std::string::npos) {
                    *snapshot = [] { return "{\"error\":\"path must look like /Script/Vein.PlayerChatComponent\"}"; };
                    return;
                }
                obj = Reflect::FindObjectByPath(path.substr(0, dot), path.substr(dot + 1));
                if (!obj) { *snapshot = [] { return "{\"error\":\"object not found\"}"; }; return; }
            }
            *snapshot = Reflect::DumpObject(obj);
        },
        5000);
    if (!ok) return Err(503, "game thread unavailable");
    return {200, (*snapshot)()};
}

Response DebugStructs(const Request& r) {
    std::string name = QueryParam(r.query, "name");
    if (name.empty()) return Err(400, "pass ?name=ChatMessageData or ?name=/Script/Vein.ChatMessageData");
    auto snapshot = std::make_shared<Reflect::DebugSnapshot>();
    if (!GameThread::Run([name, snapshot] { *snapshot = Reflect::DumpStruct(name); }, 5000))
        return Err(503, "game thread unavailable");
    return {200, (*snapshot)()};
}

Response Events(const Request& r) {
    uint64_t since = (uint64_t)strtoull(QueryParam(r.query, "since").c_str(), nullptr, 10);
    std::string lim = QueryParam(r.query, "limit");
    size_t limit = lim.empty() ? PluginState::kMaxEvents : (size_t)strtoull(lim.c_str(), nullptr, 10);
    if (limit == 0 || limit > PluginState::kMaxEvents) limit = PluginState::kMaxEvents;
    return {200, PluginState::Get().EventsJson(since, limit)};
}

Response FromAction(const Actions::Result& r) { return {r.status, r.body}; }

Response Route(const Request& r) {
    auto p = Split(r.path);
    bool GET = r.method == "GET", POST = r.method == "POST";
    if (p.empty()) return Err(404, "not found");
    const std::string& p0 = p[0];

    if (p0 == "health" && p.size() == 1) {
        if (!GET) return Err(405, "method not allowed");
        return Health();
    }
    if (p0 == "events" && p.size() == 1) {
        if (!GET) return Err(405, "method not allowed");
        return Events(r);
    }
    if (p0 == "debug") {
        if (!DebugEnabled()) return Err(404, "debug endpoints need TAKARO_PLUGIN_DEBUG=1");
        if (!GET && p.size() == 2 && p[1] == "kill-nearest" && r.method == "POST") {
            JsonValue body;
            if (!r.body.empty() && !JsonParse(r.body, body)) return Err(400, "body must be JSON");
            return FromAction(Actions::KillNearest(body));
        }
        // lane L3b: grant/revoke admin on the live game session (the proof lanes' handle).
        if (!GET && p.size() == 2 && p[1] == "set-admin" && r.method == "POST") {
            JsonValue body;
            if (!r.body.empty() && !JsonParse(r.body, body)) return Err(400, "body must be JSON");
            return FromAction(Admin::SetAdminEndpoint(body));
        }
        if (!GET) return Err(405, "method not allowed");
        if (p.size() == 2 && p[1] == "gamethread") return DebugGameThread();
        if (p.size() == 2 && p[1] == "perf") {
            // ?reset=1 zeroes the counters *after* answering, so a caller gets the window it asked
            // for and the next window starts clean.
            std::string body = Perf::Json();
            if (QueryParam(r.query, "reset") == "1") Perf::Reset();
            return {200, body};
        }
        if (p.size() == 2 && p[1] == "symbols") return DebugSymbols();
        if (p.size() == 2 && p[1] == "nearby") {
            std::string rad = QueryParam(r.query, "radius");
            double radius = rad.empty() ? 5000.0 : atof(rad.c_str());
            return FromAction(Events::Nearby(QueryParam(r.query, "gameId"), radius));
        }
        // lane L3f: which inventory container the plugin answers from, and which ones exist.
        if (p.size() == 2 && p[1] == "inventories")
            return FromAction(Actions::DebugInventories(QueryParam(r.query, "gameId")));
        if (p.size() == 2 && p[1] == "object") return DebugObject(r);
        if (p.size() == 2 && p[1] == "structs") return DebugStructs(r);
        return Err(404, "unknown debug endpoint");
    }
    // ---- lane L3 surface: routed, validated, currently 501 --------------------------------------
    if (p0 == "players") {
        if (!GET) return Err(405, "method not allowed");
        if (p.size() == 1) return FromAction(Actions::Players());
        if (p.size() == 2) return FromAction(Actions::Player(p[1]));
        if (p.size() == 3 && p[2] == "location") return FromAction(Actions::PlayerLocation(p[1]));
        if (p.size() == 3 && p[2] == "inventory") return FromAction(Actions::PlayerInventory(p[1]));
        return Err(404, "not found");
    }
    if (GET && p.size() == 1) {
        if (p0 == "items") return FromAction(Actions::Items(QueryParam(r.query, "search")));
        if (p0 == "entities") return FromAction(Actions::Entities());
        if (p0 == "locations") return FromAction(Actions::Locations());
        if (p0 == "bans") return FromAction(Actions::Bans());
    }
    if (POST && p.size() == 1) {
        static const char* kPostPaths[] = {"message", "teleport", "give", "kick", "ban", "unban", "command", "shutdown"};
        bool known = false;
        for (auto* k : kPostPaths) known = known || p0 == k;
        if (known) {
            JsonValue v;
            if (p0 != "shutdown") {
                // Bodies are validated before the 501 check, so a malformed request is still a 400.
                if (!r.body.empty() && (!JsonParse(r.body, v) || v.type != JsonValue::Object))
                    return Err(400, "body must be a JSON object");
            }
            if (p0 == "message") return FromAction(Actions::Message(v));
            if (p0 == "teleport") return FromAction(Actions::Teleport(v));
            if (p0 == "give") return FromAction(Actions::Give(v));
            if (p0 == "kick") return FromAction(Actions::Kick(v));
            if (p0 == "ban") return FromAction(Actions::Ban(v));
            if (p0 == "unban") return FromAction(Actions::Unban(v));
            if (p0 == "command") return FromAction(Actions::Command(v));
            if (p0 == "shutdown") return FromAction(Actions::Shutdown());
        }
    }
    static const char* kKnown[] = {"health", "events", "debug", "players", "items", "entities", "locations", "bans",
                                   "message", "teleport", "give", "kick", "ban", "unban", "command", "shutdown"};
    for (auto* k : kKnown)
        if (p0 == k) return Err(405, "method not allowed");
    return Err(404, "not found");
}

void* ConnThread(void* arg) {
    int c = (int)(intptr_t)arg;
    struct timeval tv{10, 0};
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    Request r;
    int errStatus = 0;
    if (ReadRequest(c, r, errStatus)) {
        g_requests++;
        Response resp;
        if (!TokenOk(r)) {
            g_unauthorized++;
            resp = Err(401, g_token.empty() ? "plugin token not configured" : "unauthorized");
        } else {
            try {
                resp = Route(r);
            } catch (const std::exception& e) {
                g_errors++;
                PluginLog("http: handler for %s %s threw: %s", r.method.c_str(), r.path.c_str(), e.what());
                resp = Err(500, "internal plugin error");
            } catch (...) {
                g_errors++;
                PluginLog("http: handler for %s %s threw a non-standard exception", r.method.c_str(), r.path.c_str());
                resp = Err(500, "internal plugin error");
            }
        }
        if (DebugEnabled()) PluginLog("http: %s %s -> %d", r.method.c_str(), r.path.c_str(), resp.status);
        Send(c, resp);
    } else if (errStatus) {
        Send(c, Err(errStatus, "bad request"));
    }
    shutdown(c, SHUT_RDWR);
    close(c);
    return nullptr;
}

void LoadToken() {
    g_token = ConfigValue("TAKARO_PLUGIN_TOKEN", "token", "");
    if (g_token.empty())
        PluginLog("http: diagnostics disabled (TAKARO_PLUGIN_TOKEN is not configured)");
    else
        PluginLog("http: token configured (%zu chars)", g_token.size());
    std::string port = ConfigValue("TAKARO_PLUGIN_PORT", "port", "");
    if (!port.empty()) {
        int v = atoi(port.c_str());
        if (v > 0 && v < 65536) g_port = v;
    }
}

void* ListenThread(void*) {
    int s = -1;
    for (int attempt = 0;; attempt++) {
        s = socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        if (s >= 0) setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons((uint16_t)g_port);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (s >= 0 && bind(s, (sockaddr*)&a, sizeof a) == 0 && listen(s, 32) == 0) break;
        PluginLog("http: bind/listen on 127.0.0.1:%d failed (%s), attempt %d", g_port, strerror(errno), attempt);
        if (s >= 0) close(s);
        struct timespec ts{5, 0};
        nanosleep(&ts, nullptr);
    }
    PluginLog("http: listening on 127.0.0.1:%d", g_port);
    for (;;) {
        int c = accept(s, nullptr, nullptr);
        if (c < 0) {
            struct timespec ts{0, 10 * 1000 * 1000};
            nanosleep(&ts, nullptr);
            continue;
        }
        pthread_t t;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&t, &attr, ConnThread, (void*)(intptr_t)c) != 0) close(c);
        pthread_attr_destroy(&attr);
    }
    return nullptr;
}

}  // namespace

int Http::Port() { return g_port; }
bool Http::TokenConfigured() { return !g_token.empty(); }

std::string Http::StatsJson() {
    return "{\"port\":" + std::to_string(g_port) + ",\"tokenConfigured\":" + (g_token.empty() ? "false" : "true") +
           ",\"requests\":" + std::to_string(g_requests.load()) +
           ",\"unauthorized\":" + std::to_string(g_unauthorized.load()) +
           ",\"handlerErrors\":" + std::to_string(g_errors.load()) + "}";
}

void Http::Start() {
    StartMs();
    LoadToken();
    if (g_token.empty()) return;
    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&t, &attr, ListenThread, nullptr);
    pthread_attr_destroy(&attr);
}
