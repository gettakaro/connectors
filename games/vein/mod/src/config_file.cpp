#include "config_file.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

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
    if (k.rfind("TAKARO_", 0) != 0 && k.rfind("VEIN_", 0) != 0) return false;
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
                                       "' (only TAKARO_* and VEIN_* keys are read)");
            else
                out.entries.emplace_back(key, value);
        }
        if (end == text.size()) break;
    }
    return out;
}

std::string DefaultPath() {
    const char* forced = getenv("TAKARO_CONFIG_FILE");
    if (forced && *forced) return forced;
    Dl_info info{};
    if (dladdr(reinterpret_cast<void*>(&DefaultPath), &info) && info.dli_fname) {
        std::string lib(info.dli_fname);
        size_t slash = lib.rfind('/');
        if (slash != std::string::npos) return lib.substr(0, slash + 1) + "takaro.cfg";
    }
    return "takaro.cfg";
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
    for (auto& e : p.entries) l.values[e.first] = e.second;  // last occurrence wins
    return l;
}

Loaded Load(const std::string& path) {
    auto text = ReadText(path);
    if (!text) { Loaded l; l.path = path; return l; }
    return FromText(path, *text);
}

bool Unparseable(const Parsed& parsed) {
    for (const auto& w : parsed.warnings)
        if (w.find("no '='") != std::string::npos) return true;
    return false;
}

const char* SourceName(Source s) {
    switch (s) {
    case Source::Env: return "environment";
    case Source::File: return "takaro.cfg";
    case Source::Saved: return "saved copy";
    case Source::Current: return "current";
    case Source::Legacy: return "legacy default";
    case Source::Generated: return "generated";
    case Source::Default: return "default";
    }
    return "default";
}

Resolution Resolve(const Loaded& user, const Loaded& saved, bool legacyInstall, const std::string& current,
                   const std::function<std::string()>& newIdentity,
                   const std::function<const char*(const char*)>& env) {
    auto envValue = [&](const char* key) -> std::string {
        const char* v = env(key);
        return v ? Trim(v) : "";
    };
    auto fileValue = [](const Loaded& l, const char* key) -> std::string {
        auto it = l.values.find(key);
        return it == l.values.end() ? "" : Trim(it->second);
    };
    Resolution r;
    auto pick = [&](const char* key, std::string& out, Source& src, bool userOnlyWhenNotDefault,
                    const char* def, bool useSaved) {
        std::string v;
        if (!(v = envValue(key)).empty()) { out = v; src = Source::Env; return true; }
        v = fileValue(user, key);
        if (!v.empty() && !(userOnlyWhenNotDefault && def && v == def)) { out = v; src = Source::File; return true; }
        if (useSaved && !(v = fileValue(saved, key)).empty()) { out = v; src = Source::Saved; return true; }
        return false;
    };
    if (!pick("TAKARO_WS_URL", r.settings.url, r.url, true, kDefaultUrl, true)) {
        r.settings.url = kDefaultUrl; r.url = Source::Default;
    }
    if (!pick("TAKARO_REGISTRATION_TOKEN", r.settings.registration, r.registration, false, nullptr, true)) {
        r.settings.registration.clear(); r.registration = Source::Default;
    }
    if (!pick("TAKARO_SERVER_NAME", r.settings.serverName, r.serverName, false, nullptr, false)) {
        r.settings.serverName = kDefaultServerName; r.serverName = Source::Default;
    }
    if (!pick("TAKARO_IDENTITY_TOKEN", r.settings.identity, r.identity, false, nullptr, true)) {
        if (!current.empty()) { r.settings.identity = current; r.identity = Source::Current; }
        else if (legacyInstall) { r.settings.identity = kLegacyIdentity; r.identity = Source::Legacy; }
        else { r.settings.identity = newIdentity(); r.identity = Source::Generated; }
    }
    return r;
}

std::string RenderSaved(const Resolution& r, const Loaded& oldSaved, bool withRegistration) {
    auto old = [&](const char* key) {
        auto it = oldSaved.values.find(key);
        return it == oldSaved.values.end() ? std::string() : Trim(it->second);
    };
    auto value = [&](const char* key, const std::string& inUse, Source src, bool write) {
        return std::string(key) + "=" + (write && src != Source::Env && src != Source::Default ? inUse : old(key)) + "\n";
    };
    return "# Written by the Takaro connector: the settings it last connected with. It sits outside\n"
           "# what an upgrade replaces, so a replaced takaro.cfg keeps this server's token and identity.\n"
           "# Edit takaro.cfg next to libtakaro-vein.so instead; a value set there wins over this file.\n" +
           value("TAKARO_WS_URL", r.settings.url, r.url, true) +
           value("TAKARO_REGISTRATION_TOKEN", r.settings.registration, r.registration, withRegistration) +
           value("TAKARO_IDENTITY_TOKEN", r.settings.identity, r.identity, true);
}

std::string SetKey(const std::string& text, const std::string& key, const std::string& value) {
    const bool crlf = text.find("\r\n") != std::string::npos;
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = Trim(text.substr(start, end - start));
        if (line.rfind("\xEF\xBB\xBF", 0) == 0) line = Trim(line.substr(3));
        if (line.rfind("export ", 0) == 0) line = Trim(line.substr(7));
        size_t eq = line.find('=');
        if (eq != std::string::npos && Trim(line.substr(0, eq)) == key) {
            size_t contentEnd = (end > start && text[end - 1] == '\r') ? end - 1 : end;
            size_t lineStart = start;
            if (lineStart == 0 && text.rfind("\xEF\xBB\xBF", 0) == 0) lineStart = 3;
            return text.substr(0, lineStart) + key + "=" + value + text.substr(contentEnd);
        }
        start = end + 1;
    }
    std::string out = text;
    const char* nl = crlf ? "\r\n" : "\n";
    if (!out.empty() && out.back() != '\n') out += nl;
    return out + key + "=" + value + nl;
}

bool WriteAtomic(const std::string& path, const std::string& text, unsigned mode) {
    struct stat st{};
    if (stat(path.c_str(), &st) == 0) mode = st.st_mode & 07777;
    const std::string tmp = path + ".tmp";
    int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (fd < 0) return false;
    (void)fchmod(fd, mode);  // open() applied the umask
    bool ok = true;
    size_t off = 0;
    while (ok && off < text.size()) {
        ssize_t n = write(fd, text.data() + off, text.size() - off);
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

}  // namespace ConfigFile
