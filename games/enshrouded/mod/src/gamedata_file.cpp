#include "gamedata_file.h"

#include "native/fileio.h"

#include <algorithm>
#include <cstdio>

namespace gamedata_file {
namespace {

// Every builds.<n> section any plugin build has embedded (mod/gamedata/enshrouded-gamedata.json in git history).
// When a section changes, add the new section's hash here; the host test fails until it is.
const std::vector<Shipped> kShipped = {
    {"1267041", "e73eae51ada94f39"},  // 0.8.0, 0.8.1
    {"1267041", "cdf020016856bb4f"},  // from game build 1273043 support on
    {"1273043", "24cb5fc9eb58c422"},  // game build 1273043 support, before character levels
    {"1273043", "5259f30449a8e2ee"},  // character levels (attributes* layout)
};

void Canonical(const JsonValue& v, std::string& out) {
    switch (v.type) {
        case JsonValue::Null: out += "null"; return;
        case JsonValue::Bool: out += v.b ? "true" : "false"; return;
        case JsonValue::Number: out += v.str; return;
        case JsonValue::String: out += JsonStr(v.str); return;
        case JsonValue::Array:
            out += '[';
            for (size_t i = 0; i < v.arr.size(); i++) {
                if (i) out += ',';
                Canonical(v.arr[i], out);
            }
            out += ']';
            return;
        case JsonValue::Object: {
            std::vector<const std::pair<std::string, JsonValue>*> kv;
            for (auto& p : v.obj) kv.push_back(&p);
            std::stable_sort(kv.begin(), kv.end(), [](auto* a, auto* b) { return a->first < b->first; });
            out += '{';
            for (size_t i = 0; i < kv.size(); i++) {
                if (i) out += ',';
                out += JsonStr(kv[i]->first) + ":";
                Canonical(kv[i]->second, out);
            }
            out += '}';
            return;
        }
    }
}

uint64_t Fnv1a64(const std::string& s) {
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    return h;
}

std::string Hex(uint64_t v, int digits) {
    char b[17];
    snprintf(b, sizeof b, "%016llx", (unsigned long long)v);
    return std::string(b + 16 - digits);
}

JsonValue* Find(JsonValue& obj, const std::string& key) {
    for (auto& p : obj.obj)
        if (p.first == key) return &p.second;
    return nullptr;
}

// Adds what `edited` lacks from `shipped`: whole groups, and whole entries inside a group both have as objects.
int MergeMissing(JsonValue& edited, const JsonValue& shipped, std::vector<std::string>& names) {
    int n = 0;
    for (auto& g : shipped.obj) {
        JsonValue* mine = Find(edited, g.first);
        if (!mine) {
            edited.obj.push_back(g);
            names.push_back(g.first);
            n++;
            continue;
        }
        if (mine->type != JsonValue::Object || g.second.type != JsonValue::Object) continue;
        for (auto& e : g.second.obj) {
            if (Find(*mine, e.first)) continue;
            mine->obj.push_back(e);
            names.push_back(g.first + "." + e.first);
            n++;
        }
    }
    return n;
}

std::string JoinNames(const std::vector<std::string>& names) {
    std::string s;
    for (size_t i = 0; i < names.size() && i < 12; i++) s += (i ? ", " : "") + names[i];
    if (names.size() > 12) s += ", ... (" + std::to_string(names.size() - 12) + " more)";
    return s;
}

bool IsContainer(const JsonValue& v) { return v.type == JsonValue::Object || v.type == JsonValue::Array; }

void Inline(const JsonValue& v, std::string& out) {
    if (v.type == JsonValue::Object) {
        if (v.obj.empty()) {
            out += "{}";
            return;
        }
        out += "{ ";
        for (size_t i = 0; i < v.obj.size(); i++) {
            out += (i ? ", " : "") + JsonStr(v.obj[i].first) + ": ";
            Inline(v.obj[i].second, out);
        }
        out += " }";
    } else if (v.type == JsonValue::Array) {
        out += "[";
        for (size_t i = 0; i < v.arr.size(); i++) {
            out += i ? ", " : "";
            Inline(v.arr[i], out);
        }
        out += "]";
    } else {
        Canonical(v, out);
    }
}

// Objects down to a group (root > builds > section > group) one key per line; deeper values on one line, keys
// aligned when they are objects (the function table).
void PrettyAt(const JsonValue& v, int depth, std::string& out) {
    if (v.type != JsonValue::Object || depth >= 4 || v.obj.empty()) {
        Inline(v, out);
        return;
    }
    size_t width = 0;
    bool align = depth == 3;
    for (auto& p : v.obj) {
        if (!IsContainer(p.second)) align = false;
        width = std::max(width, JsonStr(p.first).size() + 1);
    }
    std::string pad((size_t)(depth + 1) * 2, ' ');
    out += "{\n";
    for (size_t i = 0; i < v.obj.size(); i++) {
        std::string key = JsonStr(v.obj[i].first) + ":";
        out += pad + key + std::string(align ? width - key.size() + 1 : 1, ' ');
        PrettyAt(v.obj[i].second, depth + 1, out);
        out += i + 1 < v.obj.size() ? ",\n" : "\n";
    }
    out += std::string((size_t)depth * 2, ' ') + "}";
}

std::string StripBom(const std::string& t) { return t.rfind("\xEF\xBB\xBF", 0) == 0 ? t.substr(3) : t; }

}  // namespace

std::string SectionHash(const JsonValue& section) {
    std::string c;
    Canonical(section, c);
    return Hex(Fnv1a64(c), 16);
}

const std::vector<Shipped>& ShippedTable() { return kShipped; }

bool KnownShipped(const std::string& build, const std::string& hash) {
    for (auto& s : kShipped)
        if (build == s.build && hash == s.hash) return true;
    return false;
}

std::string Pretty(const JsonValue& root) {
    std::string out;
    PrettyAt(root, 0, out);
    return out + "\n";
}

Result Reconcile(bool fileExists, const std::string& fileText, const std::string& embeddedText) {
    Result r;
    JsonValue embedded;
    bool embeddedOk = JsonParse(embeddedText, embedded) && embedded.type == JsonValue::Object;
    const JsonValue* eb = embeddedOk ? embedded.get("builds") : nullptr;
    if (!eb || eb->type != JsonValue::Object) {
        embeddedOk = false;
        r.log.push_back("the embedded game data does not parse; this plugin build is broken");
    }
    if (!fileExists) {
        r.root = embedded;
        r.fresh = r.write = true;
        r.text = embeddedText;
        return r;
    }
    JsonValue file;
    JsonValue* fb = nullptr;
    if (JsonParse(StripBom(fileText), file) && file.type == JsonValue::Object) fb = Find(file, "builds");
    if (!fb || fb->type != JsonValue::Object) {
        r.root = embedded;
        r.log.push_back(
            "the file is not valid JSON with a \"builds\" object; left it untouched and using this plugin's embedded "
            "game data. Fix the file, or delete it to get the current defaults.");
        return r;
    }
    r.fromFile = true;
    if (!embeddedOk) {
        r.root = std::move(file);
        return r;
    }
    bool changed = false;
    for (auto& e : eb->obj) {
        const std::string& build = e.first;
        JsonValue* mine = Find(*fb, build);
        if (!mine) {
            fb->obj.push_back(e);
            changed = true;
            r.log.push_back("build " + build + ": added this plugin's section");
            continue;
        }
        std::string have = SectionHash(*mine), want = SectionHash(e.second);
        if (have == want) continue;
        if (mine->type != JsonValue::Object) {
            *mine = e.second;
            changed = true;
            r.log.push_back("build " + build + ": the file's section was not a JSON object; replaced it with this plugin's");
            continue;
        }
        if (KnownShipped(build, have)) {
            *mine = e.second;
            changed = true;
            r.log.push_back("build " + build + ": upgraded the section an earlier plugin wrote (" + have + ") to this plugin's (" +
                            want + ")");
            continue;
        }
        std::vector<std::string> names;
        int n = MergeMissing(*mine, e.second, names);
        if (n) {
            changed = true;
            r.log.push_back("build " + build + ": kept your edited section, merged " + std::to_string(n) +
                            " missing key(s) from this plugin: " + JoinNames(names));
        } else {
            r.log.push_back("build " + build + ": kept your edited section (it differs from this plugin's; nothing to merge)");
        }
    }
    for (auto& f : fb->obj)
        if (!eb->get(f.first)) r.log.push_back("build " + f.first + ": kept the file's own section (this plugin has none)");
    if (changed) {
        r.write = true;
        r.text = Pretty(file);
    }
    r.root = std::move(file);
    return r;
}

Result Apply(const std::string& path, const std::string& embeddedText, const std::function<void(const std::string&)>& log) {
    auto say = [&](const std::string& s) {
        if (log) log("gamedata: " + s);
    };
    std::string text, err;
    bool exists = false;
    if (!native::ReadWholeFile(path, text, exists, err)) {
        say("cannot read " + path + " (" + err + "); using this plugin's embedded game data");
        Result r = Reconcile(false, "", embeddedText);
        r.write = false;
        r.fromFile = false;
        return r;
    }
    Result r = Reconcile(exists, text, embeddedText);
    if (!exists) {
        for (auto& l : r.log) say(l);
        if (native::EnsureDirectory(native::DirName(path), err) && native::AtomicWriteFile(path, r.text, err))
            say("wrote default " + path);
        else
            say("could not write " + path + " (" + err + ")");
        return r;
    }
    for (auto& l : r.log) say(path + ": " + l);
    if (!r.write) return r;
    std::string bak = path + ".bak-" + Hex(Fnv1a64(text), 8);
    bool bakExists = false;
    std::string ignore;
    native::ReadWholeFile(bak, ignore, bakExists, err);
    if (!bakExists && !native::AtomicWriteFile(bak, text, err)) {
        say("not saving the upgrade: could not back up " + path + " to " + bak + " (" + err +
            "); the upgraded data is used for this run");
        return r;
    }
    if (native::RewriteKeepingAccess(path, text, r.text, err))
        say("saved " + path + " (previous content in " + bak + ")");
    else
        say("could not save " + path + " (" + err + "); the upgraded data is used for this run, the next start tries again");
    return r;
}

}  // namespace gamedata_file
