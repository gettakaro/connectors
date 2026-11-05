#include "state.h"
#include <cassert>
#include <iostream>
HMODULE g_selfModule=nullptr;
static void L(const char*s){PluginState::Get().OnLogLine(3,s);}
static size_t count2(const std::string& ev,const char*n){size_t c=0,q=0;while((q=ev.find(n,q))!=std::string::npos){c++;q++;}return c;}
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
 // 1.0 builds expose character names: player-connected waits for it, getPlayers hides the player until then
 auto login=[&](const char* peer,const char* sid,const char* mach,const char* who){
   std::string a=std::string("[online] Added peer ")+peer+" (steamid:"+sid+")"; L(a.c_str());
   std::string m=std::string("[server] Machine '")+mach+"': Player '"+peer[0]+"(0)' logged in"; L(m.c_str());
   std::string w=std::string("[server] Player '")+who+"' logged in with Permissions:"; L(w.c_str());
   L("\t - CanEditWorld"); L("[x] other"); };
 auto connects=[&](){ std::string e=st.EventsJson(0,5000); return count2(e,"\"type\":\"player-connected\""); };
 st.SetCharacterNamesAvailable(true);
 size_t c0=connects();
 login("4(5)","76561190000000004","5","Carol");
 assert(connects()==c0);                                         // held: no character name yet
 assert(st.PlayersJson().find("Carol")==std::string::npos);      // and not listed under the Steam name
 st.SetCharacterName("Carol","Hero One");
 assert(connects()==c0);                                         // the game-thread name refresh never emits
 st.Housekeep();
 assert(connects()==c0+1);
 { std::string e=st.EventsJson(0,5000); size_t q=e.rfind("\"type\":\"player-connected\"");
   assert(e.find("Hero One",q)!=std::string::npos && e.find("Carol",q)==std::string::npos); }
 assert(st.PlayersJson().find("Hero One")!=std::string::npos);
 // no character name in time: the Steam name after the wait
 st.SetConnectNameWaitMs(0);
 login("5(6)","76561190000000005","6","Dave");
 st.Housekeep();
 assert(connects()==c0+2);
 { std::string e=st.EventsJson(0,5000); size_t q=e.rfind("\"type\":\"player-connected\""); assert(e.find("Dave",q)!=std::string::npos); }
 // leaving while held: the connect goes out first, so the pair stays complete
 st.SetConnectNameWaitMs(60000);
 login("6(7)","76561190000000006","7","Erin");
 assert(connects()==c0+2);
 L("[online] Removed peer 6(7)");
 { std::string e=st.EventsJson(0,5000); size_t qc=e.rfind("\"type\":\"player-connected\""), qd=e.rfind("\"type\":\"player-disconnected\"");
   assert(connects()==c0+3 && qc<qd && e.find("Erin",qc)!=std::string::npos); }
 // character level: stored from the game thread, listed in getPlayers JSON, absent until read
 st.SetConnectNameWaitMs(0);
 login("7(8)","76561190000000007","8","Finn");
 st.Housekeep();
 assert(st.PlayersJson().find("\"level\"")==std::string::npos);
 st.SetLevel("Finn",12,340,900);
 { std::string j=st.PlayersJson(); assert(j.find("\"level\":12,\"experience\":340,\"experienceRequired\":900")!=std::string::npos); }
 st.SetLevel("Nobody",5,1,2);  // not online: ignored
 assert(count2(st.PlayersJson(),"\"level\":")==1);
 std::cout<<"PASS\n";
}
