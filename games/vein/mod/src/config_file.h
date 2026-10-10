// Optional key=value config file for panels that cannot set environment variables per process
// (AMP, Pterodactyl, LinuxGSM). Keys are the documented environment variable names; a real,
// non-empty environment variable always wins over the file.
#pragma once

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

struct Applied {
    std::string path;
    bool found = false;
    size_t applied = 0;                  // keys copied into the environment
    std::vector<std::string> keptEnv;    // keys skipped because the environment already set them
    std::vector<std::string> warnings;
};

// Reads `path` and setenv()s every entry the environment does not already set (unset or empty).
// Must run while the process is still single-threaded (the library constructor).
Applied Apply(const std::string& path);

}  // namespace ConfigFile
