#include "native_behavior.h"
#include "native_log.h"
#include "actions.h"
#include "state.h"
#include "gamethread.h"
#include "reflect.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

using Json=nlohmann::json;
namespace fs=std::filesystem;
namespace {
std::vector<state::BanRecord> bans;
uint64_t banRevision=1;
std::string lastSender,lastRecipient,lastMessage,lastGiveCode;
double lastGiveAmount=0;
bool noPawn=false;
bool partialUnban=false;
std::string playersCapability="ok";
std::string kickCapability="ok";
bool reflectionValidated=true;
bool playersDown=false;
std::optional<std::string> gameHttpPlayers;
Json Parse(const std::string& s){return Json::parse(s);}
Json Body(const JsonValue& v){Json j=Json::object();for(const auto& x:v.obj){if(x.second.type==JsonValue::String)j[x.first]=x.second.str;else if(x.second.type==JsonValue::Number)j[x.first]=x.second.num;else if(x.second.type==JsonValue::Null)j[x.first]=nullptr;}return j;}
template<class T>void Check(T&& ok,const std::string& what){if(!static_cast<bool>(ok))throw std::runtime_error(what);}
bool HasString(const Json& body,const char* key){return body.contains(key)&&body[key].is_string()&&!body[key].get<std::string>().empty();}
Actions::Result BadBody(const char* field){return {400,Json{{"error",std::string("missing ")+field}}.dump()};}
}

std::string ConfigValue(const char* env,const char*,const std::string& def){const char* v=getenv(env);return v?v:def;}
std::string Redact(const std::string& s){return s;} // URL fields are masked by real EventsParse::RedactLogLine.
std::string JsonEscape(const std::string& s){auto quoted=Json(s).dump();return quoted.substr(1,quoted.size()-2);}
const std::string& PluginDataDir(){static std::string d="/tmp/vein-native-test";return d;}
std::string IsoNowUtc(){return "2026-09-24T12:00:00.000Z";}
bool JsonParse(const std::string& s,JsonValue& out){Json j=Json::parse(s,nullptr,false);if(j.is_discarded())return false;std::function<JsonValue(const Json&)> cv=[&](const Json& x){JsonValue v;if(x.is_object()){v.type=JsonValue::Object;for(auto it=x.begin();it!=x.end();++it)v.obj.push_back({it.key(),cv(it.value())});}else if(x.is_array()){v.type=JsonValue::Array;for(auto& y:x)v.arr.push_back(cv(y));}else if(x.is_string()){v.type=JsonValue::String;v.str=x.get<std::string>();}else if(x.is_number()){v.type=JsonValue::Number;v.num=x.get<double>();v.str=x.dump();}else if(x.is_boolean()){v.type=JsonValue::Bool;v.b=x.get<bool>();}return v;};out=cv(j);return true;}
namespace state {
uint64_t BanRevision(){return banRevision;}
BanSnapshot ReadBans(){return {bans,banRevision};}
bool BanAdd(const BanRecord& record){bans.erase(std::remove_if(bans.begin(),bans.end(),[&](const auto& row){return row.gameId==record.gameId;}),bans.end());bans.push_back(record);++banRevision;return true;}
bool BanAddIfRevision(const BanRecord& record,uint64_t expected){return banRevision==expected&&BanAdd(record);}
bool FlushBans(){return true;}
std::string BanPersistenceError(){return {};}
std::vector<BanRecord> BanList(){return bans;}
}
namespace GameThread {bool Alive(){return true;}}
namespace Reflect {bool Validated(){return reflectionValidated;}}
namespace NativeTransport {std::optional<std::string> FetchGamePlayers(){return gameHttpPlayers;}}
PluginState& PluginState::Get(){static PluginState x;return x;}
std::string PluginState::Capability(const std::string& name)const{return name=="players"?playersCapability:name=="kick"?kickCapability:"ok";}
std::string PluginState::CapabilitiesJson()const{return Json{{"players",playersCapability},{"kick",kickCapability},{"chatEvents","unimplemented"},{"reflection",reflectionValidated?"ok":"degraded"}}.dump();}
namespace Actions {
Result Players(){return playersDown?Result{503,R"({"error":"reflected player getter unavailable"})"}:
    Result{200,R"([{"gameId":"76561198000000001","name":"A","characterName":"UnseenCharacter","online":true},{"gameId":"76561198000000002","name":"B","online":false}])"};}
Result Player(const std::string&){return {404,R"({"error":"player not online"})"};}
Result PlayerLocation(const std::string&){return noPawn?Result{503,R"({"error":"player has no pawn yet"})"}:Result{200,R"({"x":1,"y":2,"z":3,"yaw":90,"ageMs":10})"};}
Result PlayerInventory(const std::string&){return {200,R"([{"code":"MRE","name":"Meal","amount":1},{"code":"MRE","name":"Meal2","amount":1},{"code":"wood","name":"Wood","amount":3}])"};}
Result Items(const std::string&){return {200,R"([{"code":"MRE","name":"Meal","description":"food"}])"};}
Result Entities(){return {200,R"([{"code":"zombie","name":"Zombie","type":"enemy"}])"};}
Result Locations(){return {200,R"([{"code":"spawn","name":"Spawn","position":{"x":1,"y":2,"z":3}}])"};}
Result Bans(){return {200,R"([{"gameId":"76561198000000001","name":"A","reason":"r","expiresAt":null}])"};}
Result Message(const JsonValue& b){auto j=Body(b);if(!HasString(j,"text"))return BadBody("text");lastMessage=j.value("text","");lastSender=j.value("senderName","");lastRecipient=j.value("recipientGameId","");return {200,R"({"success":true,"verified":false})"};}
Result Teleport(const JsonValue& b){auto j=Body(b);if(!HasString(j,"gameId"))return BadBody("gameId");if(!HasString(j,"target")&&!(j.contains("x")&&j["x"].is_number()&&j.contains("y")&&j["y"].is_number()&&j.contains("z")&&j["z"].is_number()))return BadBody("x/y/z or target");return {200,R"({"success":true,"verified":true})"};}
Result Give(const JsonValue& b){auto j=Body(b);if(!HasString(j,"gameId"))return BadBody("gameId");if(!HasString(j,"code"))return BadBody("code");if(j.contains("amount")&&(!j["amount"].is_number()||j["amount"].get<double>()<=0||j["amount"].get<double>()>1000))return BadBody("positive amount <=1000");lastGiveCode=j["code"].get<std::string>();lastGiveAmount=j.value("amount",1.0);return {200,R"({"success":true,"verified":true})"};}
Result Kick(const JsonValue& b){auto j=Body(b);if(!HasString(j,"gameId"))return BadBody("gameId");return {200,R"({"success":true,"verified":true})"};}
Result Ban(const JsonValue& b){auto j=Body(b);if(!HasString(j,"gameId"))return BadBody("gameId");std::string id=j.value("gameId","");bans.erase(std::remove_if(bans.begin(),bans.end(),[&](auto& x){return x.gameId==id;}),bans.end());bans.push_back({id,"",j.value("reason",""),"",j.value("expiresAt","")});++banRevision;return {200,R"({"success":true,"verified":true})"};}
Result Unban(const JsonValue& b){auto j=Body(b);if(!HasString(j,"gameId"))return BadBody("gameId");std::string id=j.value("gameId","");bans.erase(std::remove_if(bans.begin(),bans.end(),[&](auto& x){return x.gameId==id;}),bans.end());++banRevision;return partialUnban?Result{409,R"({"error":"game ban still enforced"})"}:Result{200,R"({"success":true,"verified":true})"};}
Result UnbanIfRevision(const JsonValue& b,uint64_t expected){return banRevision==expected?Unban(b):Result{409,R"({"error":"ban revision changed before expiry"})"};}
Result Command(const JsonValue& b){auto j=Body(b);if(!HasString(j,"command"))return BadBody("command");return j.value("command","")=="bad"?Result{400,R"({"error":"bad command"})"}:Result{200,R"({"success":true,"output":"saved"})"};}
Result Shutdown(){throw std::runtime_error("shutdown invoked before response");}
size_t PendingBanJobs(){return 0;}
}

int main(){
    char tmp[]="/tmp/vein-behavior-XXXXXX";char* path=mkdtemp(tmp);Check(path,"mkdtemp");
    setenv("TAKARO_STATE_DIR",path,1);setenv("TAKARO_SENDER_NAME","Bot",1);
    {std::ofstream initial(fs::path(path)/"known-players.json");initial<<R"([{"gameId":"76561198000000001","name":"A"}])";}
    NativePersistence::Store store;Check(store.Load(),"initial outbox load");
    NativeBehavior::Engine engine(store);Check(engine.Load(),"legacy load");
    auto checkConfiguredSender=[&](const std::string& expected){
        NativeBehavior::Engine configured(store);Check(configured.Load(),"sender config load");
        auto p=configured.PrepareAction("sender-config","sendMessage",R"({"message":"hello"})",1);
        auto result=configured.ApplyOutcome(p,NativeBehavior::Engine::ExecuteAction(p));
        Check(result.errorText.empty()&&lastSender==expected,"sender config fallback: expected "+expected+", got "+lastSender);
    };
    setenv("TAKARO_SENDER_NAME","  Chat Bot  ",1);checkConfiguredSender("Chat Bot");
    setenv("TAKARO_SENDER_NAME","   ",1);setenv("TAKARO_SERVER_CHAT_NAME","Unused Alias",1);
    setenv("TAKARO_SERVER_NAME","Named Server",1);checkConfiguredSender("Named Server");
    unsetenv("TAKARO_SENDER_NAME");setenv("TAKARO_SERVER_CHAT_NAME","  Alias Bot  ",1);checkConfiguredSender("Alias Bot");
    unsetenv("TAKARO_SERVER_CHAT_NAME");unsetenv("TAKARO_SERVER_NAME");checkConfiguredSender("Takaro Dev Vein");
    setenv("TAKARO_SENDER_NAME","Bot",1);
    Check(Parse(engine.Snapshot()->knownPlayersJson).size()==1&&Parse(engine.Snapshot()->onlinePlayersJson).empty()&&Parse(engine.Snapshot()->timedBansJson).empty(),"missing legacy files do not inherit preceding contents");
    Check(!engine.SuppressRingConnection("player-connected")&&!engine.SuppressRingConnection("chat-message"),"healthy auto mode keeps ring ownership");
    playersCapability="degraded";
    Check(engine.SuppressRingConnection("player-connected")&&engine.SuppressRingConnection("player-disconnected")&&
          !engine.SuppressRingConnection("chat-message"),"auto mode tails only connections on player capability degradation");
    playersCapability="ok";
    const std::string id="76561198000000001";
    auto run=[&](const std::string& name,const Json& args){auto p=engine.PrepareAction("r",name,args.dump(),1);auto o=NativeBehavior::Engine::ExecuteAction(p);auto a=engine.ApplyOutcome(p,std::move(o));Check(a.errorText.empty(),name+": "+a.errorText);return Parse(a.payloadJson);};
    Check(run("testReachability",Json::object())["connectable"]==true,"reachability");
    kickCapability="degraded";
    auto reachability=run("testReachability",Json::object());
    Check(reachability["connectable"]==true&&reachability["reason"].get<std::string>().find("kick=degraded")!=std::string::npos&&
          reachability["reason"].get<std::string>().find("chatEvents")==std::string::npos,
          "degraded capability is reported without treating static unimplemented as a failure");
    kickCapability="ok";reflectionValidated=false;
    reachability=run("testReachability",Json::object());
    Check(reachability["connectable"]==true&&reachability["reason"].get<std::string>().find("Vein plugin degraded")!=std::string::npos,
          "degraded plugin remains connectable with explicit reason");
    reflectionValidated=true;
    Check(run("getPlayers",Json::object()).size()==1,"online players only");
    playersDown=true;
    gameHttpPlayers=Json{{"Players",Json::array({Json{{"steamID64","76561198000000001"},{"playerName","HttpAlias"},{"ping",42}}})}}.dump();
    Check(run("getPlayers",Json::object())[0]["name"]=="HttpAlias","built-in game HTTP player fallback maps alternate JSON keys");
    Check(run("getPlayer",Json{{"gameId","HttpAlias"}})["gameId"]=="76561198000000001","getPlayer finds HTTP fallback row");
    auto fallbackAlias=engine.PrepareAction("http-alias","banPlayer",Json{{"gameId","HttpAlias"}}.dump(),1);
    Check(NativeBehavior::Engine::ResolveBanTarget(fallbackAlias)=="76561198000000001","player ID resolution inherits built-in HTTP fallback");
    bool deepFallbackRejected=false;
    try{NativeBehavior::Engine::NormalizeGamePlayers(std::string(65,'[')+"0"+std::string(65,']'));}
    catch(const std::exception&){deepFallbackRejected=true;}
    Check(deepFallbackRejected,"nested game HTTP JSON over depth 64 rejected before parser recursion");
    gameHttpPlayers.reset();
    auto unavailable=engine.PrepareAction("unavailable","getPlayers","{}",1);
    Check(!NativeBehavior::Engine::ExecuteAction(unavailable).errorText.empty(),"failed plugin and game HTTP getter reports error");
    playersDown=false;
    Check(run("getPlayer",Json{{"player",{{"gameId","steam:76561198000000003"}}}})["online"]==false,"unknown offline player gets minimal valid record");
    Check(run("getPlayerLocation",Json{{"gameId",id}})==Json{{"x",1},{"y",2},{"z",3}},"location xyz DTO");
    noPawn=true;
    auto missingLocation=engine.PrepareAction("missing","getPlayerLocation",Json{{"gameId",id}}.dump(),1);
    Check(!NativeBehavior::Engine::ExecuteAction(missingLocation).errorText.empty(),"outside event window must fail");
    auto join=engine.MapEvent(Json{{"seq",1},{"type","player-connected"},{"data",{{"player",{{"gameId",id},{"name","A"}}}}}}.dump());
    Check(store.AdmitSynthetic(join.frame),"durable join before observe");
    {NativePersistence::Store afterCrash;Check(afterCrash.Load(),"reload before legacy online mirror");NativeBehavior::Engine derived(afterCrash);Check(derived.Load(),"rebuild derived online after crash");Check(Parse(derived.Snapshot()->onlinePlayersJson).size()==1,"admitted join survives crash before mirror");}
    Check(engine.ObserveAdmitted(join),"observe join opens enrichment window");
    Check(run("getPlayerLocation",Json{{"gameId",id}})==Json{{"x",1},{"y",2},{"z",3}},"cached location during join window");
    noPawn=false;
    auto inv=run("getPlayerInventory",Json{{"gameId",id}});Check(inv.size()==2&&inv[0]["amount"]==2&&inv[0]["name"]=="Meal","inventory aggregation");
    Check(run("giveItem",Json{{"gameId",id},{"item","MRE"},{"quality",nullptr}}).empty()&&lastGiveCode=="MRE"&&lastGiveAmount==1,
          "give maps Takaro item to plugin code and ignores null quality");
    Check(run("giveItem",Json{{"gameId",id},{"item",{{"code","wood"}}},{"quantity",3}}).empty()&&lastGiveCode=="wood"&&lastGiveAmount==3,
          "give object code and quantity map to plugin body");
    Check(run("listItems",Json::object())[0]["code"]=="MRE","items");
    Check(run("listEntities",Json::object())[0]["type"]=="hostile","entity mapping");
    Check(run("listLocations",Json::object())[0]["position"]["z"]==3,"locations");
    Check(run("executeConsoleCommand",Json{{"command","bad"}})["success"]==false,"command error DTO");
    Check(run("executeConsoleCommand",Json{{"command","save"}})["rawResult"]=="saved","command output");
    Check(run("sendMessage",Json{{"message","hi"},{"opts",{{"senderNameOverride","Captain"},{"recipient",{{"gameId",id}}}}}}).empty()&&lastSender=="Captain"&&lastRecipient==id,"sender/recipient");
    Check(run("sendMessage",Json{{"message","hi"},{"opts",nullptr}}).empty()&&lastSender=="Bot","sender fallback/null opts");
    Check(run("teleportPlayer",Json{{"gameId",id},{"x",1},{"y",2},{"z",3},{"dimension",nullptr}}).empty(),"teleport");
    auto notFinite=engine.PrepareAction("nan","teleportPlayer",Json{{"gameId",id},{"x","NaN"},{"y",2},{"z",3}}.dump(),1);
    Check(!NativeBehavior::Engine::ExecuteAction(notFinite).errorText.empty(),"nonfinite numeric argument rejected");
    auto hugeDate=engine.PrepareAction("date-range","banPlayer",Json{{"gameId",id},{"expiresAt",1e308}}.dump(),1);
    hugeDate.canonicalBanId=NativeBehavior::Engine::ResolveBanTarget(hugeDate);
    Check(NativeBehavior::Engine::ExecuteAction(hugeDate).errorText.find("Date range")!=std::string::npos,
          "out-of-range numeric expiry returns explicit error without undefined integer conversion");
    auto futureDate=engine.PrepareAction("future-date","banPlayer",Json{{"gameId",id},{"expiresAt",1e13}}.dump(),1);
    futureDate.canonicalBanId=NativeBehavior::Engine::ResolveBanTarget(futureDate);
    Check(NativeBehavior::Engine::ExecuteAction(futureDate).errorText.empty()&&
          bans.back().expiresAt.rfind("2286-",0)==0,"valid future numeric expiry formats without chrono overflow");
    bans.clear();
    Check(run("kickPlayer",Json{{"gameId",id}}).empty(),"kick");
    auto unseen=engine.PrepareAction("unseen","banPlayer",Json{{"gameId","UnseenCharacter"}}.dump(),1);
    Check(NativeBehavior::Engine::ResolveBanTarget(unseen)==id,"unseen character resolves on worker before journal");
    auto ban=engine.PrepareAction("ban1","banPlayer",Json{{"gameId","A"},{"expiresAt","2026-09-25T00:00:00Z"}}.dump(),1);Check(NativeBehavior::Engine::NeedsBanResolution(ban),"ban resolution phase");Check(!engine.BeforeExecute(ban),"unresolved ban cannot be journaled");ban.canonicalBanId=NativeBehavior::Engine::ResolveBanTarget(ban);Check(ban.canonicalBanId==id&&engine.BeforeExecute(ban),"canonical ban intent");auto bo=NativeBehavior::Engine::ExecuteAction(ban);Check(engine.ApplyOutcome(ban,std::move(bo)).errorText.empty(),"ban apply");
    Check(bans.size()==1&&bans[0].gameId==id,"persona canonicalized before ban");
    Check(run("listBans",Json::object())[0]["expiresAt"]=="2026-09-25T00:00:00.000Z","timed ban overlay");
    auto unban=engine.PrepareAction("unban1","unbanPlayer",Json{{"gameId",id}}.dump(),1);unban.canonicalBanId=NativeBehavior::Engine::ResolveBanTarget(unban);Check(engine.BeforeExecute(unban),"unban intent");Check(engine.ApplyOutcome(unban,NativeBehavior::Engine::ExecuteAction(unban)).errorText.empty(),"unban apply");
    auto interrupted=engine.PrepareAction("crash1","banPlayer",Json{{"gameId",id},{"expiresAt","2026-09-26T00:00:00Z"}}.dump(),1);
    interrupted.canonicalBanId=NativeBehavior::Engine::ResolveBanTarget(interrupted);
    Check(engine.BeforeExecute(interrupted),"pre-dispatch journal durable");
    Check(NativeBehavior::Engine::ExecuteAction(interrupted).errorText.empty(),"game ban changed before crash");
    NativeBehavior::Engine recovered(store);Check(recovered.Load(),"ban intent crash recovery");
    Check(recovered.NeedsBanVerification(),"startup retains ban intent until worker verifies game list");
    Check(recovered.ReconcileBanIntents(NativeBehavior::Engine::VerifyBanState()),"verified ban recovery");
    Check(Parse(recovered.Snapshot()->timedBansJson)[0]["expiresAt"]=="2026-09-26T00:00:00.000Z","recovery retains current ban");
    // After native migration, an external permanent ban must win over a
    // stale timed mirror on every later restart.
    bans[0].expiresAt.clear();++banRevision;
    Check(store.SaveLegacy("timedBans",R"([{"gameId":"76561198000000001","expiresAt":"2026-09-26T00:00:00.000Z","reason":"legacy"}])"),"legacy timed state");
    NativeBehavior::Engine stale(store);Check(stale.Load(),"native-marked restart");
    Check(bans[0].expiresAt.empty()&&Parse(stale.Snapshot()->timedBansJson).empty(),
          "native restart does not resurrect stale expiry over permanent ban");
    bans[0].expiresAt="2026-09-26T00:00:00.000Z";++banRevision;
    Check(store.SaveLegacy("timedBans",R"([{"gameId":"76561198000000001","expiresAt":"2026-09-26T00:00:00.000Z","reason":"legacy"}])"),"current timed state");
    NativeBehavior::Engine upgraded(store);Check(upgraded.Load(),"current timed ban restart");
    Check(bans[0].expiresAt=="2026-09-26T00:00:00.000Z","current timed ban retained");
    bans.clear();++banRevision;NativeBehavior::Engine orphan(store);Check(orphan.Load(),"native orphan mirror cleanup");
    Check(Parse(orphan.Snapshot()->timedBansJson).empty(),"native stale timed row removed when enforcement record absent");
    bans.push_back({id,"","legacy","","2026-09-26T00:00:00.000Z"});
    ++banRevision;
    partialUnban=true;
    auto partial=upgraded.PrepareAction("partial","unbanPlayer",Json{{"gameId",id}}.dump(),1);
    partial.canonicalBanId=NativeBehavior::Engine::ResolveBanTarget(partial);
    Check(upgraded.BeforeExecute(partial),"partial unban journal");
    auto failed=upgraded.ApplyOutcome(partial,NativeBehavior::Engine::ExecuteAction(partial));
    Check(!failed.errorText.empty()&&Parse(upgraded.Snapshot()->timedBansJson).size()==1&&upgraded.NeedsBanVerification(),
          "partial unban retains expiry and durable intent");
    Check(!upgraded.ReconcileBanIntents(NativeBehavior::Engine::VerifyBanState()),
          "game ban verification refuses to clear uncertain unban");
    auto retries=upgraded.DueTimedBans(1800000000000LL);
    Check(retries.size()==1&&retries[0].requestId=="partial","expired partial unban retries same journal intent");
    partialUnban=false;Check(upgraded.BeforeExecute(retries[0]),"idempotent retry intent");
    Check(upgraded.ApplyOutcome(retries[0],NativeBehavior::Engine::ExecuteAction(retries[0])).errorText.empty(),"expiry retry clears ban");
    Check(Parse(upgraded.Snapshot()->timedBansJson).empty()&&!upgraded.NeedsBanVerification(),"verified retry clears expiry and intent");
    auto intentFsync=upgraded.PrepareAction("intent-fsync","banPlayer",Json{{"gameId",id}}.dump(),1);
    intentFsync.canonicalBanId=NativeBehavior::Engine::ResolveBanTarget(intentFsync);
    NativePersistence::TestDirectorySyncFailure([](const std::string& file){return file.find("ban-intent.json")!=std::string::npos;});
    Check(!upgraded.BeforeExecute(intentFsync),"unsynced visible ban intent cannot authorize mutation");
    NativePersistence::TestDirectorySyncFailure({});
    Check(upgraded.BeforeExecute(intentFsync),"duplicate ban intent retries directory fsync before mutation");
    Check(store.FinishBanIntent("intent-fsync"),"test intent cleanup");
    Check(run("shutdown",Json::object()).empty()==true,"shutdown payload");auto shutdown=engine.PrepareAction("shutdown","shutdown","{}",1);Check(NativeBehavior::Engine::ExecuteAction(shutdown).deferredShutdown,"shutdown deferred");
    auto command=engine.PrepareAction("console","executeConsoleCommand",R"({"command":" shutdown\n"})",1);Check(NativeBehavior::Engine::ExecuteAction(command).deferredShutdown,"console shutdown deferred");
    for(const auto& type:{"player-connected","player-disconnected","chat-message","player-death","entity-killed","log"}){
        Json data={{"player",{{"gameId",id},{"name","A"}}},{"msg","hi"},{"entity","zombie"},{"cause","fall"}};
        auto m=engine.MapEvent(Json{{"seq",1},{"type",type},{"data",data},{"ts","2026-09-24T12:00:00Z"}}.dump());Check(m.valid,std::string(type)+" event mapping");auto frame=Parse(*m.frame);Check(frame["type"]=="gameEvent"&&frame["payload"]["type"]==type,std::string(type)+" wire frame");
        if(std::string(type)=="entity-killed")Check(frame["payload"]["data"]["weapon"]=="","weapon required");
        if(std::string(type)=="player-death")Check(frame["payload"]["data"]["msg"]=="A died (fall)","death cause");
    }
    auto fractional=engine.MapEvent(Json{{"seq",2},{"type","chat-message"},{"data",{{"msg","fraction"}}},{"ts","2026-09-24T12:00:00.317+02:00"}}.dump());
    Check(Parse(*fractional.frame)["payload"]["data"]["timestamp"]=="2026-09-24T10:00:00.317Z","fractional timestamp/offset preserved");
    auto f=std::make_shared<const std::string>(R"({"type":"gameEvent","payload":{"type":"log","data":{"msg":"x"}}})");
    Check(store.SwitchBoot("boot-a"),"boot switch");auto a1=store.Admit({"boot-a",1},f);Check(a1,"admit first");
    auto synthetic=store.AdmitSynthetic(f);Check(synthetic,"admit synthetic");Check(store.Current().scan.seq==1,"synthetic does not move scan");
    Check(store.SwitchBoot("boot-b"),"second boot");auto a2=store.Admit({"boot-b",1},f);Check(a2,"new boot same seq");
    Check(store.ConfirmThrough(synthetic.outboxId),"confirm old boot and synthetic");Check(store.Current().confirmed.bootId=="boot-b"&&store.Current().confirmed.seq==0,"old boot cannot corrupt cursor");
    NativePersistence::Store reload;Check(reload.Load(),"outbox reload");Check(reload.Current().pending.size()==1&&reload.Current().pending[0].outboxId==a2.outboxId,"unconfirmed replay");
    Check(reload.ConfirmThrough(a2.outboxId),"confirm current");Check(reload.Current().confirmed.seq==1,"current cursor");
    auto batch=reload.AdmitMany({{{"boot-b",2},f},{{"boot-b",3},f}});
    Check(batch&&batch.outboxIds.size()==2&&batch.outboxIds[0]<batch.outboxIds[1]&&reload.Current().scan.seq==3,"atomic ordered batch admission");
    Check(reload.ConfirmThrough(batch.outboxIds.back()),"batch confirmation");
    auto oldest=reload.Admit({"boot-b",4},f);Check(oldest,"oldest durable event admitted");
    auto budget=reload.AdmitMany({{{"boot-b",5},f}},32u*1024u*1024u-f->size(),0);
    Check(budget&&reload.Current().pending.size()==1&&
          reload.Current().pending[0].outboxId==budget.outboxIds[0]&&
          reload.Current().deliveryLosses>0,"external raw staging evicts oldest durable frame and records loss");
    Check(reload.ConfirmThrough(budget.outboxIds[0]),"budget event confirmed");
    const auto lossesBeforeSkip=reload.Current().deliveryLosses;
    Check(reload.Skip({"boot-b",6})&&reload.Current().confirmed.seq==6&&
          reload.Current().deliveryLosses==lossesBeforeSkip,"suppressed connection advances cursor without loss");
    NativePersistence::TestDirectorySyncFailure([](const std::string& path){return path.find("event-outbox.json")!=std::string::npos;});
    auto uncertain=reload.Admit({"boot-b",7},f);
    Check(uncertain&&!reload.EventOutboxDurable()&&reload.Current().scan.seq==7&&
          reload.Current().pending.back().outboxId==uncertain.outboxId,
          "visible post-rename failure advances memory and fences delivery");
    NativePersistence::TestDirectorySyncFailure({});
    Check(reload.RetryDurability()&&reload.EventOutboxDurable(),"directory fsync retry releases durability fence");
    Check(reload.ConfirmThrough(uncertain.outboxId),"post-rename event confirms after durability retry");
    {std::ofstream blank(store.Files().timedBans);}
    std::string timed;auto emptyTimed=reload.ReadLegacy("timedBans",timed);
    Check(!emptyTimed&&reload.LastError().find("empty existing")!=std::string::npos,"empty existing timed store is corrupt");
    {std::ofstream bad(store.Files().timedBans);bad<<R"([{"gameId":"76561198000000001","expiresAt":"garbage"}])";}
    Check(!reload.ReadLegacy("timedBans",timed),"invalid expiry blocks timed-ban migration");
    {std::ofstream bad(store.Files().timedBans);bad<<"{broken";}
    NativeBehavior::Engine cold(reload);auto loaded=cold.Load();Check(!loaded&&loaded.error.find("timedBans")!=std::string::npos,"corrupt timed ban explicit");
    NativeLog::Parser logs;std::string key,detail;Check(logs.Configure(key,detail),"default PCRE2 grammar");
    auto parsed=logs.Feed("LogVeinChat: [76561198000000001] A (aka Char): hello Ticket=abcd ?p=secret");
    Check(!parsed.empty()&&parsed[0].type=="log"&&parsed[0].dataJson.find("secret")==std::string::npos&&parsed[0].dataJson.find("abcd")==std::string::npos,"log redaction");
    setenv("VEIN_LOG_CHAT_RE",R"(LogVeinChat:\s*\[(?<gameId>7656\d{13})\]\s*(?<name>.+?)(?:\s*\(aka (?<character>[^)]*)\))?:\s?(?<msg>.*)$)",1);
    NativeLog::Parser compatible;Check(compatible.Configure(key,detail)&&!compatible.CustomChat(),"compatible custom PCRE2 capture without duplicate chat ownership");
    auto chat=compatible.Feed("LogVeinChat: [76561198000000001] A (aka Char): hello world");
    Check(chat.size()==1&&chat[0].type=="log"&&!compatible.CustomChat(),
          "custom chat grammar observes but does not duplicate game hook chat");
    setenv("VEIN_LOG_READY_RE",R"(Heartbeat ready)",1);
    NativeLog::Parser customReady;Check(customReady.Configure(key,detail),"legitimate custom ready expression may differ from default fixture");
    setenv("VEIN_LOG_TAIL","NeVeR",1);setenv("VEIN_LOG_EVENTS","FiLtErEd",1);
    NativeLog::Parser never;Check(never.Configure(key,detail)&&!never.CustomJoin()&&!never.CustomChat(),"case-insensitive tail/log config retains fallback ownership");
    setenv("VEIN_LOG_TAIL","ALWAYS",1);NativeLog::Parser always;Check(always.Configure(key,detail)&&always.TailConnections(),"tail always owns connections");
    auto joined=always.Feed("LogVein: PlayerState ID changed to 76561198000000001");
    joined=always.Feed("LogVein: [] Player SteamPersona selected character 0123456789abcdef0123456789abcdef");
    Check(std::any_of(joined.begin(),joined.end(),[](const auto& row){return row.type=="player-connected";}),
          "native tail emits a connection when enabled");
    unsetenv("VEIN_LOG_TAIL");unsetenv("VEIN_LOG_EVENTS");unsetenv("VEIN_LOG_READY_RE");
    setenv("VEIN_LOG_CHAT_RE","(?:bad)",1);NativeLog::Parser incompatible;Check(!incompatible.Configure(key,detail)&&key=="VEIN_LOG_CHAT_RE","custom regex fixture blocker");
    unsetenv("VEIN_LOG_CHAT_RE");
    setenv("TAKARO_CURSOR_FILE",(fs::path(path)/"old"/"event-cursor.json").c_str(),1);
    auto paths=NativePersistence::ResolvePaths();Check(paths.online==(fs::path(path)/"old"/"online-players.json").string(),"explicit cursor preserves legacy sibling paths");
    unsetenv("TAKARO_CURSOR_FILE");
    fs::path expanded=fs::path(path)/"expanded";fs::create_directories(expanded);
    setenv("TAKARO_STATE_DIR",expanded.c_str(),1);
    NativePersistence::Store large;Check(large.Load(),"large snapshot initial load");
    {std::ofstream stale(large.Files().outbox+".tmp.ABCDEF");stale<<"stale";}
    Json largeEvent={{"type","gameEvent"},{"payload",{{"type","log"},{"data",{{"msg",""}}}}}};
    const size_t base=largeEvent.dump().size();
    largeEvent["payload"]["data"]["msg"]=std::string((32u*1024u*1024u-base)/2,'"');
    auto escaped=std::make_shared<const std::string>(largeEvent.dump());
    Check(escaped->size()<=32u*1024u*1024u,"logical event stays within 32 MiB");
    Check(large.AdmitSynthetic(escaped),"near-32 MiB logical event admitted");
    Check(fs::file_size(large.Files().outbox)>64u*1024u*1024u,"JSON escaping exceeds former 64 MiB read cap");
    NativePersistence::Store largeReload;Check(largeReload.Load(),"escaped 32 MiB-budget snapshot reloads");
    Check(largeReload.Current().pending.size()==1&&largeReload.Current().pending[0].frame->size()==escaped->size(),"escaped frame preserved exactly");
    {std::ifstream stale(large.Files().outbox+".tmp.ABCDEF");std::string value;stale>>value;Check(value=="stale","atomic writer avoids stale PID/temp collisions");}
    fs::path invalid=fs::path(expanded)/"invalid";fs::create_directories(invalid);
    setenv("TAKARO_STATE_DIR",invalid.c_str(),1);
    {std::ofstream corrupt(invalid/"event-outbox.json");corrupt<<Json{{"version",1},{"nextOutboxId",2},
      {"scan",{{"bootId","x"},{"seq",1}}},{"confirmed",{{"bootId","x"},{"seq",0}}},
      {"deliveryLosses",0},{"pending",Json::array({Json{{"outboxId",1},{"source",{{"bootId","x"},{"seq",1}}},{"frame","{}"}}})}}.dump();}
    NativePersistence::Store corruptReload;Check(!corruptReload.Load()&&corruptReload.LastError().find("invalid durable event frame")!=std::string::npos,
      "invalid persisted game event blocks replay instead of being confirmed and discarded");
    {std::ofstream corrupt(invalid/"event-outbox.json");corrupt<<"{\"version\":1,\"pending\":[],\"padding\":"+
        std::string(64,'[')+"0"+std::string(64,']')+"}";}
    NativePersistence::Store deepReload;Check(!deepReload.Load()&&deepReload.LastError().find("depth 64")!=std::string::npos,
      "nested persisted JSON over depth 64 degrades without parser recursion");
    // A complete sidecar state set must load without renaming its four files.
    const fs::path legacy=fs::path(path)/"legacy-all";fs::create_directories(legacy);
    const std::string legacyTimed=R"([{"gameId":"76561198000000001","expiresAt":"2099-01-01T00:00:00Z","reason":"old sidecar"}])";
    {std::ofstream f(legacy/"event-cursor.json");f<<R"({"bootId":"legacy-boot","seq":17})";}
    {std::ofstream f(legacy/"online-players.json");f<<R"([{"gameId":"76561198000000001","name":"Legacy online"}])";}
    {std::ofstream f(legacy/"known-players.json");f<<R"([{"gameId":"76561198000000001","name":"Legacy known"}])";}
    {std::ofstream f(legacy/"timed-bans.json");f<<legacyTimed;}
    bans={{id,"","old sidecar","",""}};
    setenv("TAKARO_STATE_DIR",legacy.c_str(),1);
    NativePersistence::Store legacyStore;Check(legacyStore.Load(),"all four actual legacy filenames load");
    Check(legacyStore.Current().scan.bootId=="legacy-boot"&&legacyStore.Current().scan.seq==17&&
          Parse(legacyStore.Current().derivedOnlineJson)[0]["name"]=="Legacy online"&&
          Parse(legacyStore.Current().derivedKnownJson)[0]["name"]=="Legacy known",
          "legacy cursor, online and known formats retained");
    NativeBehavior::Engine legacyEngine(legacyStore);Check(legacyEngine.Load(),"legacy timed file loads with enforcement");
    Check(bans[0].expiresAt=="2099-01-01T00:00:00Z"&&
          Parse(legacyEngine.Snapshot()->timedBansJson)[0]["reason"]=="old sidecar",
          "populated legacy timed file hydrates exact expiry and reason");
    // Explicit file paths win even when the new outbox lives elsewhere.
    const fs::path alternate=fs::path(path)/"individual-overrides",stateRoot=fs::path(path)/"override-outbox";
    fs::create_directories(alternate);fs::create_directories(stateRoot);
    const fs::path cursorFile=alternate/"cursor.special",onlineFile=alternate/"online.special",
                   knownFile=alternate/"known.special",timedFile=alternate/"timed.special";
    {std::ofstream f(cursorFile);f<<R"({"bootId":"override-boot","seq":23})";}
    {std::ofstream f(onlineFile);f<<R"([{"gameId":"76561198000000001","name":"Override online"}])";}
    {std::ofstream f(knownFile);f<<R"([{"gameId":"76561198000000001","name":"Override known"}])";}
    {std::ofstream f(timedFile);f<<legacyTimed;}
    setenv("TAKARO_STATE_DIR",stateRoot.c_str(),1);
    setenv("TAKARO_CURSOR_FILE",cursorFile.c_str(),1);
    setenv("TAKARO_ONLINE_FILE",onlineFile.c_str(),1);
    setenv("TAKARO_KNOWN_PLAYERS_FILE",knownFile.c_str(),1);
    setenv("TAKARO_BAN_FILE",timedFile.c_str(),1);
    const auto explicitPaths=NativePersistence::ResolvePaths();
    Check(explicitPaths.cursor==cursorFile.string()&&explicitPaths.online==onlineFile.string()&&
          explicitPaths.knownPlayers==knownFile.string()&&explicitPaths.timedBans==timedFile.string()&&
          explicitPaths.outbox==(stateRoot/"event-outbox.json").string(),
          "all explicit legacy file paths take precedence over state directory");
    NativePersistence::Store overrideStore;Check(overrideStore.Load(),"explicit legacy paths read");
    Check(overrideStore.Current().scan.bootId=="override-boot"&&overrideStore.Current().scan.seq==23&&
          Parse(overrideStore.Current().derivedOnlineJson)[0]["name"]=="Override online"&&
          Parse(overrideStore.Current().derivedKnownJson)[0]["name"]=="Override known",
          "all configured legacy files were actually read");
    std::string overrideTimed;Check(overrideStore.ReadLegacy("timedBans",overrideTimed)&&overrideTimed==legacyTimed,
          "explicit timed-ban path was actually read");
    unsetenv("TAKARO_CURSOR_FILE");unsetenv("TAKARO_ONLINE_FILE");
    unsetenv("TAKARO_KNOWN_PLAYERS_FILE");unsetenv("TAKARO_BAN_FILE");
    // An external timed ban B can land between native A's game mutation and
    // ApplyOutcome. ApplyOutcome copies the global revision but only A's row;
    // Sync must compare the complete snapshot, then schedule B at its expiry.
    const fs::path raceRoot=fs::path(path)/"cross-player-ban-race";fs::create_directories(raceRoot);
    setenv("TAKARO_STATE_DIR",raceRoot.c_str(),1);
    bans.clear();++banRevision;
    NativePersistence::Store raceStore;Check(raceStore.Load(),"race store load");
    NativeBehavior::Engine raceEngine(raceStore);Check(raceEngine.Load(),"race engine load");
    auto nativeA=raceEngine.PrepareAction("race-A","banPlayer",
        Json{{"gameId",id},{"reason","native A"},{"expiresAt","2099-01-01T00:00:00Z"}}.dump(),1);
    nativeA.canonicalBanId=id;
    Check(raceEngine.BeforeExecute(nativeA),"race A durable ban intent");
    auto nativeAOutcome=NativeBehavior::Engine::ExecuteAction(nativeA);
    const std::string externalB="76561198000000002";
    Check(state::BanAdd({externalB,"B","external timed B","","2030-01-01T00:00:00Z"}),
          "external B arrives before native A completion");
    Check(raceEngine.ApplyOutcome(nativeA,std::move(nativeAOutcome)).errorText.empty(),"native A apply");
    Check(raceEngine.Snapshot()->banRevision==state::BanRevision()&&
          Parse(raceEngine.Snapshot()->timedBansJson).size()==1,
          "native A completion tagged global revision but omitted external B");
    Check(raceEngine.SyncBanMetadata(),"full ban-state synchronization despite matching revision");
    auto synchronized=Parse(raceEngine.Snapshot()->timedBansJson);
    Check(synchronized.size()==2&&std::any_of(synchronized.begin(),synchronized.end(),
          [&](const Json& row){return row.value("gameId",std::string())==externalB&&
                                     row.value("expiresAt",std::string())=="2030-01-01T00:00:00Z";}),
          "external B timed metadata admitted despite revision shortcut collision");
    auto dueB=raceEngine.DueTimedBans(2000000000000LL);
    Check(std::any_of(dueB.begin(),dueB.end(),[&](const NativeBehavior::PreparedAction& action){
          return action.internalExpiry&&action.expiryPlayer==externalB;}),
          "external B expiry is scheduled without restart");
    setenv("TAKARO_STATE_DIR",path,1);
    fs::remove_all(path);std::cout<<"native behavior parity/persistence/PCRE2 PASS\n";
}
