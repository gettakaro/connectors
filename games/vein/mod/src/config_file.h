// Optional key=value config file for panels that cannot set environment variables per process
// (AMP, Pterodactyl, LinuxGSM). Keys are the documented environment variable names; a real,
// non-empty environment variable always wins over the file.
#pragma once

#include <map>
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
// around the value are stripped. Only TAKARO_* and VEIN_* keys are accepted.
Parsed Parse(const std::string& text);

// TAKARO_CONFIG_FILE when set, otherwise takaro.cfg next to libtakaro-vein.so.
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
const Loaded& Current();

// The setting `name`: a non-empty environment variable, else the file's value, else the
// environment value as is (nullptr when unset, "" when explicitly empty). Never calls setenv, so
// it is safe when the library is loaded after the game has started threads.
const char* Lookup(const Loaded& file, const char* name);
inline const char* Get(const char* name) { return Lookup(Current(), name); }

}  // namespace ConfigFile
