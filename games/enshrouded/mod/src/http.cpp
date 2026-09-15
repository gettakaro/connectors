// Minimal HTTP/1.1 server on 127.0.0.1:18890 (one short-lived thread per connection, Connection: close).
#include <winsock2.h>
#include <ws2tcpip.h>

#include "common.h"
#include "hooks.h"
#include "state.h"
#include "world.h"

#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cstring>

const std::string& BootId() {
    static const std::string id = [] {
        LARGE_INTEGER qpc;
        QueryPerformanceCounter(&qpc);
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        uint64_t v = (uint64_t)qpc.QuadPart ^ ((uint64_t)ft.dwHighDateTime << 32 | ft.dwLowDateTime) ^
                     ((uint64_t)GetCurrentProcessId() << 17);
        v ^= v >> 33; v *= 0xff51afd7ed558ccdULL; v ^= v >> 33; v *= 0xc4ceb9fe1a85ec53ULL; v ^= v >> 33;
        char b[17];
        snprintf(b, sizeof b, "%016llx", (unsigned long long)v);
        return std::string(b);
    }();
    return id;
}


void HttpStart();

namespace {

const int kPort = 18890;
std::string g_token;

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
        case 413: return "Payload Too Large";
        case 501: return "Not Implemented";
        case 503: return "Service Unavailable";
        default: return "OK";
    }
}

bool ReadRequest(SOCKET c, Request& r, int& errStatus) {
    std::string buf;
    char tmp[4096];
    size_t headerEnd = std::string::npos;
    while (headerEnd == std::string::npos) {
        int n = recv(c, tmp, sizeof tmp, 0);
        if (n <= 0) return false;
        buf.append(tmp, n);
        headerEnd = buf.find("\r\n\r\n");
        if (buf.size() > 64 * 1024 && headerEnd == std::string::npos) {
            errStatus = 413;
            return false;
        }
    }
    std::string head = buf.substr(0, headerEnd);
    size_t lineEnd = head.find("\r\n");
    std::string reqLine = head.substr(0, lineEnd);
    size_t s1 = reqLine.find(' '), s2 = reqLine.rfind(' ');
    if (s1 == std::string::npos || s2 == s1) {
        errStatus = 400;
        return false;
    }
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
    if (contentLength > 1024 * 1024) {
        errStatus = 413;
        return false;
    }
    r.body = buf.substr(headerEnd + 4);
    while (r.body.size() < contentLength) {
        int n = recv(c, tmp, sizeof tmp, 0);
        if (n <= 0) return false;
        r.body.append(tmp, n);
    }
    if (r.body.size() > contentLength) r.body.resize(contentLength);
    return true;
}

void Send(SOCKET c, const Response& resp) {
    std::string out = "HTTP/1.1 " + std::to_string(resp.status) + " " + Reason(resp.status) +
                      "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(resp.body.size()) +
                      "\r\nConnection: close\r\n\r\n" + resp.body;
    size_t sent = 0;
    while (sent < out.size()) {
        int n = send(c, out.data() + sent, (int)(out.size() - sent), 0);
        if (n <= 0) break;
        sent += n;
    }
}

bool TokenOk(const Request& r) {
    if (g_token.empty()) return false;
    auto it = r.headers.find("authorization");
    if (it == r.headers.end()) return false;
    const std::string& v = it->second;
    if (v.size() < 7 || Lower(v.substr(0, 7)) != "bearer ") return false;
    std::string t = v.substr(7);
    if (t.size() != g_token.size()) return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < t.size(); i++) diff |= (unsigned char)(t[i] ^ g_token[i]);
    return diff == 0;
}

Response Err(int status, const std::string& msg) { return {status, "{\"error\":" + JsonStr(msg) + "}"}; }
Response Unimplemented() { return Err(501, "unimplemented"); }

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

// Validates a JSON object body and required string/number fields; fills err on failure.
bool Body(const Request& r, JsonValue& v, std::initializer_list<const char*> strFields,
          std::initializer_list<const char*> numFields, Response& err) {
    if (!JsonParse(r.body, v) || v.type != JsonValue::Object) {
        err = Err(400, "body must be a JSON object");
        return false;
    }
    for (auto* f : strFields) {
        auto* x = v.get(f);
        if (!x || !x->isStr() || x->str.empty()) {
            err = Err(400, std::string("missing string field '") + f + "'");
            return false;
        }
    }
    for (auto* f : numFields) {
        auto* x = v.get(f);
        if (!x || !x->isNum()) {
            err = Err(400, std::string("missing numeric field '") + f + "'");
            return false;
        }
    }
    return true;
}

// ---- account hash resolution ----
// handleAccountAction takes the game's accountIdHash (HashKey64), not the SteamID64. Online players are resolved
// from the session machine table: mgr=[server+0x10]; active copy mgr+0x48+[mgr+0x3c]*0x2578; 64 records of 0x140:
// handle@+0x188, machine handle@+0x190 (low 7 bits = machine index "M" of "[server] Machine 'M'"), hash@+0x198.
// Resolved hashes are cached in <server>\takaro\accounts.json so offline unban keeps working across restarts.
SrwLock g_accLock;
std::map<std::string, uint64_t> g_accounts;  // steamId -> accountIdHash
bool g_accLoaded = false;

std::string AccountsPath() { return PluginBaseDir() + "\\takaro\\accounts.json"; }

void LoadAccountsLocked() {
    if (g_accLoaded) return;
    g_accLoaded = true;
    FILE* f = fopen(AccountsPath().c_str(), "rb");
    if (!f) return;
    std::string text;
    char buf[4096];
    size_t k;
    while ((k = fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, k);
    fclose(f);
    JsonValue v;
    if (!JsonParse(text, v) || v.type != JsonValue::Object) return;
    for (auto& kv : v.obj)
        if (kv.second.isStr()) g_accounts[kv.first] = strtoull(kv.second.str.c_str(), nullptr, 10);
}

void SaveAccountsLocked() {
    std::string o = "{";
    bool first = true;
    for (auto& kv : g_accounts) {
        o += std::string(first ? "" : ",") + JsonStr(kv.first) + ":" + JsonStr(std::to_string(kv.second));
        first = false;
    }
    o += "}";
    std::string tmp = AccountsPath() + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return;
    fwrite(o.data(), 1, o.size(), f);
    fclose(f);
    MoveFileExA(tmp.c_str(), AccountsPath().c_str(), MOVEFILE_REPLACE_EXISTING);
}

bool CachedAccount(const std::string& steamId, uint64_t& hash) {
    Guard g(g_accLock);
    LoadAccountsLocked();
    auto it = g_accounts.find(steamId);
    if (it == g_accounts.end()) return false;
    hash = it->second;
    return true;
}

// Looks up the account hash of an online player's machine on the moderation thread. 0 when not found/ambiguous.
uint64_t LookupAccountHash(const std::string& machine) {
    if (machine.empty()) return 0;
    uint32_t idx = (uint32_t)strtoul(machine.c_str(), nullptr, 10);
    std::string out;
    if (!RunOnModerationThread(
            [idx](uint64_t server) {
                uint64_t mgr = *(uint64_t*)(server + 0x10);
                if (!mgr) return std::string("0");
                uint32_t ver = *(uint32_t*)(mgr + 0x3c);
                if (ver > 1) return std::string("0");
                uint64_t base = mgr + 0x48 + (uint64_t)ver * 0x2578;
                uint64_t found = 0;
                int matches = 0;
                for (int i = 0; i < 64; i++) {
                    uint64_t rec = base + (uint64_t)i * 0x140;
                    uint32_t h = *(uint32_t*)(rec + 0x188);
                    if (!h || (h & 0x3f) != (uint32_t)i) continue;
                    uint32_t mh = *(uint32_t*)(rec + 0x190);
                    uint64_t acc = *(uint64_t*)(rec + 0x198);
                    if (acc && (mh & 0x7f) == idx) {
                        found = acc;
                        matches++;
                    }
                }
                return matches == 1 ? std::to_string(found) : std::string("0");
            },
            out))
        return 0;
    return strtoull(out.c_str(), nullptr, 10);
}

// Resolves (and caches) the account hash for a SteamID64: live table for online players, cache otherwise.
bool ResolveAccount(const std::string& steamId, uint64_t& hash, std::string& how) {
    auto& st = PluginState::Get();
    std::string machine;
    if (st.OnlineMachine(steamId, machine)) {
        uint64_t h = LookupAccountHash(machine);
        if (h) {
            Guard g(g_accLock);
            LoadAccountsLocked();
            if (g_accounts[steamId] != h) {
                g_accounts[steamId] = h;
                SaveAccountsLocked();
            }
            hash = h;
            how = "machine " + machine;
            return true;
        }
    }
    if (CachedAccount(steamId, hash)) {
        how = "cache";
        return true;
    }
    return false;
}

bool ParseAccountId(const std::string& id, uint64_t& out) {
    if (id.empty() || id.size() > 20) return false;
    out = 0;
    for (char ch : id) {
        if (ch < '0' || ch > '9') return false;
        out = out * 10 + (uint64_t)(ch - '0');
    }
    return out != 0;
}

// listBans: authoritative persisted ban list (bannedAccounts[] in enshrouded_server.json).
Response BansJson() {
    std::string path = PluginBaseDir() + "\\enshrouded_server.json";
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return Err(503, "enshrouded_server.json not readable");
    std::string text;
    char buf[8192];
    size_t k;
    while ((k = fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, k);
    fclose(f);
    JsonValue root;
    if (!JsonParse(text, root)) return Err(503, "enshrouded_server.json parse failed");
    std::string out = "[";
    auto* arr = root.get("bannedAccounts");
    if (arr && arr->type == JsonValue::Array) {
        bool first = true;
        for (auto& b : arr->arr) {
            auto* a = b.get("accountId");
            if (!a || (a->type != JsonValue::String && a->type != JsonValue::Number) || a->str.empty()) continue;
            auto field = [&](const char* key) {
                auto* x = b.get(key);
                return x && (x->isStr() || x->isNum()) ? x->str : std::string();
            };
            std::string name = field("displayName");
            std::string steam = a->str;  // the JSON may hold the account hash; map back to the SteamID when cached
            {
                Guard g(g_accLock);
                LoadAccountsLocked();
                for (auto& kv : g_accounts)
                    if (std::to_string(kv.second) == a->str) steam = kv.first;
            }
            out += std::string(first ? "" : ",") + "{\"gameId\":" + JsonStr(steam) + ",\"steamId\":" + JsonStr(steam) +
                   ",\"accountId\":" + JsonStr(a->str) +
                   ",\"name\":" + JsonStr(name.empty() ? a->str : name) + ",\"characterName\":" + JsonStr(field("characterName")) +
                   ",\"bannedAt\":" + JsonStr(field("banDate")) + ",\"reason\":\"\",\"expiresAt\":null}";
            first = false;
        }
    }
    return {200, out + "]"};
}

// ---- world actions (shared by the REST endpoints and the command set) ----
Response DoTeleport(const std::string& id, double x, double y, double z) {
    std::string steam, name, err;
    if (!PluginState::Get().FindOnline(id, steam, name)) return Err(404, "player not online");
    if (!WorldTeleport(name, Vec3{x, y, z}, err)) return Err(503, err);
    return {200, "{\"success\":true}"};
}

Response DoGive(const std::string& id, const std::string& code, double amount) {
    std::string steam, name, err, detail;
    if (!PluginState::Get().FindOnline(id, steam, name)) return Err(404, "player not online");
    const ItemDef* item = FindItemByCode(code);
    if (!item) return Err(400, "unknown item code '" + code + "' (see GET /items)");
    if (!(amount >= 1) || amount > 100000) return Err(400, "amount must be >= 1");
    if (!WorldGiveItem(name, *item, (uint32_t)amount, err, detail)) return Err(503, err + (detail.empty() ? "" : " (" + detail + ")"));
    return {200, "{\"success\":true,\"code\":" + JsonStr(item->code) + ",\"detail\":" + JsonStr(detail) + "}"};
}

Response DoMessage(const JsonValue& v) {
    std::string machine, err;
    auto* rec = v.get("recipientGameId");
    if (rec && rec->isStr() && !rec->str.empty()) {
        std::string steam, name;
        if (!PluginState::Get().FindOnline(rec->str, steam, name)) return Err(404, "recipient not online");
        if (!PluginState::Get().OnlineMachine(steam, machine)) return Err(503, "recipient machine index unknown");
    }
    auto* t = v.get("type");
    auto* h = v.get("senderHandle");
    uint8_t type = t && t->isNum() ? (uint8_t)t->num : 0;
    uint32_t handle = h && h->isNum() ? (uint32_t)h->num : 0;
    if (!WorldSendMessage(v.get("text")->str, machine, type, handle, err)) return Err(503, err);
    return {200, "{\"success\":true}"};
}

std::vector<std::string> Tokens(const std::string& s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && isspace((unsigned char)s[i])) i++;
        if (i >= s.size()) break;
        std::string tok;
        if (s[i] == '"') {
            size_t e = s.find('"', i + 1);
            tok = s.substr(i + 1, e == std::string::npos ? std::string::npos : e - i - 1);
            i = e == std::string::npos ? s.size() : e + 1;
        } else {
            size_t e = i;
            while (e < s.size() && !isspace((unsigned char)s[e])) e++;
            tok = s.substr(i, e - i);
            i = e;
        }
        out.push_back(tok);
    }
    return out;
}

std::string ErrText(const Response& r) {
    JsonValue v;
    if (JsonParse(r.body, v) && v.get("error") && v.get("error")->isStr()) return v.get("error")->str;
    return r.body;
}

bool Num(const std::string& s, double& out) {
    char* end = nullptr;
    out = strtod(s.c_str(), &end);
    return !s.empty() && end && *end == 0;
}

// Plugin-defined console: Enshrouded's dedicated server has no command console of its own.
bool RunCommand(const std::string& line, std::string& out) {
    auto a = Tokens(line);
    if (a.empty()) {
        out = "empty command; try 'help'";
        return false;
    }
    std::string cmd = a[0];
    for (auto& c : cmd) c = (char)tolower((unsigned char)c);
    auto& st = PluginState::Get();
    if (cmd == "help") {
        out = "commands: help | version | players | say <text> | whisper <player> <text> | location <player> | "
              "teleport <player> <x> <y> <z> | tp <player> <toPlayer> | inventory <player> | give <player> <itemCode> [amount] | "
              "item <search> | kick <player> | save-and-shutdown. <player> = SteamID64 or exact name.";
        return true;
    }
    if (cmd == "version") {
        out = "takaro enshrouded plugin " TAKARO_PLUGIN_VERSION ", game build " + GameBuild();
        return true;
    }
    if (cmd == "players") {
        JsonValue v;
        JsonParse(st.PlayersJson(), v);
        out = std::to_string(v.arr.size()) + " online";
        for (auto& p : v.arr)
            out += "\n" + (p.get("name") ? p.get("name")->str : "?") + " (" + (p.get("steamId") ? p.get("steamId")->str : "?") + ")";
        return true;
    }
    if ((cmd == "say" || cmd == "broadcast") && a.size() >= 2) {
        std::string text = line.substr(line.find(a[0]) + a[0].size());
        text.erase(0, text.find_first_not_of(" \t"));
        std::string err;
        if (!WorldSendMessage(text, "", 0, 0, err)) return out = err, false;
        out = "sent: " + text;
        return true;
    }
    if (cmd == "whisper" && a.size() >= 3) {
        std::string steam, name, machine, err;
        if (!st.FindOnline(a[1], steam, name) || !st.OnlineMachine(steam, machine)) return out = "player not online: " + a[1], false;
        std::string text;
        for (size_t i = 2; i < a.size(); i++) text += (i > 2 ? " " : "") + a[i];
        if (!WorldSendMessage(text, machine, 0, 0, err)) return out = err, false;
        out = "whispered to " + name + ": " + text;
        return true;
    }
    if (cmd == "location" && a.size() == 2) {
        std::string steam, name, err;
        if (!st.FindOnline(a[1], steam, name)) return out = "player not online: " + a[1], false;
        Vec3 v;
        if (!WorldGetLocation(name, v, err)) return out = err, false;
        char b[160];
        snprintf(b, sizeof b, "%s is at x=%.2f y=%.2f z=%.2f", name.c_str(), v.x, v.y, v.z);
        out = b;
        return true;
    }
    if ((cmd == "teleport" || cmd == "tp") && (a.size() == 5 || a.size() == 3)) {
        double x, y, z;
        if (a.size() == 3) {
            std::string steam, name, err;
            if (!st.FindOnline(a[2], steam, name)) return out = "target player not online: " + a[2], false;
            Vec3 v;
            if (!WorldGetLocation(name, v, err)) return out = err, false;
            x = v.x, y = v.y, z = v.z;
        } else if (!Num(a[2], x) || !Num(a[3], y) || !Num(a[4], z)) {
            return out = "usage: teleport <player> <x> <y> <z>", false;
        }
        Response r = DoTeleport(a[1], x, y, z);
        if (r.status != 200) return out = ErrText(r), false;
        char b[160];
        snprintf(b, sizeof b, "teleport queued to x=%.2f y=%.2f z=%.2f", x, y, z);
        out = b;
        return true;
    }
    if (cmd == "inventory" && a.size() == 2) {
        std::string steam, name, err, json;
        if (!st.FindOnline(a[1], steam, name)) return out = "player not online: " + a[1], false;
        if (!WorldInventoryJson(name, json, err)) return out = err, false;
        JsonValue v;
        JsonParse(json, v);
        out = name + ": " + std::to_string(v.arr.size()) + " stacks";
        for (auto& i : v.arr)
            out += "\n" + i.get("amount")->str + " x " + i.get("code")->str + " [" + i.get("inventory")->str + "]";
        return true;
    }
    if (cmd == "give" && (a.size() == 3 || a.size() == 4)) {
        double n = 1;
        if (a.size() == 4 && !Num(a[3], n)) return out = "usage: give <player> <itemCode> [amount]", false;
        Response r = DoGive(a[1], a[2], n);
        if (r.status != 200) return out = ErrText(r), false;
        out = "gave " + std::to_string((long long)n) + " x " + a[2];
        return true;
    }
    if (cmd == "item" && a.size() == 2) {
        std::string q = a[1];
        for (auto& c : q) c = (char)tolower((unsigned char)c);
        int n = 0;
        for (size_t i = 0; i < kItemCount && n < 25; i++) {
            std::string c = kItems[i].code;
            for (auto& ch : c) ch = (char)tolower((unsigned char)ch);
            if (c.find(q) == std::string::npos) continue;
            out += std::string(n ? "\n" : "") + kItems[i].code + " (" + kItems[i].category + ", max " + std::to_string(kItems[i].maxStack) + ")";
            n++;
        }
        if (!n) out = "no item matches '" + a[1] + "'";
        return n > 0;
    }
    if (cmd == "kick" && a.size() == 2) {
        std::string steam, name, err, how;
        if (!st.FindOnline(a[1], steam, name)) return out = "player not online: " + a[1], false;
        uint64_t hash = 0;
        if (!ResolveAccount(steam, hash, how)) return out = "account hash unknown", false;
        if (!AccountAction(hash, 0, err)) return out = err, false;
        out = "kicked " + name;
        return true;
    }
    if (cmd == "save-and-shutdown") {
        std::string err;
        if (!RequestShutdown(err)) return out = err, false;
        out = "graceful shutdown started (save + quit)";
        return true;
    }
    out = "unknown command or wrong arguments: '" + a[0] + "'; try 'help'";
    return false;
}

Response Route(const Request& r) {
    auto parts = Split(r.path);
    bool get = r.method == "GET", post = r.method == "POST";
    auto& st = PluginState::Get();
    if (parts.empty()) return Err(404, "not found");
    const std::string& p0 = parts[0];

    if (p0 == "health" && parts.size() == 1 && get) {
        return {200, "{\"status\":\"ok\",\"version\":\"" TAKARO_PLUGIN_VERSION "\",\"bootId\":" + JsonStr(BootId()) + ",\"gameBuild\":" +
                         JsonStr(GameBuild()) + ",\"capabilities\":" + st.CapabilitiesJson() +
                         ",\"capabilityDetails\":" + st.CapabilityDetailsJson() +
                         ",\"diagnostics\":" + HookDiagnosticsJson() + "}"};
    }
    if (p0 == "players" && get) {
        if (parts.size() == 1) return {200, st.PlayersJson()};
        std::string pj;
        if (!st.PlayerJson(parts[1], pj)) return Err(404, "player not online");
        if (parts.size() == 2) return {200, pj};
        if (parts.size() == 3 && (parts[2] == "location" || parts[2] == "inventory")) {
            std::string steam, name, err;
            if (!st.FindOnline(parts[1], steam, name)) return Err(404, "player not online");
            if (parts[2] == "location") {
                Vec3 v;
                if (!WorldGetLocation(name, v, err)) return Err(503, err);
                char b[160];
                snprintf(b, sizeof b, "{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f}", v.x, v.y, v.z);
                return {200, b};
            }
            std::string json;
            if (!WorldInventoryJson(name, json, err)) return Err(503, err);
            return {200, json};
        }
        return Err(404, "not found");
    }
    if (p0 == "events" && parts.size() == 1 && get) {
        std::string since = QueryParam(r.query, "since"), limit = QueryParam(r.query, "limit");
        uint64_t s = since.empty() ? 0 : strtoull(since.c_str(), nullptr, 10);
        size_t l = limit.empty() ? 5000 : (size_t)strtoull(limit.c_str(), nullptr, 10);
        if (l == 0 || l > 5000) l = 5000;
        return {200, "{\"bootId\":" + JsonStr(BootId()) + "," + st.EventsJson(s, l).substr(1)};
    }
    if (p0 == "debug" && parts.size() == 2 && parts[1] == "findu64" && get) {
        // Diagnostic: find a u64 (query v) inside the machine manager and server objects; reports offsets.
        uint64_t needle = strtoull(QueryParam(r.query, "v").c_str(), nullptr, 10);
        std::string out;
        if (!RunOnModerationThread(
                [needle](uint64_t server) {
                    std::string o = "{";
                    auto scanRange = [&](const char* name, uint64_t base, size_t len) {
                        o += std::string("\"") + name + "\":[";
                        MEMORY_BASIC_INFORMATION mbi;
                        bool first = true;
                        for (size_t off = 0; off + 8 <= len; off += 4) {
                            uint64_t a = base + off;
                            if ((off & 0xfff) == 0 || off == 0) {
                                if (!VirtualQuery((LPCVOID)a, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT ||
                                    (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
                                    off = ((off + 0x1000) & ~(size_t)0xfff) - 4;
                                    continue;
                                }
                            }
                            if (*(uint64_t*)a == needle) {
                                char b[32];
                                snprintf(b, sizeof b, "%s\"0x%zx\"", first ? "" : ",", off);
                                o += b;
                                first = false;
                            }
                        }
                        o += "]";
                    };
                    scanRange("server", server, 0x2bc000);
                    o += ",";
                    uint64_t mgr = *(uint64_t*)(server + 0x10);
                    scanRange("mgr", mgr, 0x10000);
                    char b[80];
                    snprintf(b, sizeof b, ",\"mgrMinusServer\":\"%lld\"}", (long long)(mgr - server));
                    return o + b;
                },
                out, 10000))
            return Err(503, "moderation system unavailable");
        return {200, out};
    }
    if (p0 == "debug" && parts.size() == 2 && parts[1] == "machines" && get) {
        // Diagnostic: the session machine table the account-action handler matches against.
        // mgr=[server+0x10]; records at mgr+0x48+[mgr+0x3c]*0x2578, stride 0x140: handle@+0x188, account@+0x198.
        std::string out;
        if (!RunOnModerationThread(
                [](uint64_t server) {
                    std::string o = "[";
                    uint64_t mgr = *(uint64_t*)(server + 0x10);
                    if (!mgr) return std::string("[]");
                    uint32_t ver = *(uint32_t*)(mgr + 0x3c);
                    bool first = true;
                    char b[200];
                    snprintf(b, sizeof b, "{\"ver\":%u,\"records\":[", ver);
                    o = b;
                    for (uint32_t v = 0; v < 2; v++) {
                        uint64_t base = mgr + 0x48 + (uint64_t)v * 0x2578;
                        for (int i = 0; i < 64; i++) {
                            uint64_t rec = base + (uint64_t)i * 0x140;
                            uint32_t h = *(uint32_t*)(rec + 0x188);
                            if (!h || (h & 0x3f) != (uint32_t)i) continue;
                            uint64_t acc = *(uint64_t*)(rec + 0x198);
                            snprintf(b, sizeof b,
                                     "%s{\"v\":%u,\"slot\":%d,\"handle\":%u,\"account\":\"%llu\",\"a18c\":%u,\"a190\":%u,\"a194\":%u}",
                                     first ? "" : ",", v, i, h, (unsigned long long)acc, *(uint32_t*)(rec + 0x18c),
                                     *(uint32_t*)(rec + 0x190), *(uint32_t*)(rec + 0x194));
                            o += b;
                            first = false;
                        }
                    }
                    return o + "]}";
                },
                out))
            return Err(503, "moderation system unavailable");
        return {200, out};
    }
    if (p0 == "debug" && parts.size() == 2 && parts[1] == "nearby" && get) {
        std::string steam, name, json, err;
        if (!st.FindOnline(QueryParam(r.query, "player"), steam, name)) return Err(404, "player not online");
        std::string rad = QueryParam(r.query, "radius");
        if (!WorldNearbyJson(name, rad.empty() ? 80 : atof(rad.c_str()), json, err)) return Err(503, err);
        return {200, json};
    }
    if (p0 == "debug" && parts.size() == 2 && parts[1] == "slots" && get) {
        std::string json, err;
        if (!WorldSlotsJson(json, err)) return Err(503, err);
        return {200, json};
    }
    if (p0 == "debug" && parts.size() == 2 && parts[1] == "gamethread" && get) {
        uint64_t t0 = NowMs();
        DWORD httpTid = GetCurrentThreadId();
        std::string out;
        if (!RunOnGameThread([] { return std::to_string(GetCurrentThreadId()); }, out, 3000))
            return Err(503, "game thread did not run the task");
        return {200, "{\"ranOnThreadId\":" + out + ",\"httpThreadId\":" + std::to_string(httpTid) +
                         ",\"latencyMs\":" + std::to_string(NowMs() - t0) + "}"};
    }

    // Action endpoints: validate the body shape, then 501 until the capability is wired.
    if (post) {
        JsonValue v;
        Response e;
        if (p0 == "message" && parts.size() == 1) {
            if (!Body(r, v, {"text"}, {}, e)) return e;
            return DoMessage(v);
        }
        if (p0 == "teleport" && parts.size() == 1) {
            if (!Body(r, v, {"gameId"}, {"x", "y", "z"}, e)) return e;
            return DoTeleport(v.get("gameId")->str, v.get("x")->num, v.get("y")->num, v.get("z")->num);
        }
        if (p0 == "give" && parts.size() == 1) {
            if (!Body(r, v, {"gameId", "code"}, {"amount"}, e)) return e;
            return DoGive(v.get("gameId")->str, v.get("code")->str, v.get("amount")->num);
        }
        if ((p0 == "kick" || p0 == "ban" || p0 == "unban") && parts.size() == 1) {
            if (!Body(r, v, {"gameId"}, {}, e)) return e;
            const std::string& id = v.get("gameId")->str;
            uint64_t account = 0;
            if (!ParseAccountId(id, account)) return Err(400, "gameId must be a SteamID64 (decimal)");
            // the game's handler only acts on accounts connected to the server for kick and ban
            if (p0 != "unban" && !st.IsOnline(id)) return Err(404, "player not online");
            std::string err, how;
            uint8_t type = p0 == "kick" ? 0 : p0 == "ban" ? 1 : 2;
            uint64_t hash = 0;
            if (!ResolveAccount(id, hash, how))
                return Err(p0 == "unban" ? 404 : 503, "account hash for this SteamID is unknown (player never seen online by the plugin)");
            PluginLog("moderation: %s steam=%s accountHash=%llu (%s)", p0.c_str(), id.c_str(), (unsigned long long)hash, how.c_str());
            int status = 503;
            bool bypass = false;
            if (!AccountAction(hash, type, err, 5000, &status, &bypass)) return Err(status, err);
            return {200, std::string("{\"success\":true") + (bypass ? ",\"adminProtectionBypassed\":true" : "") + "}"};
        }
        if (p0 == "command" && parts.size() == 1) {
            if (!Body(r, v, {"command"}, {}, e)) return e;
            std::string out;
            bool ok = RunCommand(v.get("command")->str, out);
            return {200, "{\"success\":" + std::string(ok ? "true" : "false") + ",\"output\":" + JsonStr(out) + "}"};
        }
        if (p0 == "shutdown" && parts.size() == 1) {
            std::string err;
            if (!RequestShutdown(err)) return Err(503, err);
            return {200, "{\"success\":true}"};
        }
    }
    if (get && parts.size() == 1 && p0 == "bans") return BansJson();
    if (get && parts.size() == 1 && p0 == "items") return {200, ItemsJson()};
    if (get && parts.size() == 1 && p0 == "entities") return {200, EntitiesJson()};
    if (get && parts.size() == 1 && p0 == "locations") return {200, LocationsJson()};

    static const char* known[] = {"health", "players", "events", "message", "teleport", "give", "kick", "ban",
                                  "unban", "bans", "items", "entities", "locations", "command", "shutdown", "debug"};
    for (auto* k : known)
        if (p0 == k) return Err(405, "method not allowed");
    return Err(404, "not found");
}

DWORD WINAPI ConnThread(LPVOID arg) {
    SOCKET c = (SOCKET)(uintptr_t)arg;
    DWORD to = 10000;
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to, sizeof to);
    setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, (const char*)&to, sizeof to);
    Request r;
    int errStatus = 0;
    if (ReadRequest(c, r, errStatus)) {
        Response resp = TokenOk(r) ? Route(r) : Err(401, g_token.empty() ? "plugin token not configured" : "unauthorized");
        Send(c, resp);
    } else if (errStatus) {
        Send(c, Err(errStatus, "bad request"));
    }
    shutdown(c, SD_BOTH);
    closesocket(c);
    return 0;
}

void LoadToken() {
    char env[512];
    DWORD n = GetEnvironmentVariableA("TAKARO_PLUGIN_TOKEN", env, sizeof env);
    if (n > 0 && n < sizeof env) {
        g_token = env;
        PluginLog("http: token from env TAKARO_PLUGIN_TOKEN");
        return;
    }
    std::string path = PluginBaseDir() + "\\takaro\\plugin.json";
    FILE* f = fopen(path.c_str(), "rb");
    if (f) {
        std::string text;
        char buf[4096];
        size_t k;
        while ((k = fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, k);
        fclose(f);
        JsonValue v;
        if (JsonParse(text, v)) {
            auto* t = v.get("token");
            if (t && t->isStr() && !t->str.empty()) {
                g_token = t->str;
                PluginLog("http: token from takaro\\plugin.json");
                return;
            }
        }
        PluginLog("http: takaro\\plugin.json has no usable \"token\"");
    }
    PluginLog("http: WARNING no token configured; all requests will be rejected with 401");
}

DWORD WINAPI ListenThread(LPVOID) {
    WSADATA w;
    if (WSAStartup(MAKEWORD(2, 2), &w)) {
        PluginLog("http: WSAStartup failed");
        return 1;
    }
    SOCKET s = INVALID_SOCKET;
    for (int attempt = 0;; attempt++) {
        s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(kPort);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (s != INVALID_SOCKET && bind(s, (sockaddr*)&a, sizeof a) == 0 && listen(s, 32) == 0) break;
        PluginLog("http: bind/listen failed err=%d (attempt %d), retrying", WSAGetLastError(), attempt);
        if (s != INVALID_SOCKET) closesocket(s);
        Sleep(5000);
    }
    PluginLog("http: listening on 127.0.0.1:%d", kPort);
    for (;;) {
        SOCKET c = accept(s, nullptr, nullptr);
        if (c == INVALID_SOCKET) {
            Sleep(10);
            continue;
        }
        HANDLE h = CreateThread(nullptr, 0, ConnThread, (LPVOID)(uintptr_t)c, 0, nullptr);
        if (h) CloseHandle(h);
        else closesocket(c);
    }
}

}  // namespace

void AccountsHousekeep() {
    for (auto& sm : PluginState::Get().OnlineMachines()) {
        uint64_t h;
        if (CachedAccount(sm.first, h)) continue;
        std::string how;
        if (ResolveAccount(sm.first, h, how))
            PluginLog("accounts: cached steam=%s accountHash=%llu (%s)", sm.first.c_str(), (unsigned long long)h, how.c_str());
    }
}

void HttpStart() {
    LoadToken();
    HANDLE h = CreateThread(nullptr, 0, ListenThread, nullptr, 0, nullptr);
    if (h) CloseHandle(h);
}
