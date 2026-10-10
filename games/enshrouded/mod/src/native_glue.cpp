// Wires the game-independent native connector (src/native/) to this plugin: GameApi calls the plugin's
// contract router in-process, capabilities land in PluginState, and the transport is WinHTTP.
//
// takaro\plugin.json is re-read while the server runs (ConfigWatcher, on its own thread): a saved
// registration token connects without a restart, and a changed token, URL, identity or name reconnects
// at once. What the operator must fix is printed to the server console (stdout), where a panel shows it.
#include "common.h"
#include "native/bridge.h"
#include "native/config.h"
#include "native/config_file.h"
#include "native/fileio.h"
#include "native/json_util.h"
#include "native/persistence.h"
#include "native/transport_winhttp.h"
#include "plugin_api.h"
#include "state.h"

#include <bcrypt.h>

#include <atomic>
#include <mutex>
#include <thread>

namespace {

class PluginGameApi : public native::GameApi {
public:
    native::GameResponse Call(const std::string& method, const std::string& path, const std::string& body) override {
        PluginResponse r = PluginCall(method, path, body);
        return {r.status, r.body};
    }
    void SetCapability(const std::string& name, const std::string& status, const std::string& detail) override {
        PluginState::Get().SetCapability(name, status, detail);
    }
    std::string BaseDir() override { return PluginBaseDir(); }
};

std::string EnvUtf8(const char* name) {
    wchar_t wname[128];
    MultiByteToWideChar(CP_UTF8, 0, name, -1, wname, 128);
    DWORD n = GetEnvironmentVariableW(wname, nullptr, 0);
    if (!n) return "";
    std::wstring w(n, L'\0');
    n = GetEnvironmentVariableW(wname, &w[0], n);
    w.resize(n);
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], len, nullptr, nullptr);
    return s;
}

std::string NewIdentity() {
    unsigned char b[16] = {0};
    if (BCryptGenRandom(nullptr, b, sizeof b, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        // Not expected; time, counter and pid still make a collision within one domain unlikely.
        LARGE_INTEGER qpc;
        QueryPerformanceCounter(&qpc);
        unsigned long long seed = (unsigned long long)qpc.QuadPart ^ ((unsigned long long)GetCurrentProcessId() << 32) ^
                                  GetTickCount64();
        for (auto& c : b) {
            seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
            c = (unsigned char)(seed >> 56);
        }
    }
    return native::UuidFromBytes(b);
}

bool DirectoryExists(const std::string& path) {
    int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    std::wstring w((size_t)(n > 0 ? n : 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &w[0], n);
    DWORD a = GetFileAttributesW(w.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

// Wine maps Z: to the Linux root; a panel user knows the Linux path.
std::string DisplayPath(const std::string& p) {
    if (p.size() > 2 && (p[0] == 'Z' || p[0] == 'z') && p[1] == ':' && p[2] == '\\') {
        std::string out = p.substr(2);
        for (auto& c : out)
            if (c == '\\') c = '/';
        return out;
    }
    return p;
}

std::string HostServerName(const std::string& base) {
    std::string text, err;
    bool exists = false;
    if (!native::ReadWholeFile(native::JoinPath(base, "enshrouded_server.json"), text, exists, err) || !exists) return "";
    native::SettingsFile f = native::ParseSettingsFile(true, text);
    return f.String("name");
}

// One write per message, so engine output on the same stream never splits it.
void ConsoleWrite(const std::string& text) {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!h || h == INVALID_HANDLE_VALUE) return;
    DWORD put = 0;
    WriteFile(h, text.data(), (DWORD)text.size(), &put, nullptr);
}

void ConsoleLine(const std::string& line) {
    PluginLog("console: %s", line.c_str());
    ConsoleWrite("[Takaro] " + line + "\n");
}

void ConsoleBanner(const std::vector<std::string>& lines) {
    static const char rule[] = "*************************************************************************";
    std::string out = std::string("[Takaro] ") + rule + "\n";
    for (auto& l : lines) {
        PluginLog("console: %s", l.c_str());
        out += "[Takaro]   " + l + "\n";
    }
    out += std::string("[Takaro] ") + rule + "\n";
    ConsoleWrite(out);
}

PluginGameApi g_api;
std::atomic<native::Bridge*> g_bridge{nullptr};
SrwLock g_disabledLock;
std::string g_disabled = "{\"enabled\":false,\"reason\":\"starting\"}";

// Console state, shared by the watcher thread and the bridge callbacks.
std::mutex g_consoleMu;
std::string g_consoleKey;  // the problem (or "connected") last printed; reprinted only when it changes
native::LiveResolution g_live;
std::string g_userPath;

void ConsoleOnce(const std::string& key, const std::vector<std::string>& lines) {
    std::lock_guard<std::mutex> g(g_consoleMu);
    if (g_consoleKey == key) return;
    g_consoleKey = key;
    if (lines.size() == 1) ConsoleLine(lines[0]);
    else ConsoleBanner(lines);
}

void BannerBlocked(const native::LiveResolution& live, const std::string& blocker) {
    std::string path = DisplayPath(g_userPath);
    if (live.s.registrationToken.empty())
        ConsoleOnce("no-token", {"registrationToken not set, the server is not connected to Takaro.",
                                 "Paste the registration token from Takaro into " + path,
                                 "and save it. The connector connects within a few seconds, no restart needed."});
    else
        ConsoleOnce("blocked:" + blocker, {"Not connecting to Takaro: " + blocker + ".",
                                           "Fix \"url\" in " + path,
                                           "and save it. The connector reads it again within a few seconds."});
}

void OnIdentifyRejected(const std::string& summary, int status) {
    native::LiveResolution live;
    {
        std::lock_guard<std::mutex> g(g_consoleMu);
        live = g_live;
    }
    std::string path = DisplayPath(g_userPath);
    if (status == 409) {
        std::vector<std::string> lines = {
            "Takaro refused the server name \"" + live.s.serverName + "\": another game server in",
            "this Takaro domain already has it (" + summary + ")."};
        if (live.name == native::Source::Env) {
            lines.push_back("Change the TAKARO_SERVER_NAME environment variable and restart the server.");
        } else {
            lines.push_back("Set a different \"name\" in " + path);
            lines.push_back("and save it. The connector reconnects within a few seconds, no restart needed.");
        }
        ConsoleOnce("name-taken", lines);
        return;
    }
    if (live.registration == native::Source::Env)
        ConsoleOnce("refused", {"Takaro refused this server: " + summary + ".",
                                "Check the TAKARO_REGISTRATION_TOKEN environment variable; it wins over " + path,
                                "and a changed environment variable needs a server restart."});
    else
        ConsoleOnce("refused", {"Takaro refused this server: " + summary + ".",
                                "Check \"registrationToken\" in " + path,
                                "and save it. The connector reconnects within a few seconds, no restart needed."});
}

native::ConfigWatcher* g_watcher = nullptr;

void OnIdentified(const native::LiveSettings& accepted) {
    if (g_watcher) g_watcher->MarkAccepted(accepted);
    ConsoleOnce("connected", {"connected to Takaro as \"" + accepted.serverName +
                              "\"; the server shows as reachable in the Takaro dashboard"});
}

void SetDisabled(const std::string& reason, const native::NativeConfig& cfg) {
    Guard g(g_disabledLock);
    g_disabled = native::ObjBuilder()
                     .B("enabled", false)
                     .S("reason", reason)
                     .S("configFile", g_userPath)
                     .Raw("config", native::ConfigSummaryJson(cfg))
                     .Done();
}

native::Bridge* StartBridge(const std::string& base, const native::NativeConfig& cfg, const native::EnvFn& env) {
    if (cfg.legacyHttp)
        PluginLog("native: WARNING TAKARO_LEGACY_HTTP=1 and the direct connection are both on; do not run the legacy "
                  "sidecar with the same identity at the same time");
    // Never freed: the plugin lives as long as the server process.
    auto* store = new native::Store(native::ResolveStatePaths(base, env));
    native::WinHttpTransport::Options to;
    to.url = cfg.url;
    to.caFile = cfg.caFile;
    to.reconnectBaseMs = cfg.reconnectBaseMs;
    to.reconnectMaxMs = cfg.reconnectMaxMs;
    auto* transport = new native::WinHttpTransport(to);
    native::BridgeOptions bo;
    bo.config = cfg;
    bo.game = &g_api;
    bo.transport = transport;
    bo.store = store;
    bo.onIdentified = OnIdentified;
    bo.onIdentifyRejected = OnIdentifyRejected;
    auto* bridge = new native::Bridge(bo);
    PluginLog("native: starting direct Takaro connection to %s as '%s' (state %s)", cfg.url.c_str(),
              cfg.serverName.c_str(), store->Paths().dir.c_str());
    ConsoleLine("connecting to " + cfg.url + " as \"" + cfg.serverName + "\"");
    bridge->Start();
    g_bridge = bridge;
    return bridge;
}

void LogSources(const native::NativeConfig& cfg) {
    for (auto& s : cfg.sources) PluginLog("native: config %s from %s", s.first.c_str(), s.second.c_str());
}

// The watcher thread: re-reads plugin.json and applies what changed.
void WatchLoop(std::string base, native::NativeConfig cfg, native::EnvFn env) {
    for (;;) {
        Sleep(1000);
        bool changed = g_watcher->Poll((int64_t)GetTickCount64());
        for (auto& w : g_watcher->TakeConsoleWarnings()) ConsoleLine("WARNING: " + w);
        if (!changed) continue;
        const native::LiveResolution& live = g_watcher->Active();
        native::ApplyLive(cfg, live);
        LogSources(cfg);
        std::string blocker = native::ConnectBlocker(live.s);
        {
            std::lock_guard<std::mutex> g(g_consoleMu);
            g_live = live;
            g_consoleKey.clear();  // new settings: report their outcome afresh
        }
        native::Bridge* bridge = g_bridge.load();
        if (bridge) {
            bridge->Reconfigure(live.s, blocker.empty());
            if (blocker.empty()) ConsoleLine(DisplayPath(g_userPath) + " changed; connecting to " + live.s.url + " as \"" +
                                             live.s.serverName + "\"");
        } else if (blocker.empty()) {
            ConsoleLine(DisplayPath(g_userPath) + " changed");
            StartBridge(base, cfg, env);
        }
        if (!blocker.empty()) {
            BannerBlocked(live, blocker);
            if (!bridge) SetDisabled("not connected: " + blocker, cfg);
        }
    }
}

}  // namespace

void NativeStart() {
    const std::string base = PluginBaseDir();
    g_userPath = native::JoinPath(native::JoinPath(base, "takaro"), "plugin.json");
    std::string pluginJson, err;
    bool exists = false;
    native::ReadWholeFile(g_userPath, pluginJson, exists, err);
    native::EnvFn env = [](const char* name) { return EnvUtf8(name); };
    native::NativeConfig cfg = native::LoadNativeConfig(base, env, pluginJson);
    for (auto& w : cfg.warnings) PluginLog("native: config: %s", w.c_str());
    if (!cfg.enabled) {
        PluginLog("native: direct Takaro connection OFF: %s. The plugin keeps running.", cfg.disabledReason.c_str());
        ConsoleLine("direct Takaro connection off: " + cfg.disabledReason);
        SetDisabled(cfg.disabledReason, cfg);
        return;
    }

    native::StatePaths paths = native::ResolveStatePaths(base, env);
    native::ConfigWatcher::Options wo;
    wo.userPath = g_userPath;
    wo.savedPath = native::JoinPath(paths.dir, native::kSavedFileName);
    std::string savedText;
    bool savedExists = false;
    native::ReadWholeFile(wo.savedPath, savedText, savedExists, err);
    wo.legacyInstall = DirectoryExists(paths.dir) && !savedExists;
    wo.hostName = HostServerName(base);
    wo.env = env;
    wo.newIdentity = NewIdentity;
    wo.log = [](const std::string& line) { PluginLog("native: %s", line.c_str()); };
    if (std::string ms = EnvUtf8("TAKARO_CONFIG_POLL_MS"); !ms.empty() && atoi(ms.c_str()) >= 200) {
        wo.pollMs = atoi(ms.c_str());
        wo.settleMs = wo.pollMs / 2;
    }
    g_watcher = new native::ConfigWatcher(wo);  // never freed, like the bridge
    const native::LiveResolution& live = g_watcher->Start((int64_t)GetTickCount64());
    for (auto& w : g_watcher->TakeConsoleWarnings()) ConsoleLine("WARNING: " + w);
    native::ApplyLive(cfg, live);
    LogSources(cfg);
    PluginLog("native: config file %s (%s), saved copy %s%s", g_userPath.c_str(), exists ? "found" : "missing",
              wo.savedPath.c_str(), wo.legacyInstall ? " (install from before the saved copy)" : "");
    {
        std::lock_guard<std::mutex> g(g_consoleMu);
        g_live = live;
    }
    std::string blocker = native::ConnectBlocker(live.s);
    if (blocker.empty()) {
        StartBridge(base, cfg, env);
    } else {
        PluginLog("native: direct Takaro connection OFF: %s; watching %s", blocker.c_str(), g_userPath.c_str());
        BannerBlocked(live, blocker);
        SetDisabled("not connected: " + blocker, cfg);
    }
    std::thread(WatchLoop, base, cfg, env).detach();
}

std::string NativeHealthJson() {
    if (native::Bridge* b = g_bridge.load()) return b->HealthJson();
    Guard g(g_disabledLock);
    return g_disabled;
}
