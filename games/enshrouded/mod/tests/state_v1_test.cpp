// 1.0 (build 1267041) login correlation: three players join together and finish loading in a different order.
// "Machine 'M'" is peer index + 1 (live log 2026-10-15); the peer's (G) is the same for all three.
#include "state.h"
#include <cassert>
#include <iostream>
HMODULE g_selfModule=nullptr;
static void L(const char*s){PluginState::Get().OnLogLine(3,s);}
int main(){
 auto& st=PluginState::Get();
 st.SetMachineIsPeerIndexPlusOne(true);
 L("[online] Session accepted with peer (steamid:76561198202668475)");
 L("[online] Added peer 0(1) (steamid:76561198202668475)");
 L("[session] Remote player added. Player handle: 0(0)");
 L("[online] Added peer 1(1) (steamid:76561198041959712)");
 L("[session] Remote player added. Player handle: 1(1)");
 L("[online] Added peer 2(1) (steamid:76561198090698061)");
 L("[session] Remote player added. Player handle: 2(2)");
 L("[server] Machine '2': Player '1(1)' logged in");
 L("[server] Player 'Mad' logged in with Permissions:");
 L("\t - CanKickBan");
 L("[server] Machine '3': Player '2(2)' logged in");
 L("[server] Player 'Conditions' logged in with Permissions:");
 L("\t - CanEditWorld");
 L("[server] Machine '1': Player '0(0)' logged in");
 L("[server] Player 'gambit5555' logged in with Permissions:");
 L("\t - CanEditWorld");
 L("[x] other");
 std::string sid, name;
 assert(st.FindOnline("Mad", sid, name) && sid == "76561198041959712");
 assert(st.FindOnline("Conditions", sid, name) && sid == "76561198090698061");
 assert(st.FindOnline("gambit5555", sid, name) && sid == "76561198202668475");
 std::string m;
 assert(st.OnlineMachine("76561198041959712", m) && m == "2");
 std::cout << "state_v1_test: ok\n";
}
