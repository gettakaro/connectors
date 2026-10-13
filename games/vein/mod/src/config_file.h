// takaro.cfg: the connector's one settings file, next to libtakaro-vein.so (TAKARO_CONFIG_FILE
// overrides the path). Keys are the documented environment variable names; a real, non-empty
// environment variable always wins over the file and is never written to it.
#pragma once

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <cstdlib>
#include <utility>
#include <vector>

namespace ConfigFile {

struct Parsed {
    std::vector<std::pair<std::string, std::string>> entries;  // in file order, last one wins
    std::vector<std::string> warnings;                          // "line N: why" (never the value)
};

// KEY=VALUE per line. Blank lines and lines starting with # or ; are ignored; whitespace around
// key and value, a UTF-8 BOM, CR line ends, an "export " prefix and one pair of matching quotes
// around the value are stripped. Only TAKARO_* and VEIN_* keys are accepted.
Parsed Parse(const std::string& text);
// True when a line has no '=': the file is half-saved or mistyped, and must not be applied.
bool Unparseable(const Parsed& parsed);

// TAKARO_CONFIG_FILE when set, otherwise takaro.cfg next to libtakaro-vein.so.
std::string DefaultPath();

struct Loaded {
    std::string path;
    bool found = false;
    std::map<std::string, std::string> values;
    std::vector<std::string> warnings;
    std::string note;  // what Ensure did or why it could not ("" when the file was fine)
};

constexpr size_t kMaxBytes = 256 * 1024;  // a larger file is read in part and never rewritten
// The whole file, or nullopt when it cannot be read (missing, permissions, I/O error).
std::optional<std::string> ReadText(const std::string& path);
Loaded FromText(const std::string& path, const std::string& text);

// A random UUID (version 4).
std::string NewIdentity();
// The file the connector writes when there is none, with this identity and an unused token.
std::string Template(const std::string& identity);
// "My VEIN server (<first 8 characters of the identity>)", the name used when none is set.
std::string DefaultServerName(const std::string& identity);

// The text of `path` after making sure it names a server identity: a missing file is created
// from Template() (only when `create`), and a file whose TAKARO_IDENTITY_TOKEN is empty gets a
// new one written into it. Nothing else is ever written. nullopt when the file does not exist
// and could not be created; `note` says what was written or what failed.
std::optional<std::string> Ensure(const std::string& path, bool create, std::string& note,
                                  const std::function<std::string()>& newIdentity = NewIdentity);

// The file at DefaultPath(), ensured and read once on first use (as the library loads). Immutable
// afterwards, so any thread may read it.
const Loaded& Current();

// The setting `name`: a non-empty environment variable, else the file's value, else the
// environment value as is (nullptr when unset, "" when explicitly empty). Never calls setenv, so
// it is safe when the library is loaded after the game has started threads.
const char* Lookup(const Loaded& file, const char* name);
inline const char* Get(const char* name) { return Lookup(Current(), name); }

// ---- settings that apply without a restart ----
// URL, registration token, identity and server name are re-read from the file while the server
// runs (the bridge thread polls it); every other key keeps the value read at startup.

constexpr const char* kDefaultUrl = "wss://connect.takaro.io/";

enum class Source { Env, File, Default };
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

// Per field: a non-empty environment variable, else the file, else the default (the Takaro URL,
// no token, no identity, DefaultServerName(identity)). `getenv` is injectable for tests.
Resolution Resolve(const Loaded& file, const std::function<const char*(const char*)>& env = ::getenv);

// `text` with KEY's line set to KEY=value (the first matching line, else appended), keeping every
// other line and the line-ending style.
std::string SetKey(const std::string& text, const std::string& key, const std::string& value);

// Writes a unique temp file and renames it over `path`, so a reader never sees half a file; keeps
// the old file's mode and, where the process may, its owner. With `expected`, gives up (EAGAIN)
// when the file no longer holds that text; with `create`, gives up (EEXIST) when it exists.
bool WriteAtomic(const std::string& path, const std::string& text, unsigned mode = 0600,
                 const std::string* expected = nullptr, bool create = false);

}  // namespace ConfigFile
