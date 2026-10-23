// Offline check of the gamedata derivation: maps enshrouded_server.exe as an image (nothing runs) and prints the
// derived section as JSON. Build with zig for x86_64-windows-gnu and run on Windows (or from WSL):
//   derive_test.exe <path\to\enshrouded_server.exe> [enshrouded-gamedata.json]
// With the gamedata file, the newest build's layout supplies the constants no anchor can derive, as in the plugin.
#include "common.h"
#include "derive.h"
#include "scan.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

HMODULE g_selfModule = nullptr;

static void Dump(const JsonValue& v) {
    if (v.type == JsonValue::Object) {
        printf("{");
        for (size_t i = 0; i < v.obj.size(); i++) {
            printf("%s%s:", i ? "," : "", JsonStr(v.obj[i].first).c_str());
            Dump(v.obj[i].second);
        }
        printf("}");
    } else if (v.type == JsonValue::String) {
        printf("%s", JsonStr(v.str).c_str());
    } else {
        printf("%s", v.str.c_str());
    }
}

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    HMODULE m = LoadLibraryExA(argv[1], nullptr, LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!m) {
        fprintf(stderr, "cannot map %s (%lu)\n", argv[1], GetLastError());
        return 2;
    }
    uintptr_t base = (uintptr_t)m & ~(uintptr_t)3;  // image-resource handles carry flag bits
    if (!scan::InitAt(base)) return 3;
    JsonValue root;
    const JsonValue* tmpl = nullptr;
    if (argc > 2) {
        std::ifstream f(argv[2], std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        if (!JsonParse(ss.str(), root)) {
            fprintf(stderr, "cannot parse %s\n", argv[2]);
            return 2;
        }
        const JsonValue* builds = root.get("builds");
        uint64_t best = 0;
        for (auto& kv : builds ? builds->obj : root.obj) {
            uint64_t n = strtoull(kv.first.c_str(), nullptr, 10);
            if (builds && n >= best && kv.second.get("layout")) tmpl = kv.second.get("layout"), best = n;
        }
    }
    derive::Result r = derive::Derive(tmpl);
    if (!r.ok) {
        fprintf(stderr, "derive failed: %s\n", r.error.c_str());
        return 1;
    }
    printf("{\"functions\":");
    Dump(r.functions);
    printf(",\"callsites\":");
    Dump(r.callsites);
    printf(",\"layout\":");
    Dump(r.layout);
    printf("}\n");
    return 0;
}
