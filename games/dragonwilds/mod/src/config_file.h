// takaro.cfg: KEY=VALUE settings next to libtakaro-dragonwilds.so. Keys are the documented
// environment variable names; a non-empty environment variable always wins over the file.
#pragma once

#include <cstdlib>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ConfigFile {

struct Parsed {
    std::vector<std::pair<std::string, std::string>> entries;  // in file order, last one wins
    std::vector<std::string> warnings;                          // "line N: why" (never the value)
};

// KEY=VALUE per line. Blank lines and lines starting with # or ; are ignored; whitespace around
// key and value, a UTF-8 BOM, CR line ends, an "export " prefix and one pair of matching quotes
// around the value are stripped. Only TAKARO_* and DRAGONWILDS_* keys are accepted.
Parsed Parse(const std::string& text);

// TAKARO_CONFIG_FILE when set, otherwise takaro.cfg next to libtakaro-dragonwilds.so.
std::string DefaultPath();

struct Loaded {
    std::string path;
    bool found = false;
    std::map<std::string, std::string> values;
    std::vector<std::string> warnings;
};

// Reads and parses `path`; a missing file is not an error.
Loaded Load(const std::string& path);

// The file at DefaultPath(), read once on first use. Immutable afterwards, so any thread may read it.
// Only the connection settings below are re-read while the server runs.
const Loaded& Current();

// The setting `name`: a non-empty environment variable, else the file's value, else the
// environment value as is (nullptr when unset, "" when explicitly empty). Never calls setenv.
const char* Lookup(const Loaded& file, const char* name);
inline const char* Get(const char* name) { return Lookup(Current(), name); }

// ---- settings that apply without a restart ----

constexpr const char* kDefaultUrl = "wss://connect.takaro.io/";
// What releases up to 0.3 used when no identity or name was set. Installs that ran one keep them.
constexpr const char* kLegacyIdentity = "dragonwilds";
constexpr const char* kLegacyServerName = "Dragonwilds";
// The connector's own copy of the settings it connected with, in the state directory, which no
// upgrade replaces. A value set in takaro.cfg (or the environment) wins over it.
constexpr const char* kSavedFileName = "saved-settings.cfg";

// The whole file, or nullopt when it cannot be read (missing, permissions).
std::optional<std::string> ReadText(const std::string& path);
Loaded FromText(const std::string& path, const std::string& text);
// True when a line has no '=': the file is half-saved or mistyped, and must not be applied.
bool Unparseable(const Parsed& parsed);

enum class Source { Env, File, Saved, Current, Legacy, Generated, Default };
const char* SourceName(Source s);

struct Settings {
    std::string url, registration, identity, serverName;
    bool operator==(const Settings& o) const {
        return url == o.url && registration == o.registration && identity == o.identity &&
               serverName == o.serverName;
    }
    bool operator!=(const Settings& o) const { return !(*this == o); }
};
struct Resolution {
    Settings settings;
    Source url = Source::Default, registration = Source::Default, identity = Source::Default,
           serverName = Source::Default;
};

// Takaro keeps game-server names unique per domain, so a fresh install registers as
// "Dragonwilds (<first 8 identity characters>)".
std::string UniqueServerName(const std::string& identity);

// Per field: a non-empty environment variable, else takaro.cfg, else the saved copy. The URL is
// taken from takaro.cfg only when it differs from the default, so a freshly shipped file cannot
// undo a saved custom URL. The identity falls back to `current` (the one this run already uses),
// then to "dragonwilds" for an install that ran an older connector (`legacyInstall`), and only
// then to `newIdentity()`. The server name falls back to "Dragonwilds" for such an install and to
// UniqueServerName otherwise. `getenv` is injectable for tests.
Resolution Resolve(const Loaded& user, const Loaded& saved, bool legacyInstall, const std::string& current,
                   const std::function<std::string()>& newIdentity,
                   const std::function<const char*(const char*)>& env = ::getenv);

// The saved copy to write: the URL, identity, server name and (only when `withRegistration`) the
// registration token in use. Values from the environment are not written; the old saved value stays.
std::string RenderSaved(const Resolution& r, const Loaded& oldSaved, bool withRegistration);

// `text` with KEY's line set to KEY=value (the first matching line, else appended), keeping every
// other line and the line-ending style.
std::string SetKey(const std::string& text, const std::string& key, const std::string& value);

// Writes via a temp file + rename so a reader never sees half a file; keeps the replaced file's
// mode and, where permitted, its owner.
bool WriteAtomic(const std::string& path, const std::string& text, unsigned mode = 0600);

// A random UUID (version 4).
std::string NewIdentity();

}  // namespace ConfigFile
