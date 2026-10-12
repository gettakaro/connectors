// takaro.cfg: KEY=VALUE settings next to libtakaro-dragonwilds.so, the connector's one config file.
// Keys are the documented environment variable names; a non-empty environment variable always wins
// over the file and is never written to it.
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

// TAKARO_CONFIG_FILE when set, otherwise takaro.cfg next to libtakaro-dragonwilds.so; absolute.
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

// ---- the connection settings, which apply without a restart ----

constexpr const char* kDefaultUrl = "wss://connect.takaro.io/";

// The whole file, or nullopt when it cannot be read (missing, permissions).
std::optional<std::string> ReadText(const std::string& path);
Loaded FromText(const std::string& path, const std::string& text);
// True when a line has no '=': the file is half-saved or mistyped, and must not be applied.
bool Unparseable(const Parsed& parsed);

struct Settings {
    std::string url, registration, identity, serverName;
    bool envUrl = false, envRegistration = false, envIdentity = false, envServerName = false;
    bool operator==(const Settings& o) const {
        return url == o.url && registration == o.registration && identity == o.identity &&
               serverName == o.serverName;
    }
    bool operator!=(const Settings& o) const { return !(*this == o); }
};

// Takaro keeps game-server names unique per domain, so an install without a name registers as
// "Dragonwilds (<first 8 identity characters>)".
std::string UniqueServerName(const std::string& identity);

// Per field: a non-empty environment variable, else the file. The URL defaults to kDefaultUrl, the
// identity to "" (not connectable) and the server name to UniqueServerName. `env` is injectable.
Settings Resolve(const Loaded& file, const std::function<const char*(const char*)>& env = ::getenv);

// The file the connector writes when none exists: every connection key, an empty registration
// token, `identity` and its unique name.
std::string Template(const std::string& identity);

enum class Ensured { Unchanged, Created, IdentityAdded, NotWritable };
struct EnsureResult {
    Ensured what = Ensured::Unchanged;
    std::optional<std::string> text;  // the file's text afterwards; nullopt when there is none
};
// Gives the file at `path` an identity: creates it from Template when it is missing, and writes a
// new identity into it when its TAKARO_IDENTITY_TOKEN is empty. `text` is what was just read
// (nullopt: missing). A half-saved, unreadable or meanwhile changed file is left alone. Nothing
// else is ever written.
EnsureResult EnsureIdentity(const std::string& path, const std::optional<std::string>& text,
                            const std::function<std::string()>& newIdentity);

// `text` with KEY's line set to KEY=value (the last matching line, which Parse uses, else appended), keeping every
// other line and the line-ending style.
std::string SetKey(const std::string& text, const std::string& key, const std::string& value);

// Writes via a temp file + rename so a reader never sees half a file; keeps the replaced file's
// mode and, where permitted, its owner.
bool WriteAtomic(const std::string& path, const std::string& text, unsigned mode = 0600);

// A random UUID (version 4).
std::string NewIdentity();

}  // namespace ConfigFile
