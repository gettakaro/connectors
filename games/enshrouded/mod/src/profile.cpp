#include "profile.h"

#include "common.h"
#include "derive.h"
#include "scan.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace profile {
namespace {

const char kEmbedded[] =
#include "gamedata_default.inc"
    ;

JsonValue g_root;
const JsonValue* g_build = nullptr;
std::string g_source = "none";

bool ReadFile(const std::string& path, std::string& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    char buf[8192];
    size_t n;
    out.clear();
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    fclose(f);
    return true;
}

bool WriteFile(const std::string& path, const std::string& text) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
    fclose(f);
    return ok;
}

uint64_t ToU64(const JsonValue* v, uint64_t def) {
    if (!v) return def;
    if (v->type == JsonValue::Number) return (uint64_t)strtoull(v->str.c_str(), nullptr, 10);
    if (v->type == JsonValue::String && !v->str.empty()) return (uint64_t)strtoull(v->str.c_str(), nullptr, 0);
    return def;
}

const JsonValue* Section(const char* key) { return g_build ? g_build->get(key) : nullptr; }

bool SelectBuild(const JsonValue& root, const std::string& build) {
    const JsonValue* builds = root.get("builds");
    if (!builds || builds->type != JsonValue::Object) return false;
    const JsonValue* b = builds->get(build);
    if (!b || b->type != JsonValue::Object) return false;
    g_build = b;
    return true;
}

void Serialize(const JsonValue& v, int depth, std::string& out) {
    std::string pad(depth * 2, ' ');
    switch (v.type) {
        case JsonValue::Object: {
            // a function entry ({rva, verify}) stays on one line, like the hand-written file
            bool flat = depth >= 4;
            out += "{";
            for (size_t i = 0; i < v.obj.size(); i++) {
                out += i ? "," : "";
                out += flat ? " " : "\n" + pad + "  ";
                out += JsonStr(v.obj[i].first) + ": ";
                Serialize(v.obj[i].second, depth + 1, out);
            }
            out += v.obj.empty() ? "}" : (flat ? " }" : "\n" + pad + "}");
            break;
        }
        case JsonValue::Array:
            out += "[";
            for (size_t i = 0; i < v.arr.size(); i++) {
                out += i ? ", " : "";
                Serialize(v.arr[i], depth + 1, out);
            }
            out += "]";
            break;
        case JsonValue::String: out += JsonStr(v.str); break;
        case JsonValue::Number: out += v.str; break;
        case JsonValue::Bool: out += v.b ? "true" : "false"; break;
        default: out += "null";
    }
}

JsonValue Str(const std::string& s) {
    JsonValue v;
    v.type = JsonValue::String;
    v.str = s;
    return v;
}

// The layout of the newest build in the file: supplies the constants no anchor can derive.
const JsonValue* TemplateLayout(const JsonValue& root) {
    const JsonValue* builds = root.get("builds");
    if (!builds || builds->type != JsonValue::Object) return nullptr;
    const JsonValue* best = nullptr;
    uint64_t bestN = 0;
    for (auto& kv : builds->obj) {
        uint64_t n = strtoull(kv.first.c_str(), nullptr, 10);
        const JsonValue* l = kv.second.get("layout");
        if (!l || l->type != JsonValue::Object || n < bestN) continue;
        best = l;
        bestN = n;
    }
    return best;
}

// Derives a section for `gameBuild`, stores it in `root` and writes `root` to `path`. Returns the file text.
bool DeriveInto(JsonValue& root, const std::string& gameBuild, const std::string& path, std::string& written) {
    derive::Result r = derive::Derive(TemplateLayout(root));
    if (!r.ok) {
        PluginLog("gamedata: deriving build %s failed: %s", gameBuild.c_str(), r.error.c_str());
        return false;
    }
    SYSTEMTIME t;
    GetSystemTime(&t);
    char note[160];
    snprintf(note, sizeof note, "derived automatically by plugin %s on %04u-%02u-%02u (%d values from anchors)",
             TAKARO_PLUGIN_VERSION, t.wYear, t.wMonth, t.wDay, r.derivedValues);
    JsonValue section;
    section.type = JsonValue::Object;
    section.obj = {{"note", Str(note)}, {"functions", r.functions}, {"callsites", r.callsites}, {"layout", r.layout}};
    if (root.type != JsonValue::Object) root.type = JsonValue::Object;
    JsonValue* builds = nullptr;
    for (auto& kv : root.obj)
        if (kv.first == "builds") builds = &kv.second;
    if (!builds) {
        root.obj.push_back({"builds", JsonValue{}});
        builds = &root.obj.back().second;
    }
    builds->type = JsonValue::Object;
    bool replaced = false;
    for (auto& kv : builds->obj)
        if (kv.first == gameBuild) kv.second = section, replaced = true;
    if (!replaced) builds->obj.push_back({gameBuild, section});
    written.clear();
    Serialize(root, 0, written);
    written += "\n";
    if (!WriteFile(path, written)) {
        PluginLog("gamedata: cannot write derived section to %s", path.c_str());
        return false;
    }
    PluginLog("gamedata: derived build %s (%d values) and saved it to %s", gameBuild.c_str(), r.derivedValues,
              path.c_str());
    return true;
}

}  // namespace

void Load(const std::string& gameBuild) {
    std::string dir = PluginBaseDir() + "\\takaro";
    std::string path = dir + "\\enshrouded-gamedata.json";
    std::string text;
    CreateDirectoryA(dir.c_str(), nullptr);
    JsonValue embedded, fileRoot;
    bool embeddedOk = JsonParse(kEmbedded, embedded);
    bool fileOk = false;
    if (ReadFile(path, text)) {
        std::string body = text.rfind("\xEF\xBB\xBF", 0) == 0 ? text.substr(3) : text;
        fileOk = JsonParse(body, fileRoot) && fileRoot.type == JsonValue::Object;
        if (fileOk && (g_root = fileRoot, SelectBuild(g_root, gameBuild))) {
            g_source = "file " + path;
        } else if (embeddedOk && (g_root = embedded, SelectBuild(g_root, gameBuild))) {
            g_source = "embedded (file " + path + " has no valid section for this build)";
        }
    } else {
        if (WriteFile(path, kEmbedded)) PluginLog("gamedata: wrote default %s", path.c_str());
        fileOk = embeddedOk && (fileRoot = embedded, true);
        if (embeddedOk && (g_root = embedded, SelectBuild(g_root, gameBuild))) g_source = "embedded";
    }
    // A build without a section: derive one from the running exe, save it, then load the saved file back, so the
    // section in use is exactly what is on disk. takaro/gamedata-derive-test forces this for a known build and saves
    // to enshrouded-gamedata-derived.json instead, leaving the main file as it is.
    bool forceDerive = GetFileAttributesA((dir + "\\gamedata-derive-test").c_str()) != INVALID_FILE_ATTRIBUTES;
    if (!g_build || forceDerive) {
        std::string target = forceDerive ? dir + "\\enshrouded-gamedata-derived.json" : path;
        // keep the sections already in the file; a file that does not parse is rebuilt from the embedded copy
        JsonValue work = fileOk ? fileRoot : embedded;
        std::string written;
        JsonValue reread;
        const JsonValue* builds = nullptr;
        if (DeriveInto(work, gameBuild, target, written) && JsonParse(written, reread) &&
            (builds = reread.get("builds")) && builds->get(gameBuild)) {
            g_build = nullptr;
            g_root = std::move(reread);
            if (SelectBuild(g_root, gameBuild)) g_source = "derived, file " + target;
        }
    }
    if (g_build) {
        const JsonValue* note = g_build->get("note");
        PluginLog("gamedata: build %s from %s (%s)", gameBuild.c_str(), g_source.c_str(),
                  note && note->type == JsonValue::String ? note->str.c_str() : "");
    } else {
        g_source = "none";
        PluginLog("gamedata: no section for build %s; using built-in signature heuristics", gameBuild.c_str());
    }
}

bool Active() { return g_build != nullptr; }
std::string Source() { return g_source; }

uint32_t JobFunction(const char* jobName) {
    uint32_t s = scan::FindCString(jobName);
    if (!s) return 0;
    uint32_t found = 0;
    int hits = 0;
    for (uint32_t ref : scan::FindPointerRefs(s, 16)) {
        // {char* name, u64 nameLen, fn*}: the length must match, the function must be code
        if (scan::ReadU64(ref + 8) != strlen(jobName)) continue;
        uint64_t fn = scan::ReadU64(ref + 16);
        if (fn < scan::PointerBase()) continue;
        uint32_t rva = (uint32_t)(fn - scan::PointerBase());
        if (!scan::InText(rva)) continue;
        if (found != rva) hits++;
        found = rva;
    }
    return hits == 1 ? found : 0;
}

uint32_t Fn(const char* name, std::string& how) {
    const JsonValue* fns = Section("functions");
    const JsonValue* f = fns ? fns->get(name) : nullptr;
    if (!f || f->type != JsonValue::Object) {
        how = std::string("gamedata has no function '") + name + "'";
        return 0;
    }
    const JsonValue* verify = f->get("verify");
    std::string vpat = verify && verify->type == JsonValue::String ? verify->str : "";
    auto check = [&](uint32_t rva) { return rva && scan::InText(rva) && (vpat.empty() || scan::MatchAt(rva, vpat.c_str())); };
    uint32_t rva = (uint32_t)ToU64(f->get("rva"), 0);
    if (rva) {
        if (check(rva)) {
            how = std::string("gamedata rva") + (vpat.empty() ? "" : " + verify");
            return rva;
        }
        how = "gamedata rva failed its verify bytes";
    }
    const JsonValue* pat = f->get("pattern");
    if (pat && pat->type == JsonValue::String) {
        auto hits = scan::FindPattern(pat->str.c_str(), 3);
        if (hits.size() == 1 && check(hits[0])) {
            how = "gamedata pattern";
            return hits[0];
        }
        how += (how.empty() ? "" : "; ") + std::string("gamedata pattern hits=") + std::to_string(hits.size());
    }
    const JsonValue* job = f->get("job");
    if (job && job->type == JsonValue::String) {
        uint32_t j = JobFunction(job->str.c_str());
        if (check(j)) {
            how = "gamedata job '" + job->str + "'";
            return j;
        }
        how += (how.empty() ? "" : "; ") + std::string("job '") + job->str + "' unresolved";
    }
    if (how.empty()) how = std::string("gamedata function '") + name + "' has no rva/pattern/job";
    return 0;
}

uint32_t CallsiteRet(const char* name) {
    const JsonValue* cs = Section("callsites");
    uint32_t rva = (uint32_t)ToU64(cs ? cs->get(name) : nullptr, 0);
    if (rva < 5 || !scan::InText(rva - 5) || !scan::MatchAt(rva - 5, "E8")) return 0;
    return rva;
}

uint64_t L(const char* name, uint64_t def) {
    const JsonValue* l = Section("layout");
    return ToU64(l ? l->get(name) : nullptr, def);
}

std::string LStr(const char* name, const std::string& def) {
    const JsonValue* l = Section("layout");
    const JsonValue* v = l ? l->get(name) : nullptr;
    return v && v->type == JsonValue::String ? v->str : def;
}

}  // namespace profile
