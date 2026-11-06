#include "probe_core.h"

#include "common.h"

#include <cctype>
#include <set>

namespace probe {
namespace {

std::string LowerAscii(std::string s) {
    for (auto& c : s)
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}

// Same set as the connector's chat mapping (native::Str -> Trim, C isspace), so the probe claims exactly the text
// Takaro would see and match against its prefix.
bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r'; }

// A non-negative JSON integer (digits only, no overflow); false for anything else.
bool U64(const JsonValue* v, uint64_t& out) {
    if (!v || !v->isNum() || v->str.empty() || v->str.size() > 19) return false;
    out = 0;
    for (char c : v->str) {
        if (c < '0' || c > '9') return false;
        out = out * 10 + (uint64_t)(c - '0');
    }
    return true;
}

const uint64_t kMaxAgeLimitSeconds = 7 * 24 * 3600;

}  // namespace

bool IsReserved(const std::string& lowerName) { return lowerName == "link"; }

bool ParseCatalog(const std::string& text, Catalog& out, std::string& err) {
    JsonValue root;
    if (!JsonParse(text, root) || root.type != JsonValue::Object) {
        err = "catalog is not a JSON object";
        return false;
    }
    Catalog c;
    if (auto* v = root.get("revision"); v && v->isStr()) c.revision = v->str;
    if (auto* v = root.get("prefix"); v && v->isStr()) c.prefix = v->str;
    if (auto* v = root.get("nonce"); v && v->isStr()) c.nonce = LowerAscii(v->str);
    uint64_t maxAge = 0;
    if (!U64(root.get("issuedAtMs"), c.issuedAtMs) || c.issuedAtMs == 0) {
        err = "issuedAtMs must be a positive integer (Unix ms)";
        return false;
    }
    if (!U64(root.get("maxAgeSeconds"), maxAge) || maxAge == 0 || maxAge > kMaxAgeLimitSeconds) {
        err = "maxAgeSeconds must be an integer 1.." + std::to_string(kMaxAgeLimitSeconds);
        return false;
    }
    c.maxAgeMs = maxAge * 1000;
    if (auto* v = root.get("available"); v && v->type == JsonValue::Bool) c.available = v->b;
    if (auto* v = root.get("failDuringSend"); v && v->type == JsonValue::Bool) c.failDuringSend = v->b;
    if (c.prefix.empty() || c.prefix.size() > 8) {
        err = "prefix must be 1..8 bytes";
        return false;
    }
    if (c.nonce.empty() || c.nonce.size() > 32) {
        err = "nonce must be 1..32 bytes";
        return false;
    }
    if (IsReserved(c.nonce)) {
        err = "nonce collides with the reserved name '" + c.nonce + "'";
        return false;
    }
    if (auto* cmds = root.get("commands"); cmds && cmds->type == JsonValue::Array) {
        for (auto& j : cmds->arr) {
            Command cmd;
            if (auto* v = j.get("name"); v && v->isStr()) cmd.name = LowerAscii(v->str);
            if (cmd.name.empty()) continue;
            if (auto* v = j.get("aliases"); v && v->type == JsonValue::Array)
                for (auto& a : v->arr)
                    if (a.isStr() && !a.str.empty()) cmd.aliases.push_back(LowerAscii(a.str));
            if (auto* v = j.get("enabled"); v && v->type == JsonValue::Bool) cmd.enabled = v->b;
            if (auto* v = j.get("permission"); v && v->isStr()) cmd.permission = v->str;
            c.commands.push_back(cmd);
        }
    }
    for (int i = 0; i < (int)c.commands.size(); i++) {
        std::set<std::string> own{c.commands[i].name};
        own.insert(c.commands[i].aliases.begin(), c.commands[i].aliases.end());
        for (auto& t : own) {
            if (t == c.nonce) {
                err = "nonce collides with command trigger '" + t + "'";
                return false;
            }
            auto it = c.triggers.find(t);
            if (it == c.triggers.end()) c.triggers[t] = i;
            else it->second = -1;
        }
    }
    if (auto* g = root.get("grants"); g && g->type == JsonValue::Object)
        for (auto& kv : g->obj)
            if (kv.second.type == JsonValue::Array)
                for (auto& p : kv.second.arr)
                    if (p.isStr()) c.grants[kv.first].push_back(p.str);
    out = std::move(c);
    return true;
}

Decision Decide(const std::string& text, const Catalog* cat, uint64_t nowMs, const Sender& sender) {
    Decision d;
    if (!cat) {
        d.reason = "probe off";
        return d;
    }
    size_t b = 0, end = text.size();
    while (b < end && IsSpace(text[b])) b++;
    while (end > b && IsSpace(text[end - 1])) end--;
    const std::string t = text.substr(b, end - b);
    if (t.compare(0, cat->prefix.size(), cat->prefix) != 0) {
        d.reason = "no prefix";
        return d;
    }
    size_t p = cat->prefix.size(), e = p;
    while (e < t.size() && !IsSpace(t[e])) e++;
    d.trigger = LowerAscii(t.substr(p, e - p));
    while (e < t.size() && IsSpace(t[e])) e++;
    d.args = t.substr(e);
    if (d.trigger.empty()) {
        d.reason = "no command name";
        return d;
    }
    auto it = cat->triggers.find(d.trigger);
    bool isNonce = d.trigger == cat->nonce, reserved = IsReserved(d.trigger);
    if (it == cat->triggers.end() && !isNonce && !reserved) {
        d.reason = "unknown command";
        return d;
    }
    // From here on the name is claimed: it never reaches public chat, whatever goes wrong.
    d.kind = Kind::Reject;
    if (!sender.valid) {
        d.reason = "sender not verified (" + sender.reason + ")";
        return d;
    }
    if (nowMs < cat->issuedAtMs || nowMs - cat->issuedAtMs > cat->maxAgeMs) {
        d.reason = "command list is out of date";
        return d;
    }
    if (!cat->available) {
        d.reason = "Takaro is unavailable";
        return d;
    }
    if (reserved) {
        d.reason = "reserved command";
        return d;
    }
    if (isNonce) {
        d.command = cat->nonce;
        d.kind = Kind::Invoke;
        return d;
    }
    if (it->second < 0) {
        d.reason = "ambiguous command";
        return d;
    }
    const Command& cmd = cat->commands[it->second];
    d.command = cmd.name;
    if (!cmd.enabled) {
        d.reason = "command disabled";
        return d;
    }
    if (!cmd.permission.empty()) {
        auto g = cat->grants.find(sender.steamId);
        bool ok = false;
        if (g != cat->grants.end())
            for (auto& perm : g->second) ok = ok || perm == cmd.permission;
        if (!ok) {
            d.reason = "permission denied";
            return d;
        }
    }
    d.kind = Kind::Invoke;
    return d;
}

std::string ReplyText(const Decision& d, const std::string& invocationId, bool failed) {
    std::string name = d.command.empty() ? d.trigger : d.command;
    if (d.kind == Kind::Invoke)
        return failed ? "[probe] " + name + ": failed, not run (id " + invocationId + ")"
                      : "[probe] " + name + ": submitted (id " + invocationId + ")";
    return "[probe] " + name + ": " + d.reason;
}

std::string Hex(const std::string& bytes) {
    static const char* k = "0123456789abcdef";
    std::string o;
    o.reserve(bytes.size() * 2);
    for (unsigned char c : bytes) {
        o += k[c >> 4];
        o += k[c & 15];
    }
    return o;
}

}  // namespace probe
