// Wires the game-independent native connector (src/native/) to this plugin: GameApi calls the plugin's
// contract router in-process, capabilities land in PluginState, and the transport is WinHTTP.
#include "common.h"
#include "native/bridge.h"
#include "native/config.h"
#include "native/fileio.h"
#include "native/json_util.h"
#include "native/persistence.h"
#include "native/transport_winhttp.h"
#include "plugin_api.h"
#include "state.h"

#include <atomic>

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

PluginGameApi g_api;
std::atomic<native::Bridge*> g_bridge{nullptr};
SrwLock g_disabledLock;
std::string g_disabled = "{\"enabled\":false,\"reason\":\"starting\"}";

}  // namespace

void NativeStart() {
    const std::string base = PluginBaseDir();
    std::string pluginJson, err;
    bool exists = false;
    native::ReadWholeFile(native::JoinPath(native::JoinPath(base, "takaro"), "plugin.json"), pluginJson, exists, err);
    native::EnvFn env = [](const char* name) { return EnvUtf8(name); };
    native::NativeConfig cfg = native::LoadNativeConfig(base, env, pluginJson);
    for (auto& w : cfg.warnings) PluginLog("native: config: %s", w.c_str());
    for (auto& s : cfg.sources) PluginLog("native: config %s from %s", s.first.c_str(), s.second.c_str());
    if (!cfg.enabled) {
        PluginLog("native: direct Takaro connection OFF: %s. The plugin keeps running.", cfg.disabledReason.c_str());
        Guard g(g_disabledLock);
        g_disabled = native::ObjBuilder()
                         .B("enabled", false)
                         .S("reason", cfg.disabledReason)
                         .Raw("config", native::ConfigSummaryJson(cfg))
                         .Done();
        return;
    }
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
    auto* bridge = new native::Bridge(bo);
    PluginLog("native: starting direct Takaro connection to %s as '%s' (state %s)", cfg.url.c_str(),
              cfg.serverName.c_str(), store->Paths().dir.c_str());
    bridge->Start();
    g_bridge = bridge;
}

std::string NativeHealthJson() {
    if (native::Bridge* b = g_bridge.load()) return b->HealthJson();
    Guard g(g_disabledLock);
    return g_disabled;
}
