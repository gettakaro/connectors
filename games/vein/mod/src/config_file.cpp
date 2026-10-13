#include "config_file.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
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
    while ((n = fread(buf, 1, sizeof buf, f)) > 0 && text.size() <= kMaxBytes) text.append(buf, n);
    const bool failed = ferror(f) != 0;
    fclose(f);
    if (failed) return std::nullopt;
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

bool Unparseable(const Parsed& parsed) {
    for (const auto& w : parsed.warnings)
        if (w.find("no '='") != std::string::npos) return true;
    return false;
}

std::string DefaultServerName(const std::string& identity) {
    return "My VEIN server (" + identity.substr(0, 8) + ")";
}

std::string Template(const std::string& identity) {
    return "# Takaro VEIN connector settings. Environment variables with the same name win over this file.\n"
           "# Paste the registration token from the Takaro dashboard and save; it connects within seconds.\n"
           "TAKARO_REGISTRATION_TOKEN=\n"
           "# This server's identity in Takaro. Keep it when you move or reinstall the server.\n"
           "TAKARO_IDENTITY_TOKEN=" + identity + "\n"
           "# The name Takaro gives this server, also shown in front of Takaro's chat messages.\n"
           "TAKARO_SERVER_NAME=" + DefaultServerName(identity) + "\n"
           "# Optional: SteamID64s that get VEIN's in-game admin rights, comma separated.\n"
           "TAKARO_ADMIN_STEAMIDS=\n";
}

std::optional<std::string> Ensure(const std::string& path, bool create, std::string& note,
                                  const std::function<std::string()>& newIdentity) {
    note.clear();
    auto text = ReadText(path);
    if (!text) {
        struct stat st{};
        if (::stat(path.c_str(), &st) == 0 || errno != ENOENT) {
            note = "cannot read " + path;
            return std::nullopt;  // there, but unreadable: never replace it
        }
        if (!create) return std::nullopt;
        const std::string fresh = Template(newIdentity());
        if (!WriteAtomic(path, fresh, 0600, nullptr, true)) {
            note = "could not create " + path + ": " + strerror(errno);
            return std::nullopt;
        }
        note = "created " + path + " with a new server identity";
        return fresh;
    }
    const Parsed parsed = Parse(*text);
    // Half-saved (the next settled read decides) or too large to rewrite safely.
    if (Unparseable(parsed) || text->size() > kMaxBytes) return text;
    std::string identity;
    for (const auto& e : parsed.entries)
        if (e.first == "TAKARO_IDENTITY_TOKEN") identity = Trim(e.second);
    if (!identity.empty()) return text;
    const std::string next = SetKey(*text, "TAKARO_IDENTITY_TOKEN", newIdentity());
    if (!WriteAtomic(path, next, 0600, &*text)) {
        note = "could not write a server identity into " + path + ": " + strerror(errno);
        return text;
    }
    note = "wrote a new server identity into " + path;
    return next;
}

const char* SourceName(Source s) {
    switch (s) {
    case Source::Env: return "environment";
    case Source::File: return "takaro.cfg";
    case Source::Default: return "default";
    }
    return "default";
}

Resolution Resolve(const Loaded& file, const std::function<const char*(const char*)>& env) {
    Resolution r;
    auto pick = [&](const char* key, std::string& out, Source& src) {
        const char* e = env(key);
        std::string v = e ? Trim(e) : "";
        if (!v.empty()) { out = v; src = Source::Env; return; }
        auto it = file.values.find(key);
        v = it == file.values.end() ? "" : Trim(it->second);
        if (!v.empty()) { out = v; src = Source::File; return; }
        out.clear(); src = Source::Default;
    };
    pick("TAKARO_WS_URL", r.settings.url, r.url);
    if (r.settings.url.empty()) r.settings.url = kDefaultUrl;
    pick("TAKARO_REGISTRATION_TOKEN", r.settings.registration, r.registration);
    pick("TAKARO_IDENTITY_TOKEN", r.settings.identity, r.identity);
    pick("TAKARO_SERVER_NAME", r.settings.serverName, r.serverName);
    if (r.settings.serverName.empty()) r.settings.serverName = DefaultServerName(r.settings.identity);
    return r;
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

bool WriteAtomic(const std::string& path, const std::string& text, unsigned mode, const std::string* expected,
                 bool create) {
    struct stat st{};
    const bool existed = stat(path.c_str(), &st) == 0;
    if (existed) mode = st.st_mode & 07777;
    std::string tmp = path + ".XXXXXX";
    int fd = mkstemp(tmp.data());  // unique, O_EXCL: never follows a planted link
    if (fd < 0) return false;
    if (existed) (void)!fchown(fd, st.st_uid, st.st_gid);  // only root may give a file away
    (void)fchmod(fd, mode);
    bool ok = true;
    size_t off = 0;
    while (ok && off < text.size()) {
        ssize_t n = write(fd, text.data() + off, text.size() - off);
        if (n <= 0) ok = false; else off += static_cast<size_t>(n);
    }
    ok = ok && fsync(fd) == 0;
    ok = close(fd) == 0 && ok;
    if (ok && expected) {  // someone saved meanwhile: keep their file, the next poll sees it
        auto now = ReadText(path);
        if (!now || *now != *expected) { errno = EAGAIN; ok = false; }
    }
    // create: link() fails when the file appeared meanwhile, so a concurrent writer is never replaced.
    if (ok) ok = create ? link(tmp.c_str(), path.c_str()) == 0 : rename(tmp.c_str(), path.c_str()) == 0;
    const int err = errno;
    if (create || !ok) unlink(tmp.c_str());
    if (ok) {
        const size_t slash = path.rfind('/');
        const std::string dir = slash == std::string::npos ? "." : slash == 0 ? "/" : path.substr(0, slash);
        int d = open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (d >= 0) { (void)fsync(d); close(d); }
    }
    errno = err;  // the caller reports why
    return ok;
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
    static const Loaded& loaded = *new Loaded([] {  // still read by exit handlers
        const std::string path = DefaultPath();
        std::string note;
        auto text = Ensure(path, true, note);
        Loaded l;
        if (text) l = FromText(path, *text);
        l.path = path;
        l.note = note;
        return l;
    }());
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
