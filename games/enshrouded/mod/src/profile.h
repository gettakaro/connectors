// Per-build game data (gamedata/enshrouded-gamedata.json): function RVAs/patterns/job names, call sites and struct
// layout for one game build. A game update needs a new JSON section, not a new plugin build.
#pragma once
#include <cstdint>
#include <string>

namespace profile {

// Loads takaro/enshrouded-gamedata.json (written from the embedded copy when missing) and selects the section for
// `gameBuild`. Call after scan::Init().
void Load(const std::string& gameBuild);
// True when a section for the running build was found: callers use it instead of the built-in heuristics.
bool Active();
std::string Source();  // "file <path>" / "embedded" + the selected build, for diagnostics

// Resolves a named function: "rva" (+ "verify" bytes), else a unique "pattern", else "job" (ECS job table).
// 0 when the entry is missing or fails its check; `how` says which.
uint32_t Fn(const char* name, std::string& how);
// A call-site return address RVA from "callsites" (0 when missing or not preceded by an E8 call).
uint32_t CallsiteRet(const char* name);
// Layout value (hex or decimal string, or a number); `def` when missing.
uint64_t L(const char* name, uint64_t def);
std::string LStr(const char* name, const std::string& def);

// RVA of the function registered for an ECS job name ({char* name, u64 len, fn*} table entries); 0 if not unique.
uint32_t JobFunction(const char* jobName);

}  // namespace profile
