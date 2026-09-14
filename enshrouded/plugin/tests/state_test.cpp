#include "state.h"
#include <cassert>
#include <iostream>
HMODULE g_selfModule=nullptr;
static void L(const char*s){PluginState::Get().OnLogLine(3,s);}
int main(){
 auto& st=PluginState::Get();
 // A joins, B joins concurrently
 L("[online] Added peer 0(1) (steamid:76561190000000001)");
 L("[online] Added peer 1(2) (steamid:76561190000000002)");
 L("[server] Machine '2': Player '1(0)' logged in");
 L("[server] Player 'Bob' logged in with Permissions:");
 L("\t - CanKickBan"); L("\t - CanAccessInventories"); L("\t - CanEditBase"); L("\t - CanEditWorld"); L("\t - CanExtendBase"); L("\t - CanReceiveEXP");
 L("[I] something else");
 L("[server] Machine '1': Player '0(0)' logged in");
 L("[server] Player 'Al'ice' logged in with Permissions:");
 L("\t - CanEditWorld");
 st.Housekeep(); // not yet 750ms
 L("[x] other");
 std::cout<<st.PlayersJson()<<"\n";
 // Alice leaves, rejoins before "Removed peer"
 L("[server] Remove Player 'Al'ice'");
 L("[online] Added peer 2(3) (steamid:76561190000000001)");
 L("[online] Removed peer 0(1)");
 L("[server] Machine '3': Player '2(0)' logged in");
 L("[server] Player 'Al'ice' logged in with Permissions:");
 L("\t - CanEditWorld");
 L("[x] other");
 // Bob drops without Remove Player
 L("[online] Removed peer 1(2)");
 // pending peer that never logs in
 L("[online] Added peer 3(4) (steamid:76561190000000009)");
 L("[online] Removed peer 3(4)");
 std::cout<<st.PlayersJson()<<"\n";
 std::string ev=st.EventsJson(0,5000);
 size_t p=0; while((p=ev.find("\"type\":\"player-",p))!=std::string::npos){ size_t e=ev.find("\"ts\"",p); std::cout<<ev.substr(p,e-p)<<"\n"; p=e;}
 auto count=[&](const char*n){size_t c=0,q=0;while((q=ev.find(n,q))!=std::string::npos){c++;q++;}return c;};
 assert(count("\"type\":\"player-connected\"")==3);
 assert(count("\"type\":\"player-disconnected\"")==2);
 std::string one=st.PlayersJson(); assert(one.find("2(3)")!=std::string::npos && one.find("Bob")==std::string::npos);
 std::cout<<"PASS\n";
}
