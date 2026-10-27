// takaro/enshrouded-gamedata.json on disk versus the copy embedded in the plugin (mod/gamedata/).
//
// Per game build section of the file:
//   - missing in the file                     -> the embedded section is added
//   - equal to a section some release shipped -> written by a plugin and never edited: replaced by the embedded one
//   - anything else                           -> edited by the owner: kept as it is; keys it lacks are added from the
//                                                embedded section (two levels deep: a whole function entry, callsite
//                                                or layout value at a time; the owner's value wins per key)
// Sections only the file has are kept. Broken JSON is left untouched and the embedded data is used.
// Pure except Apply(), so the host tests run all of it.
#pragma once
#include "common.h"

#include <functional>
#include <string>
#include <vector>

namespace gamedata_file {

// 16 hex digits: FNV-1a 64 of the section's canonical JSON (keys sorted, no whitespace, numbers as written).
std::string SectionHash(const JsonValue& section);
// Sections earlier releases shipped, so a file they wrote is recognised as unedited.
bool KnownShipped(const std::string& build, const std::string& hash);
// Every shipped section's hash, for the host test that keeps the table complete.
struct Shipped {
    const char* build;
    const char* hash;
};
const std::vector<Shipped>& ShippedTable();

// The file as the plugin writes it: two-space indent, function entries on one line.
std::string Pretty(const JsonValue& root);

struct Result {
    JsonValue root;            // the data to use (file + upgrades, or embedded)
    bool fromFile = false;     // false: embedded (no file, or the file is broken)
    bool write = false;        // the file must be (re)written with `text`
    bool fresh = false;        // no file existed: `text` is the embedded file verbatim
    std::string text;
    std::vector<std::string> log;  // one line per outcome, without the "gamedata: " prefix
};
Result Reconcile(bool fileExists, const std::string& fileText, const std::string& embeddedText);

// Reads `path`, reconciles, and writes the result back: a missing file is created; a changed one is first copied to
// `<path>.bak-<8 hex of the old content>` (kept if it already exists), then rewritten in place so it keeps its owner,
// mode and ACL. Unchanged files are not touched. Never throws; every outcome goes to `log`.
Result Apply(const std::string& path, const std::string& embeddedText, const std::function<void(const std::string&)>& log);

}  // namespace gamedata_file
