// The unsupported-game-build notice (state.cpp): plugin.log line, /health gameSupport, testReachability reason and
// the console 'version'/'help' output all use this text.
#include "state.h"
#include <cassert>
#include <iostream>
HMODULE g_selfModule = nullptr;
int main() {
    auto& st = PluginState::Get();
    assert(st.GameSupportNotice().empty());  // before startup decides
    st.SetCapability("gameThread", "degraded", "hook installed, waiting for first tick");
    st.SetCapability("kick", "degraded", "hook installed, waiting for moderation system to run");
    st.SetCapability("players", "degraded", "log sink not resolved");
    st.SetCapability("giveItem", "degraded", "item and entity tables are for game build 1267041");
    st.SetCapability("listItems", "degraded", "item and entity tables are for game build 1267041");
    st.SetCapability("shutdown", "ok", "game console control handler");
    st.SetCapability("teleport", "degraded", "initializing");

    st.SetGameSupport("1273043", true, true, "1267041, 1273043");
    assert(st.GameSupportNotice().empty());  // supported build: nothing to say

    st.SetGameSupport("9999999", false, false, "1267041, 1273043");
    std::string n = st.GameSupportNotice();
    std::cout << n << "\n";
    assert(n == "game build 9999999 is not supported by plugin " TAKARO_PLUGIN_VERSION
                ": no gamedata section for this build (this plugin has 1267041, 1273043); the item and entity tables are "
                "for another build; capabilities off: giveItem, listItems, players; update the plugin to a release that "
                "supports this game build");

    // a gamedata section for the new build restores the hooks, but the catalogue needs layout.itemTables
    st.SetCapability("players", "ok", "log-derived");
    st.SetGameSupport("9999999", true, false, "1267041, 1273043, 9999999");
    n = st.GameSupportNotice();
    std::cout << n << "\n";
    assert(n.find("no gamedata section") == std::string::npos);
    assert(n.find(": the item and entity tables are for another build; capabilities off: giveItem, listItems;") != std::string::npos);

    st.SetGameSupport("9999999", false, true, "");
    assert(st.GameSupportNotice().find("(this plugin has none)") != std::string::npos);
    std::cout << "game_support_test ok\n";
}
