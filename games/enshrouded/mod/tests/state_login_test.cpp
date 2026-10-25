// Login -> Steam account correlation on builds where "Machine 'M'" is not always peer index + 1 (1273043): the game
// names the machine's peer ("Unable to find machine for peer") or the machine follows the one Steam auth waiting for
// it. When neither is certain the login is left unresolved instead of guessed. Log sequences replay real server
// logs with made-up accounts and names.
#include "state.h"
#include <cassert>
#include <iostream>
HMODULE g_selfModule = nullptr;
static void L(const std::string& s) { PluginState::Get().OnLogLine(3, s); }
static const char* A = "76561198000000001";
static const char* B = "76561198000000002";
static const char* C = "76561198000000003";
static const char* D = "76561198000000004";
static void login(const char* machine, const char* handle, const char* who) {
    L(std::string("[server] Machine '") + machine + "': Player '" + handle + "' logged in");
    L(std::string("[server] Player '") + who + "' logged in with Permissions:");
    L("\t - CanEditWorld");
    L("[x] other");
}
static void leave(const char* who, const char* peer) {
    L(std::string("[server] Remove Player '") + who + "'");
    L(std::string("[online] Removed peer ") + peer);
}
static size_t count(const std::string& hay, const std::string& n) {
    size_t c = 0, q = 0;
    while ((q = hay.find(n, q)) != std::string::npos) c++, q++;
    return c;
}
int main() {
    auto& st = PluginState::Get();
    st.SetMachineIsPeerIndexPlusOne(true);
    std::string sid, name, m;

    // 1) One account's connection flaps on peer slot 4 while another takes machine 5: peer index + 1 would give
    //    machine 5 to the flapping peer 4(2). The machine follows the one account that passed Steam auth.
    L(std::string("[online] Added peer 4(1) (steamid:") + A + ")");
    L(std::string("[online] Client '") + A + "' authenticated by steam");
    L(std::string("[online] Added peer 5(1) (steamid:") + B + ")");
    L("[online] Disconnecting peer 4(1)");
    L("[online] Removed peer 4(1)");
    L(std::string("[online] Client '") + B + "' authenticated by steam");
    L("[session] Add remote machine index 5 (id: 1000000001).");
    L("[session] Remote player added. Player handle: 4(1)");
    L(std::string("[online] Added peer 4(2) (steamid:") + A + ")");
    login("5", "4(1)", "Birch");
    L(std::string("[online] Client '") + A + "' authenticated by steam");
    L("[online] Disconnecting peer 4(2)");
    L("[online] Removed peer 4(2)");
    assert(st.FindOnline("Birch", sid, name) && sid == B);
    L(std::string("[online] Added peer 4(3) (steamid:") + A + ")");
    L(std::string("[online] Client '") + A + "' authenticated by steam");
    L("[session] Add remote machine index 6 (id: 1000000002).");
    L("[session] Remote player added. Player handle: 5(2)");
    login("6", "5(2)", "Ash");
    assert(st.FindOnline("Ash", sid, name) && sid == A);
    assert(st.FindOnline("Birch", sid, name) && sid == B);
    leave("Birch", "5(1)");  // Birch leaves; Ash stays online
    assert(!st.FindOnline("Birch", sid, name));
    assert(st.FindOnline("Ash", sid, name) && sid == A);
    leave("Ash", "4(3)");

    // 2) Two accounts pass Steam auth in the same millisecond and get their machines in the other order. The
    //    "Unable to find machine for peer" line before each "Add remote machine index" names that machine's peer.
    L(std::string("[online] Added peer 0(1) (steamid:") + C + ")");
    L(std::string("[online] Client '") + C + "' authenticated by steam");
    L("[session] Unable to find machine for peer 0(1).");
    L("[session] Add remote machine index 1 (id: 1000000003).");
    L("[online] Disconnecting peer 0(1)");
    L("[online] Removed peer 0(1)");
    L(std::string("[online] Added peer 0(2) (steamid:") + C + ")");
    L(std::string("[online] Added peer 1(1) (steamid:") + B + ")");
    L(std::string("[online] Client '") + C + "' authenticated by steam");
    L(std::string("[online] Client '") + B + "' authenticated by steam");
    L("[session] Unable to find machine for peer 1(1).");
    L("[session] Unable to find machine for peer 1(1).");
    L("[session] Add remote machine index 1 (id: 1000000004).");
    L("[session] Remote player added. Player handle: 0(1)");
    L("[session] Unable to find machine for peer 0(2).");
    L("[session] Unable to find machine for peer 0(2).");
    L("[session] Add remote machine index 2 (id: 1000000005).");
    L("[session] Remote player added. Player handle: 1(2)");
    login("2", "1(2)", "Cedar");
    login("1", "0(1)", "Birch");
    assert(st.FindOnline("Cedar", sid, name) && sid == C);
    assert(st.FindOnline("Birch", sid, name) && sid == B);
    assert(st.OnlineMachine(C, m) && m == "2");
    assert(st.OnlineMachine(B, m) && m == "1");
    leave("Cedar", "0(2)");
    leave("Birch", "1(1)");

    // 3) Clear on bind: a flapping peer keeps machine 3 pending while the game hands machine 3 to another peer.
    //    The older binding must go, or the login on machine 3 lands on the flapping account.
    L(std::string("[online] Added peer 2(1) (steamid:") + A + ")");
    L(std::string("[online] Client '") + A + "' authenticated by steam");
    L("[session] Unable to find machine for peer 2(1).");
    L("[session] Add remote machine index 3 (id: 1000000006).");
    L(std::string("[online] Added peer 3(1) (steamid:") + D + ")");
    L(std::string("[online] Client '") + D + "' authenticated by steam");
    L("[session] Unable to find machine for peer 3(1).");
    L("[session] Add remote machine index 3 (id: 1000000007).");
    login("3", "3(1)", "Elm");
    assert(st.FindOnline("Elm", sid, name) && sid == D);
    assert(!st.FindOnline("Ash", sid, name));
    leave("Elm", "3(1)");
    L("[online] Removed peer 2(1)");

    // 4) Same-millisecond auths without a peer line: no certain binding. Both logins stay unresolved (logged, not
    //    tracked) instead of one account being guessed for the other.
    std::string ev0 = st.EventsJson(0, 100000);
    L(std::string("[online] Added peer 6(1) (steamid:") + A + ")");
    L(std::string("[online] Added peer 7(1) (steamid:") + C + ")");
    L(std::string("[online] Client '") + A + "' authenticated by steam");
    L(std::string("[online] Client '") + C + "' authenticated by steam");
    L("[session] Add remote machine index 8 (id: 1000000008).");
    L("[session] Add remote machine index 7 (id: 1000000009).");
    login("8", "7(1)", "Cedar");
    login("7", "6(1)", "Ash");
    assert(!st.FindOnline("Cedar", sid, name));
    assert(!st.FindOnline("Ash", sid, name));
    std::string ev1 = st.EventsJson(0, 100000);
    assert(count(ev1, "\"type\":\"player-connected\"") == count(ev0, "\"type\":\"player-connected\""));
    L("[online] Removed peer 6(1)");
    L("[online] Removed peer 7(1)");

    // 5) A named peer the plugin does not track (no Steam id): its machine is not handed to another account.
    L(std::string("[online] Added peer 8(1) (steamid:") + B + ")");
    L(std::string("[online] Client '") + B + "' authenticated by steam");
    L("[session] Unable to find machine for peer 9(1).");
    L("[session] Add remote machine index 10 (id: 1000000010).");
    login("10", "9(1)", "Stranger");
    assert(!st.FindOnline("Stranger", sid, name) && !st.FindOnline(B, sid, name));
    // the account's own machine still binds afterwards
    L("[session] Unable to find machine for peer 8(1).");
    L("[session] Add remote machine index 9 (id: 1000000011).");
    login("9", "8(1)", "Birch");
    assert(st.FindOnline("Birch", sid, name) && sid == B);
    leave("Birch", "8(1)");

    // 6) With the character-name join (connect held until the name is read), the held connect carries the right
    //    account and the character name.
    st.SetCharacterNamesAvailable(true);
    st.SetConnectNameWaitMs(60000);
    size_t c0 = count(st.EventsJson(0, 100000), "\"type\":\"player-connected\"");
    L(std::string("[online] Added peer 0(3) (steamid:") + C + ")");
    L(std::string("[online] Added peer 1(3) (steamid:") + A + ")");
    L(std::string("[online] Client '") + C + "' authenticated by steam");
    L(std::string("[online] Client '") + A + "' authenticated by steam");
    L("[session] Unable to find machine for peer 1(3).");
    L("[session] Add remote machine index 1 (id: 1000000012).");
    L("[session] Unable to find machine for peer 0(3).");
    L("[session] Add remote machine index 2 (id: 1000000013).");
    login("2", "1(2)", "Cedar");
    login("1", "0(1)", "Ash");
    st.SetCharacterName("Cedar", "Hero Cedar");
    st.SetCharacterName("Ash", "Hero Ash");
    st.Housekeep();
    std::string ev = st.EventsJson(0, 100000);
    assert(count(ev, "\"type\":\"player-connected\"") == c0 + 2);
    size_t qa = ev.find("\"name\":\"Hero Ash\""), qc = ev.find("\"name\":\"Hero Cedar\"");
    assert(qa != std::string::npos && qc != std::string::npos);
    assert(ev.rfind("\"gameId\":", qa) == ev.rfind(std::string("\"gameId\":\"") + A + "\"", qa));
    assert(ev.rfind("\"gameId\":", qc) == ev.rfind(std::string("\"gameId\":\"") + C + "\"", qc));
    std::cout << "state_login_test: ok\n";
}
