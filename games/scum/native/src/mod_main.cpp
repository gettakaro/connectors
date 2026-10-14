// UE4SS C++ mod entry for TakaroSCUM: holds the Takaro WebSocket (Bridge + WinHTTP transport) and exposes it
// to the Lua mod of the same name (Mods/TakaroSCUM/Scripts) as global functions. No game symbols here.
#include "bridge.h"
#include "log.h"
#include "ue4ss_abi.hpp"
#include "version.h"

#include <mutex>
#include <set>
#include <string>

using RC::LuaMadeSimple::Lua;

namespace {

std::string Utf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

// <...>/Mods/TakaroSCUM (the folder above dlls/main.dll), UTF-8, no trailing separator.
std::string ModDir() {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&ModDir), &self);
    wchar_t buf[MAX_PATH * 2] = {0};
    DWORD n = GetModuleFileNameW(self, buf, (DWORD)(sizeof buf / sizeof buf[0]));
    std::wstring p(buf, n);
    for (int i = 0; i < 2; i++) {
        size_t s = p.find_last_of(L"\\/");
        if (s == std::wstring::npos) break;
        p.resize(s);
    }
    return Utf8(p);
}

// Arguments are read in order: every UE4SS get_* pops index 1, so the next argument moves to index 1.
struct Args {
    const Lua& L;
    bool Has() const { return L.get_stack_size() >= 1; }
    std::string Str(bool* ok = nullptr) {
        if (Has() && L.is_string(1)) {
            std::string_view v = L.get_string(1);
            if (ok) *ok = true;
            return std::string(v.data(), v.size());  // copy before anything can collect the popped string
        }
        if (Has()) L.discard_value(1);
        if (ok) *ok = false;
        return std::string();
    }
    int64_t Int(int64_t def) {
        if (!Has()) return def;
        if (L.is_integer(1)) return L.get_integer(1);
        if (L.is_number(1)) return (int64_t)L.get_number(1);
        L.discard_value(1);
        return def;
    }
};

const char* StatusName(native::QueueStatus s) { return native::QueueStatusName(s); }

// takaro_connect(url [, caFile]) -> bool
int LuaConnect(const Lua& L) {
    try {
        Args a{L};
        std::string url = a.Str(), ca = a.Str();
        L.set_bool(native::Bridge::Get().Connect(url, ca));
    } catch (...) {
        L.set_bool(false);
    }
    return 1;
}

// takaro_disconnect()
int LuaDisconnect(const Lua&) {
    try {
        native::Bridge::Get().Disconnect();
    } catch (...) {
    }
    return 0;
}

// takaro_send(text, kind, epoch) -> status, outboxId
//   status: "accepted" | "disconnected" | "stale-epoch" | "full" | "too-large"; outboxId: events only, else 0
int LuaSend(const Lua& L) {
    try {
        Args a{L};
        bool ok = false;
        std::string text = a.Str(&ok);
        std::string kind = a.Str();
        int64_t epoch = a.Int(0);
        if (!ok) {
            L.set_string("too-large");
            L.set_integer(0);
            return 2;
        }
        uint64_t outbox = 0;
        L.set_string(StatusName(native::Bridge::Get().Send(text, kind, (uint64_t)(epoch < 0 ? 0 : epoch), &outbox)));
        L.set_integer((int64_t)outbox);
    } catch (...) {
        L.set_string("disconnected");
        L.set_integer(0);
    }
    return 2;
}

// takaro_poll() -> kind, epoch, text   (nil when the inbox is empty)
int LuaPoll(const Lua& L) {
    try {
        native::InboxItem it;
        if (!native::Bridge::Get().Poll(it)) {
            L.set_nil();
            return 1;
        }
        L.set_string(it.kind);
        L.set_integer((int64_t)it.epoch);
        L.set_string(it.text);
        return 3;
    } catch (...) {
        L.set_nil();
        return 1;
    }
}

// takaro_close(epoch, reason)
int LuaClose(const Lua& L) {
    try {
        Args a{L};
        int64_t epoch = a.Int(0);
        std::string reason = a.Str();
        native::Bridge::Get().Close((uint64_t)epoch, reason);
    } catch (...) {
    }
    return 0;
}

// takaro_identified(epoch): identify succeeded, reset the reconnect backoff
int LuaIdentified(const Lua& L) {
    try {
        Args a{L};
        native::Bridge::Get().MarkIdentified((uint64_t)a.Int(0));
    } catch (...) {
    }
    return 0;
}

// takaro_status() -> JSON text
int LuaStatus(const Lua& L) {
    try {
        L.set_string(native::Bridge::Get().StatusJson());
    } catch (...) {
        L.set_string("{}");
    }
    return 1;
}

// takaro_log(line): append to <mod>/takaro.log (the Lua side redacts before calling)
int LuaLog(const Lua& L) {
    try {
        Args a{L};
        LogLine(a.Str());
    } catch (...) {
    }
    return 0;
}

// takaro_now_ms() -> monotonic milliseconds
int LuaNowMs(const Lua& L) {
    L.set_integer((int64_t)GetTickCount64());
    return 1;
}

// takaro_mod_dir() -> UTF-8 path of the mod folder
int LuaModDir(const Lua& L) {
    try {
        L.set_string(ModDir());
    } catch (...) {
        L.set_nil();
    }
    return 1;
}

// UTF-8 -> UTF-16 for Win32 paths.
std::wstring Wide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

// takaro_list_dir(path) -> "name\nname\n..." (regular files only) or nil when the folder cannot be read.
// FindFirstFileW costs microseconds for a log folder; UE4SS IterateGameDirectories walks the whole game tree (~300 ms).
int LuaListDir(const Lua& L) {
    try {
        Args a{L};
        std::wstring pattern = Wide(a.Str()) + L"\\*";
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
        if (h == INVALID_HANDLE_VALUE) {
            L.set_nil();
            return 1;
        }
        std::string out;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            out += Utf8(fd.cFileName);
            out += '\n';
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        L.set_string(out);
    } catch (...) {
        L.set_nil();
    }
    return 1;
}

// takaro_now_us() -> monotonic microseconds (QueryPerformanceCounter), for game-thread budget measurements
int LuaNowUs(const Lua& L) {
    static LARGE_INTEGER freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    L.set_integer((int64_t)(c.QuadPart / freq.QuadPart * 1000000 + (c.QuadPart % freq.QuadPart) * 1000000 / freq.QuadPart));
    return 1;
}

std::mutex g_regMu;
std::set<const void*> g_registered;

void RegisterLuaFunctions(Lua& lua) {
    {
        std::lock_guard<std::mutex> g(g_regMu);
        if (!g_registered.insert(&lua).second) return;  // both dispatch paths fire; register once per instance
    }
    struct Fn {
        const char* name;
        Lua::LuaFunction fn;
    };
    static const Fn fns[] = {
        {"takaro_connect", &LuaConnect},   {"takaro_disconnect", &LuaDisconnect},
        {"takaro_send", &LuaSend},         {"takaro_poll", &LuaPoll},
        {"takaro_close", &LuaClose},       {"takaro_identified", &LuaIdentified},
        {"takaro_status", &LuaStatus},     {"takaro_log", &LuaLog},
        {"takaro_now_ms", &LuaNowMs},      {"takaro_mod_dir", &LuaModDir},
        {"takaro_list_dir", &LuaListDir},  {"takaro_now_us", &LuaNowUs},
    };
    for (const Fn& f : fns) lua.register_function(f.name, f.fn);
    PluginLog("native: registered %zu Lua functions (TakaroSCUM native %s)", sizeof fns / sizeof fns[0],
              TAKARO_PLUGIN_VERSION);
}

void OnLuaStartSelf(RC::CppUserModBase*, Lua& lua, Lua&, Lua&, Lua*) { RegisterLuaFunctions(lua); }

class TakaroScumMod final : public RC::CppUserModBase {
public:
    TakaroScumMod() {
        ModName = L"TakaroSCUM";
        ModVersion = TAKARO_PLUGIN_VERSION_W;
        ModDescription = L"Takaro connector transport (WinHTTP WebSocket) for the TakaroSCUM Lua mod";
        ModAuthors = L"Takaro";
        LogSetFile(ModDir() + "\\takaro.log");
        PluginLog("native: TakaroSCUM native %s loaded", TAKARO_PLUGIN_VERSION);
        register_on_lua_start(&OnLuaStartSelf);
    }
    ~TakaroScumMod() override {
        native::Bridge::Get().Disconnect();
        PluginLog("native: TakaroSCUM native unloaded");
    }

    auto on_update() -> void override {}
    auto on_unreal_init() -> void override {}
    auto on_ui_init() -> void override {}
    auto on_program_start() -> void override {}
    auto on_lua_start(RC::StringViewType, Lua&, Lua&, Lua&, std::vector<Lua*>&) -> void override {}
    auto on_lua_start(Lua&, Lua&, Lua&, std::vector<Lua*>&) -> void override {}
    auto on_lua_stop(RC::StringViewType, Lua&, Lua&, Lua&, std::vector<Lua*>&) -> void override {}
    auto on_lua_stop(Lua&, Lua&, Lua&, std::vector<Lua*>&) -> void override {}
    auto on_dll_load(RC::StringViewType) -> void override {}
    auto render_tab() -> void override {}
    auto on_lua_start(RC::StringViewType, Lua&, Lua&, Lua&, Lua*) -> void override {}
    // Legacy virtual path for the Lua mod of the same name; the dispatcher path above normally wins.
    auto on_lua_start(Lua& lua, Lua&, Lua&, Lua*) -> void override { RegisterLuaFunctions(lua); }
    auto on_lua_stop(RC::StringViewType, Lua&, Lua&, Lua&, Lua*) -> void override {}
    auto on_lua_stop(Lua& lua, Lua&, Lua&, Lua*) -> void override {
        // The Lua mod is stopping (hot reload or shutdown): forget its instance and drop the socket off-thread,
        // so the restarted Lua side opens a fresh connection and identifies again.
        {
            std::lock_guard<std::mutex> g(g_regMu);
            g_registered.erase(&lua);
        }
        native::Bridge::Get().DisconnectAsync();
    }
    auto on_cpp_mods_loaded() -> void override {}
};

}  // namespace

extern "C" __declspec(dllexport) RC::CppUserModBase* start_mod() { return new TakaroScumMod(); }

extern "C" __declspec(dllexport) void uninstall_mod(RC::CppUserModBase* mod) { delete mod; }
