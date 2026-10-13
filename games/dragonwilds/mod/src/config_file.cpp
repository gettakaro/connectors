#include "config_file.h"

#include "common.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <ctime>

namespace ConfigFile {
namespace {

std::string Trim(const std::string& s) {
    const char* ws = " \t\r\n";
    size_t b = s.find_first_not_of(ws);
    if (b == std::string::npos) return "";
    return s.substr(b, s.find_last_not_of(ws) - b + 1);
}

bool ValidKey(const std::string& k) {
    if (k.rfind("TAKARO_", 0) != 0 && k.rfind("DRAGONWILDS_", 0) != 0) return false;
    for (unsigned char c : k)
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) return false;
    return true;
}

}  // namespace

Parsed Parse(const std::string& input) {
    Parsed out;
    std::string text = input;
    if (text.rfind("\xEF\xBB\xBF", 0) == 0) text.erase(0, 3);
    size_t start = 0, lineNo = 0;
    while (start <= text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = Trim(text.substr(start, end - start));
        ++lineNo;
        start = end + 1;
        if (line.empty() || line[0] == '#' || line[0] == ';') {
            if (end == text.size()) break;
            continue;
        }
        if (line.rfind("export ", 0) == 0) line = Trim(line.substr(7));
        size_t eq = line.find('=');
        if (eq == std::string::npos) {
            out.warnings.push_back("line " + std::to_string(lineNo) + ": no '=' (expected KEY=VALUE)");
        } else {
            std::string key = Trim(line.substr(0, eq));
            std::string value = Trim(line.substr(eq + 1));
            if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') && value.back() == value.front())
                value = value.substr(1, value.size() - 2);
            if (!ValidKey(key))
                out.warnings.push_back("line " + std::to_string(lineNo) + ": ignored key '" + key +
                                       "' (only TAKARO_* and DRAGONWILDS_* keys are read)");
            else
                out.entries.emplace_back(key, value);
        }
        if (end == text.size()) break;
    }
    return out;
}

std::string DefaultPath() {
    std::string path = "takaro.cfg";
    Dl_info info{};
    if (const char* forced = getenv("TAKARO_CONFIG_FILE"); forced && *forced) {
        path = forced;
    } else if (dladdr(reinterpret_cast<void*>(&DefaultPath), &info) && info.dli_fname) {
        std::string lib(info.dli_fname);
        size_t slash = lib.rfind('/');
        if (slash != std::string::npos) path = lib.substr(0, slash + 1) + "takaro.cfg";
    }
    char cwd[4096];
    if (path[0] != '/' && getcwd(cwd, sizeof cwd)) path = std::string(cwd) + "/" + path;
    return path;
}

std::optional<std::string> ReadText(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return std::nullopt;
    std::string text;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0 && text.size() < 256 * 1024) text.append(buf, n);
    fclose(f);
    return text;
}

Loaded FromText(const std::string& path, const std::string& text) {
    Loaded l;
    l.path = path;
    l.found = true;
    Parsed p = Parse(text);
    l.warnings = p.warnings;
    for (auto& e : p.entries) l.values[e.first] = e.second;
    return l;
}

Loaded Load(const std::string& path) {
    auto text = ReadText(path);
    if (!text) { Loaded l; l.path = path; return l; }
    return FromText(path, *text);
}

const Loaded& Current() {
    static const Loaded& loaded = *new Loaded(Load(DefaultPath()));  // still read by exit handlers
    return loaded;
}

const char* Lookup(const Loaded& file, const char* name) {
    const char* env = getenv(name);
    if (env && *env) return env;
    auto it = file.values.find(name);
    if (it != file.values.end()) return it->second.c_str();
    return env;
}

bool Unparseable(const Parsed& parsed) {
    for (const auto& w : parsed.warnings)
        if (w.find("no '='") != std::string::npos) return true;
    return false;
}

std::string UniqueServerName(const std::string& identity) {
    return "Dragonwilds (" + identity.substr(0, 8) + ")";
}

Settings Resolve(const Loaded& file, const std::function<const char*(const char*)>& env) {
    auto pick = [&](const char* key, std::string& out, bool& fromEnv) {
        const char* e = env(key);
        out = e ? Trim(e) : "";
        fromEnv = !out.empty();
        if (fromEnv) return;
        auto it = file.values.find(key);
        if (it != file.values.end()) out = Trim(it->second);
    };
    Settings s;
    pick("TAKARO_WS_URL", s.url, s.envUrl);
    pick("TAKARO_REGISTRATION_TOKEN", s.registration, s.envRegistration);
    pick("TAKARO_IDENTITY_TOKEN", s.identity, s.envIdentity);
    pick("TAKARO_SERVER_NAME", s.serverName, s.envServerName);
    if (s.url.empty()) s.url = kDefaultUrl;
    if (s.serverName.empty() && !s.identity.empty()) s.serverName = UniqueServerName(s.identity);
    return s;
}

std::string Template(const std::string& identity) {
    return "# Takaro connector settings for this RuneScape: Dragonwilds server. One KEY=VALUE per line.\n"
           "# Saving is enough: the connector re-reads this file within a few seconds, no restart.\n"
           "# A non-empty environment variable with the same name wins over the line here.\n"
           "\n"
           "# The registration token of your Takaro Generic game server.\n"
           "TAKARO_REGISTRATION_TOKEN=\n"
           "\n"
           "# This server's identity in Takaro, created by the connector. Keep it.\n"
           "TAKARO_IDENTITY_TOKEN=" + identity + "\n"
           "\n"
           "# The name Takaro shows for this server; unique in your Takaro domain.\n"
           "TAKARO_SERVER_NAME=" + UniqueServerName(identity) + "\n"
           "\n"
           "TAKARO_WS_URL=" + kDefaultUrl + "\n";
}

EnsureResult EnsureIdentity(const std::string& path, const std::optional<std::string>& text,
                            const std::function<std::string()>& newIdentity) {
    EnsureResult r;
    r.text = text;
    std::string next;
    struct stat st{};
    if (!text && stat(path.c_str(), &st) == 0) {  // there, but unreadable: never replace it
        PluginLog("config: cannot read %s", path.c_str());
        r.what = Ensured::NotWritable;
        return r;
    }
    if (!text) {
        next = Template(newIdentity());
        r.what = Ensured::Created;
    } else {
        const Parsed parsed = Parse(*text);
        if (Unparseable(parsed)) return r;
        std::string identity;
        for (const auto& e : parsed.entries)
            if (e.first == "TAKARO_IDENTITY_TOKEN") identity = Trim(e.second);
        if (!identity.empty()) return r;
        next = SetKey(*text, "TAKARO_IDENTITY_TOKEN", newIdentity());
        r.what = Ensured::IdentityAdded;
    }
    if (ReadText(path) != text) {  // saved meanwhile: the next read applies that text
        r.what = Ensured::Unchanged;
        return r;
    }
    if (!WriteAtomic(path, next, 0600)) {
        PluginLog("config: cannot write %s", path.c_str());
        r.what = Ensured::NotWritable;
        return r;
    }
    PluginLog(r.what == Ensured::Created ? "config: created %s with a new identity"
                                         : "config: wrote a new identity into %s", path.c_str());
    r.text = next;
    return r;
}

std::string SetKey(const std::string& text, const std::string& key, const std::string& value) {
    const bool crlf = text.find("\r\n") != std::string::npos;
    size_t start = 0, matchStart = std::string::npos, matchEnd = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = Trim(text.substr(start, end - start));
        if (line.rfind("\xEF\xBB\xBF", 0) == 0) line = Trim(line.substr(3));
        if (line.rfind("export ", 0) == 0) line = Trim(line.substr(7));
        size_t eq = line.find('=');
        if (eq != std::string::npos && Trim(line.substr(0, eq)) == key) {  // the last one wins in Parse
            matchStart = (start == 0 && text.rfind("\xEF\xBB\xBF", 0) == 0) ? 3 : start;
            matchEnd = (end > start && text[end - 1] == '\r') ? end - 1 : end;
        }
        start = end + 1;
    }
    if (matchStart != std::string::npos)
        return text.substr(0, matchStart) + key + "=" + value + text.substr(matchEnd);
    std::string out = text;
    const char* nl = crlf ? "\r\n" : "\n";
    if (!out.empty() && out.back() != '\n') out += nl;
    return out + key + "=" + value + nl;
}

bool WriteAtomic(const std::string& path, const std::string& text, unsigned mode) {
    struct stat st{};
    const bool replacing = stat(path.c_str(), &st) == 0;
    if (replacing) mode = st.st_mode & 07777;
    std::string tmp = path + ".takaro-XXXXXX";
    int fd = mkstemp(tmp.data());
    if (fd < 0) return false;
    (void)fchmod(fd, mode);
    // Root (or the owner) can hand the file back to whoever owned it; anyone else keeps their own.
    if (replacing && fchown(fd, st.st_uid, st.st_gid) != 0) { /* not permitted: keep the writer as owner */ }
    bool ok = true;
    size_t off = 0;
    while (ok && off < text.size()) {
        ssize_t n = write(fd, text.data() + off, text.size() - off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) ok = false; else off += static_cast<size_t>(n);
    }
    ok = ok && fsync(fd) == 0;
    ok = close(fd) == 0 && ok;
    if (!ok || rename(tmp.c_str(), path.c_str()) != 0) { unlink(tmp.c_str()); return false; }
    return true;
}

std::string NewIdentity() {
    unsigned char b[16] = {};
    bool filled = false;
    if (FILE* f = fopen("/dev/urandom", "rb")) {
        filled = fread(b, 1, sizeof b, f) == sizeof b;
        fclose(f);
    }
    if (!filled) {
        // Never expected on Linux; time and pid still make a collision within one domain unlikely.
        unsigned long long seed = static_cast<unsigned long long>(time(nullptr)) ^ (static_cast<unsigned long long>(getpid()) << 32);
        for (auto& c : b) { seed = seed * 6364136223846793005ULL + 1442695040888963407ULL; c = static_cast<unsigned char>(seed >> 56); }
    }
    b[6] = static_cast<unsigned char>((b[6] & 0x0F) | 0x40);
    b[8] = static_cast<unsigned char>((b[8] & 0x3F) | 0x80);
    char out[37];
    snprintf(out, sizeof out, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    return out;
}

}  // namespace ConfigFile
