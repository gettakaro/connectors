#include "native_behavior.h"
#include "native_log.h"
#include "actions.h"
#include "actions_util.h"
#include "common.h"
#include "gamethread.h"
#include "reflect.h"
#include "state.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <map>
#include <set>
#include <stdexcept>

namespace NativeBehavior {
namespace {
using Json=nlohmann::json;
Json Object(const Json& j){return j.is_object()?j:Json::object();}
Json Array(const Json& j){return j.is_array()?j:Json::array();}
std::string Trim(std::string s){auto b=s.find_first_not_of(" \t\r\n");if(b==std::string::npos)return {};auto e=s.find_last_not_of(" \t\r\n");return s.substr(b,e-b+1);}
std::string Str(const Json& j,const char* k){auto p=j.find(k);if(p==j.end())return {}; if(p->is_string())return Trim(p->get<std::string>()); if(p->is_number())return p->dump(); return {};}
std::string Lower(std::string s){std::transform(s.begin(),s.end(),s.begin(),[](unsigned char c){return std::tolower(c);});return s;}
std::string StripPlatform(std::string s){if(Lower(s).rfind("steam:",0)==0)s.erase(0,6);return s;}
bool SteamId(const std::string& s){return s.size()==17&&s.rfind("7656",0)==0&&std::all_of(s.begin(),s.end(),[](unsigned char c){return std::isdigit(c);});}
double Num(const Json& j,const char* key,double def=0){auto p=j.find(key);if(p==j.end())return def; if(p->is_number()){double x=p->get<double>();return std::isfinite(x)?x:def;}if(p->is_string())try{size_t n=0;auto s=Trim(p->get<std::string>());double x=std::stod(s,&n);if(n==s.size()&&std::isfinite(x))return x;}catch(...){}return def;}
bool HasNum(const Json& j,const char* k){auto p=j.find(k);if(p==j.end())return false;if(p->is_number())return std::isfinite(p->get<double>());if(p->is_string()){try{size_t n=0;auto s=Trim(p->get<std::string>());double x=std::stod(s,&n);return n==s.size()&&std::isfinite(x);}catch(...){}}return false;}
Json Parse(const std::string& s){auto j=Json::parse(s,nullptr,false);return j.is_discarded()?Json():j;}
bool WithinDepth64(const std::string& s){
    unsigned depth=0;bool quoted=false,escaped=false;
    for(char c:s){
        if(quoted){if(escaped)escaped=false;else if(c=='\\')escaped=true;else if(c=='"')quoted=false;}
        else if(c=='"')quoted=true;
        else if(c=='{'||c=='['){if(++depth>64)return false;}
        else if(c=='}'||c==']'){if(depth==0)return false;--depth;}
    }
    return depth==0&&!quoted;
}
Json Player(const Json& raw){
    Json p=Object(raw);std::string steam=Str(p,"steamId");if(!SteamId(steam))steam=Str(p,"gameId");
    if(!SteamId(steam))steam=StripPlatform(Str(p,"platformId"));if(!SteamId(steam))steam.clear();
    std::string id=steam.empty()?Str(p,"gameId"):steam;if(id.empty())id=Str(p,"name");if(id.empty())id=Str(p,"characterName");
    if(id.empty())throw std::runtime_error("player has no identifier");
    Json out={{"gameId",id},{"name",Str(p,"name").empty()?(Str(p,"characterName").empty()?id:Str(p,"characterName")):Str(p,"name")}};
    if(!steam.empty()){out["steamId"]=steam;out["platformId"]="steam:"+steam;}
    if(!Str(p,"ip").empty())out["ip"]=Str(p,"ip");if(p.contains("ping")&&p["ping"].is_number())out["ping"]=p["ping"];
    return out;
}
Json Position(const Json& raw){Json p=Object(raw);if(!HasNum(p,"x")||!HasNum(p,"y")||!HasNum(p,"z"))throw std::runtime_error("invalid position");Json o={{"x",Num(p,"x")},{"y",Num(p,"y")},{"z",Num(p,"z")}};if(!Str(p,"dimension").empty())o["dimension"]=Str(p,"dimension");return o;}
std::string PlayerId(const Json& args){for(const Json& s:{Object(args),Object(args.value("player",Json::object())),Object(args.value("playerRef",Json::object()))})for(const char* k:{"gameId","steamId","platformId"})if(!Str(s,k).empty())return Str(s,k);throw std::runtime_error("Expected player identifier (gameId, or player.gameId)");}
Json Body(const Json& j){JsonValue v; if(!JsonParse(j.dump(),v))throw std::runtime_error("invalid action body");return j;}
JsonValue NativeBody(const Json& j){JsonValue v;if(!JsonParse(j.dump(),v))throw std::runtime_error("invalid action body");return v;}
Json ResultBody(const Actions::Result& r){Json j=Parse(r.body);if(r.status>=400){std::string msg=j.is_object()?Str(j,"error"):"";throw std::runtime_error(msg.empty()?"game action failed ("+std::to_string(r.status)+")":msg);}return j;}
Json Rows(const Json& raw,const char* key){if(raw.is_array())return raw;Json r=Object(raw);for(const char* k:{key,"data","items","players","bans","entities","locations"})if(r.contains(k)&&r[k].is_array())return r[k];return Json::array();}
std::string ResolveKnownId(const PreparedAction& p,const std::string& raw){
    const std::string bare=StripPlatform(raw),needle=Lower(bare);
    if(SteamId(bare))return bare;
    for(auto& row:Rows(Parse(p.view->knownPlayersJson),"players")){
        Json r=Object(row);
        for(const char* key:{"gameId","steamId","platformId","name","characterName"})
            if(Lower(StripPlatform(Str(r,key)))==needle)return StripPlatform(Str(r,"gameId"));
    }
    return bare;
}
std::string ResolveId(const PreparedAction& p,const Json& args){
    const std::string raw=PlayerId(args),known=ResolveKnownId(p,raw),needle=Lower(StripPlatform(raw));
    if(SteamId(known))return known;
    Actions::Result players=Actions::Players();
    Json rows=players.status<400?Parse(players.body):Json();
    if(!rows.is_array()){
        auto fallback=NativeTransport::FetchGamePlayers();
        if(fallback){auto normalized=Engine::NormalizeGamePlayers(*fallback);if(normalized)rows=Parse(*normalized);}
    }
    if(rows.is_array())for(auto& row:rows){
        Json r=Object(row);
        for(const char* key:{"gameId","steamId","platformId","name","characterName"})
            if(Lower(StripPlatform(Str(r,key)))==needle)return StripPlatform(Str(r,"gameId"));
    }
    return known;
}
std::string ConsoleBanId(const PreparedAction& p){
    if(p.action!="executeConsoleCommand")return {};
    const auto words=ActionsUtil::Words(Str(Object(Parse(p.argsJson)),"command"));
    if(words.size()<2)return {};
    const std::string verb=Lower(words.front());
    return verb=="ban"||verb=="unban"?ResolveKnownId(p,words[1]):std::string();
}
bool ConsoleUnban(const PreparedAction& p){
    if(p.action!="executeConsoleCommand")return false;
    const auto words=ActionsUtil::Words(Str(Object(Parse(p.argsJson)),"command"));
    return words.size()>=2&&Lower(words.front())=="unban";
}
bool UnbanIntent(const Json& raw){
    const Json j=Object(raw);
    const std::string mutation=Str(j,"mutation");
    return mutation=="unban" || (mutation.empty()&&Str(j,"action")=="unbanPlayer");
}
Json BanRecordJson(const state::BanRecord& b){
    return Json{{"gameId",b.gameId},{"name",b.name},{"reason",b.reason},
                {"createdAt",b.createdAt},{"expiresAt",b.expiresAt}};
}
bool SameBanRecord(const state::BanRecord& b,const Json& expected){
    return b.gameId==Str(expected,"gameId")&&b.expiresAt==Str(expected,"expiresAt")&&
           b.reason==Str(expected,"reason")&&b.name==Str(expected,"name")&&
           b.createdAt==Str(expected,"createdAt");
}
std::string IntentTarget(const Json& intent,const std::vector<state::BanRecord>& current){
    const std::string raw=Str(Object(intent),"gameId");
    if(raw.empty())throw std::runtime_error("corrupt ban intent: missing gameId");
    for(const auto& b:current)if(b.gameId==raw)return raw;
    if(!intent.contains("beforeBans")||!intent["beforeBans"].is_array())return raw;
    std::map<std::string,std::string> before,after;
    for(const auto& row:intent["beforeBans"])before[Str(Object(row),"gameId")]=Str(Object(row),"expiresAt");
    for(const auto& b:current)after[b.gameId]=b.expiresAt;
    std::vector<std::string> changed;
    for(const auto& kv:after)if(!before.count(kv.first)||before[kv.first]!=kv.second)changed.push_back(kv.first);
    for(const auto& kv:before)if(!after.count(kv.first))changed.push_back(kv.first);
    if(changed.size()==1)return changed.front();
    if(changed.size()>1)throw std::runtime_error("ambiguous ban intent recovery for "+raw);
    return raw;
}
Json Item(const Json& raw,bool definition){Json i=Object(raw);std::string code=Str(i,"code");if(code.empty())code=Str(i,"name");if(code.empty())throw std::runtime_error("item has no code");Json o={{"code",code},{"name",Str(i,"name").empty()?code:Str(i,"name")}};if(definition){if(!Str(i,"description").empty())o["description"]=Str(i,"description");}else{o["amount"]=Num(i,"amount",1);if(i.contains("quality")&&!i["quality"].is_null()&&i["quality"]!="")o["quality"]=i["quality"].is_string()?i["quality"]:Json(i["quality"].dump());}return o;}
Json Inventory(const Json& raw){Json out=Json::array();std::map<std::pair<std::string,std::string>,size_t> idx;for(auto& row:Rows(raw,"items")){Json i=Item(row,false);auto key=std::make_pair(i["code"].get<std::string>(),i.contains("quality")?i["quality"].get<std::string>():"");auto it=idx.find(key);if(it==idx.end()){idx[key]=out.size();out.push_back(i);}else out[it->second]["amount"]=out[it->second]["amount"].get<double>()+i["amount"].get<double>();}return out;}
Json Entity(const Json& raw){Json e=Object(raw);std::string code=Str(e,"code");if(code.empty())code=Str(e,"name");if(code.empty())throw std::runtime_error("entity has no code");std::string type=Lower(Str(e,"type")), mapped="neutral";for(const char* x:{"hostile","enemy","monster","aggressive","zombie","infected","undead","ghoul","raider"})if(type.find(x)!=std::string::npos)mapped="hostile";if(mapped=="neutral")for(const char* x:{"friendly","ally","npc","villager","survivor","companion","pet","merchant","trader"})if(type.find(x)!=std::string::npos)mapped="friendly";Json o={{"code",code},{"name",Str(e,"name").empty()?code:Str(e,"name")},{"type",mapped}};if(!Str(e,"description").empty())o["description"]=Str(e,"description");return o;}
Json Location(const Json& raw){Json l=Object(raw);std::string code=Str(l,"code");if(code.empty())code=Str(l,"name");if(code.empty())throw std::runtime_error("location has no code");Json o={{"code",code},{"name",Str(l,"name").empty()?code:Str(l,"name")},{"position",Position(l.value("position",l))}};for(const char* k:{"radius","sizeX","sizeY","sizeZ"})if(HasNum(l,k))o[k]=Num(l,k);return o;}
Json Ban(const Json& raw){Json b=Object(raw),p=Object(b.value("player",Json::object()));if(p.empty())p=b;Json o={{"player",Player(p)},{"reason",Str(b,"reason")},{"expiresAt",nullptr}};if(b.contains("expiresAt")&&!b["expiresAt"].is_null())o["expiresAt"]=b["expiresAt"];return o;}
std::string Channel(std::string c){c=Lower(c);if(c=="local"||c=="radio")return "team";if(c=="team"||c=="friends"||c=="whisper")return c;return "global";}
Json MapEventData(const std::string& type,const Json& raw){
    Json d=Object(raw),out=Json::object(); auto withPlayer=[&](){Json p=Object(d.value("player",Json::object()));out["player"]=Player(p.empty()?d:p);};
    if(type=="player-connected"||type=="player-disconnected")withPlayer();
    else if(type=="chat-message"){out["msg"]=Str(d,"msg");if(out["msg"]=="")out["msg"]=Str(d,"message");if(out["msg"]=="")out["msg"]=Str(d,"text");out["channel"]=Channel(Str(d,"channel"));Json p=Object(d.value("player",Json::object()));if(!p.empty())out["player"]=Player(p);}
    else if(type=="player-death"){withPlayer();Json a=Object(d.value("attacker",Json::object()));if(!a.empty())try{out["attacker"]=Player(a);}catch(...){}if(d.contains("position")&&!d["position"].is_null())out["position"]=Position(d["position"]);if(!out.contains("attacker")){std::string name=Str(out["player"],"name"), killer=Str(d,"killerEntity");if(killer.empty())killer=Str(Object(d.value("killer",Json::object())),"code");out["msg"]=killer.empty()?(name+" died"+(Str(d,"cause").empty()?"":" ("+Str(d,"cause")+")")):(name+" was killed by "+killer);}}
    else if(type=="entity-killed"){withPlayer();out["entity"]=Str(d,"entity");if(out["entity"]=="")out["entity"]=Str(Object(d.value("entity",Json::object())),"code");if(out["entity"]=="")out["entity"]="unknown";out["weapon"]=Str(d,"weapon");}
    else if(type=="log")out["msg"]=raw.is_string()?raw.get<std::string>():(!Str(d,"msg").empty()?Str(d,"msg"):(!Str(d,"message").empty()?Str(d,"message"):(!Str(d,"line").empty()?Str(d,"line"):raw.dump())));
    else throw std::runtime_error("unknown event type: "+type);
    return out;
}
std::string IsoMs(int64_t ms){
    const time_t seconds=static_cast<time_t>(ms/1000);
    tm t{};if(!gmtime_r(&seconds,&t))throw std::runtime_error("date is outside supported range");
    char b[48];const int year=t.tm_year+1900;
    const int written=year<=9999?
        snprintf(b,sizeof b,"%04d-%02d-%02dT%02d:%02d:%02d",year,t.tm_mon+1,t.tm_mday,t.tm_hour,t.tm_min,t.tm_sec):
        snprintf(b,sizeof b,"+%06d-%02d-%02dT%02d:%02d:%02d",year,t.tm_mon+1,t.tm_mday,t.tm_hour,t.tm_min,t.tm_sec);
    if(written<0||static_cast<size_t>(written)>=sizeof b)throw std::runtime_error("date is outside supported range");
    char sub[8];snprintf(sub,sizeof sub,".%03lldZ",(long long)(ms%1000));return std::string(b)+sub;
}
int64_t DateMs(const Json& v){
    if(v.is_number()){
        const double value=v.get<double>();
        if(!std::isfinite(value)||value<=0)return 0;
        if(value>8.64e15)throw std::runtime_error("date exceeds JavaScript Date range");
        return static_cast<int64_t>(value);
    }
    if(!v.is_string())return 0;
    const std::string s=v.get<std::string>();tm t{};char* end=nullptr;
    if(s.size()>8&&s[0]=='+'&&s[7]=='-'){
        try{t.tm_year=std::stoi(s.substr(1,6))-1900;}catch(...){return 0;}
        end=strptime(s.c_str()+8,"%m-%dT%H:%M:%S",&t);
    }else end=strptime(s.c_str(),"%Y-%m-%dT%H:%M:%S",&t);
    if(!end)return 0;
    int64_t ms=static_cast<int64_t>(timegm(&t))*1000;
    if(*end=='.'){
        ++end;int digits=0,fraction=0;
        while(std::isdigit(static_cast<unsigned char>(*end))){if(digits<3)fraction=fraction*10+(*end-'0');++digits;++end;}
        if(digits==0)return 0;
        while(digits++<3)fraction*=10;
        ms+=fraction;
    }
    if(*end=='+'||*end=='-'){
        int sign=*end=='+'?1:-1; ++end;
        if(!std::isdigit(static_cast<unsigned char>(end[0]))||!std::isdigit(static_cast<unsigned char>(end[1])))return 0;
        int hours=(end[0]-'0')*10+end[1]-'0';end+=2;if(*end==':')++end;
        if(!std::isdigit(static_cast<unsigned char>(end[0]))||!std::isdigit(static_cast<unsigned char>(end[1])))return 0;
        int minutes=(end[0]-'0')*10+end[1]-'0';end+=2;
        if(hours>23||minutes>59)return 0;
        ms-=sign*(hours*60+minutes)*60000LL;
    }else if(*end=='Z'||*end=='z')++end;
    return *end?0:ms;
}
std::string Expiry(const Json& v){int64_t ms=DateMs(v);return ms>0?IsoMs(ms):"";}
int64_t WallMs(){using namespace std::chrono;return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();}
bool ShutdownCommand(std::string s){s=Lower(Trim(s));while(!s.empty()&&(s.back()==';'||std::isspace((unsigned char)s.back())))s.pop_back();return s=="shutdown";}
}

class LogState { public: NativeLog::Parser parser; };
Engine::Engine(NativePersistence::Store& store):store_(store),view_(std::make_shared<View>()),log_(std::make_unique<LogState>()){}
Engine::~Engine() = default;
std::optional<std::string> Engine::NormalizeGamePlayers(const std::string& rawJson){
    if(!WithinDepth64(rawJson))throw std::runtime_error("VEIN_HTTP_API players JSON exceeds depth 64 or is malformed");
    Json parsed=Parse(rawJson),list;
    if(parsed.is_array())list=std::move(parsed);
    else if(parsed.is_object())for(const char* key:{"players","Players","data"})
        if(parsed.contains(key)&&parsed[key].is_array()){list=parsed[key];break;}
    if(!list.is_array())return std::nullopt;
    Json normalized=Json::array();
    for(const auto& entry:list){
        const Json p=Object(entry);
        std::string id;
        for(const char* key:{"steamId","steamID","steamID64","SteamId","uniqueId","id","gameId"})
            if(!(id=Str(p,key)).empty())break;
        if(id.empty())continue;
        std::string name;
        for(const char* key:{"name","playerName","Name","persona"})
            if(!(name=Str(p,key)).empty())break;
        Json row={{"gameId",id},{"steamId",id},{"platformId","steam:"+id},{"online",true}};
        if(!name.empty())row["name"]=name;
        if(p.contains("ping")&&p["ping"].is_number()&&std::isfinite(p["ping"].get<double>()))row["ping"]=p["ping"];
        normalized.push_back(std::move(row));
    }
    return normalized.dump();
}
NativePersistence::Result Engine::Load(){
    auto v=std::make_shared<View>();std::string x;
    if(store_.Current().hasDerivedState){
        v->onlinePlayersJson=store_.Current().derivedOnlineJson;
        v->knownPlayersJson=store_.Current().derivedKnownJson;
        auto mirrored=store_.SaveLegacy("online",v->onlinePlayersJson);if(!mirrored)return mirrored;
        mirrored=store_.SaveLegacy("knownPlayers",v->knownPlayersJson);if(!mirrored)return mirrored;
    }else{
        auto r=store_.ReadLegacy("online",v->onlinePlayersJson);if(!r)return r;
        r=store_.ReadLegacy("knownPlayers",v->knownPlayersJson);if(!r)return r;
    }
    auto timed=store_.ReadLegacy("timedBans",x);if(!timed)return timed;v->timedBansJson=x;
    v->senderName=Trim(ConfigValue("TAKARO_SENDER_NAME","senderName",ConfigValue("TAKARO_SERVER_CHAT_NAME","serverChatName","")));
    v->serverName=ConfigValue("TAKARO_SERVER_NAME","serverName","Takaro Dev Vein");v->banRevision=state::BanRevision();
    std::string key,detail;if(!log_->parser.Configure(key,detail))return {false,key+": "+detail};
    std::string intent;auto r=store_.ReadBanIntent(intent);if(!r)return r;
    std::set<std::string> journalTargets;
    if(!intent.empty()){
        Json journal=Parse(intent);if(!journal.is_object()||!journal.contains("intents")||!journal["intents"].is_array())return {false,"corrupt ban intent journal"};
        // A game ban can survive a crash while the plugin's bans.json is still
        // old. Load has no verified view of VEIN's own list, so it must neither
        // derive an expiry from the plugin list nor retire the durable intent.
        // The serialized action worker verifies both lists after startup.
        for(const auto& j:journal["intents"]){
            const std::string id=Str(Object(j),"gameId");
            if(id.empty())return {false,"corrupt ban intent: missing gameId"};
            journalTargets.insert(id);
        }
    }
    // Legacy sidecar kept the expiry only in timed-bans.json. It called the
    // plugin ban endpoint without expiresAt, so bans.json still says permanent.
    // Hydrate that metadata before native expiry scheduling, but never let a
    // stale timed row overwrite a ban touched by an unfinished journal intent.
    const auto current=state::ReadBans();
    uint64_t derivedRevision=current.revision;
    if(!store_.Current().legacyBanMigrationDone){
        uint64_t hydratedCount=0;
        for(const auto& entry:Array(Parse(v->timedBansJson))){
            const Json timedRow=Object(entry);
            const std::string id=Str(timedRow,"gameId");
            if(journalTargets.count(id))continue;
            auto found=std::find_if(current.records.begin(),current.records.end(),
                [&](const state::BanRecord& ban){return ban.gameId==id;});
            if(found==current.records.end())return {false,"timed ban is missing from game enforcement bans: "+id};
            if(found->expiresAt.empty()){
                state::BanRecord merged=*found;
                merged.expiresAt=Str(timedRow,"expiresAt");
                if(merged.reason.empty())merged.reason=Str(timedRow,"reason");
                if(!state::BanAddIfRevision(merged,current.revision+hydratedCount))
                    return {false,"ban changed during legacy timed migration: "+id};
                ++hydratedCount;
            }else if(found->expiresAt!=Str(timedRow,"expiresAt")){
                return {false,"timed ban expiry conflicts with game enforcement store: "+id};
            }
        }
        if(hydratedCount&&!state::FlushBans())return {false,state::BanPersistenceError()};
        derivedRevision+=hydratedCount;
        auto marked=store_.MarkLegacyBanMigrationDone();if(!marked)return marked;
        if(!store_.EventOutboxDurable()){
            marked=store_.RetryDurability();if(!marked||!store_.EventOutboxDurable())
                return {false,"cannot durably mark completed legacy ban migration"};
        }
    }else{
        // Once native has completed migration, bans.json is authoritative for
        // current expiries. A stale timed-bans.json row must never resurrect a
        // timed ban after a newer permanent change through the HTTP endpoint.
        Json next=Json::array();
        for(const auto& row:Array(Parse(v->timedBansJson)))
            if(journalTargets.count(Str(Object(row),"gameId")))next.push_back(row);
        for(const auto& ban:current.records)
            if(!ban.expiresAt.empty()&&!journalTargets.count(ban.gameId))
                next.push_back({{"gameId",ban.gameId},{"expiresAt",ban.expiresAt},{"reason",ban.reason}});
        if(next.dump()!=v->timedBansJson){
            auto saved=store_.SaveLegacy("timedBans",next.dump());if(!saved)return saved;
            v->timedBansJson=next.dump();
        }
    }
    if(state::BanRevision()!=derivedRevision)
        return {false,"ban list changed while loading timed state; retry native initialization"};
    v->banRevision=derivedRevision;
    v->revision=++revision_;view_=std::move(v);return {true,{}};
}
PreparedAction Engine::PrepareAction(std::string id,std::string action,std::string args,uint64_t epoch){
    PreparedAction p;p.requestId=std::move(id);p.action=std::move(action);p.argsJson=std::move(args);p.epoch=epoch;p.revision=revision_;p.view=view_;
    Json a=Object(Parse(p.argsJson));if(p.action=="listBans"){
        if(!state::BanPersistenceError().empty())
            throw std::runtime_error("Vein connector cannot list bans: "+state::BanPersistenceError());
        std::string raw;auto r=store_.ReadLegacy("timedBans",raw);if(!r)throw std::runtime_error("Vein connector cannot list bans: "+r.error);
        auto v=std::make_shared<View>(*view_);v->timedBansJson=raw;p.view=v;
    }
    if(p.action=="testReachability"){
        auto v=std::make_shared<View>(*p.view);v->persistenceError=store_.LastError();p.view=v;
    }
    return p;
}
bool Engine::NeedsBanResolution(const PreparedAction& p){
    return !p.internalExpiry && (p.action=="banPlayer" || p.action=="unbanPlayer" || !ConsoleBanId(p).empty());
}
std::string Engine::ResolveBanTarget(const PreparedAction& p){
    if(p.internalExpiry)return p.expiryPlayer;
    if(!NeedsBanResolution(p))return {};
    const std::string consoleId=ConsoleBanId(p);
    const Json args=consoleId.empty()?Object(Parse(p.argsJson)):Json{{"gameId",consoleId}};
    const std::string id=ResolveId(p,args);
    if(id.empty())throw std::runtime_error("ban target has no canonical gameId");
    return id;
}
NativePersistence::Result Engine::BeforeExecute(const PreparedAction& p){
    const std::string consoleId=ConsoleBanId(p);
    if(p.action!="banPlayer"&&p.action!="unbanPlayer"&&!p.internalExpiry&&consoleId.empty())return {true,{}};
    const std::string id=p.internalExpiry?p.expiryPlayer:p.canonicalBanId;
    if(id.empty())return {false,"ban target must be resolved before journaling"};
    const auto current=state::ReadBans();
    if(p.internalExpiry&&current.revision!=p.expectedBanRevision)
        return {false,"ban changed before timed expiry"};
    Json before=Json::array(),beforeRecord=nullptr;
    for(const auto& b:current.records){
        before.push_back({{"gameId",b.gameId},{"expiresAt",b.expiresAt}});
        if(b.gameId==id)beforeRecord=BanRecordJson(b);
    }
    const auto words=p.action=="executeConsoleCommand"?ActionsUtil::Words(Str(Object(Parse(p.argsJson)),"command")):std::vector<std::string>{};
    const bool unban=p.internalExpiry||p.action=="unbanPlayer"||(!words.empty()&&Lower(words[0])=="unban");
    Json desired=nullptr;
    if(!unban){
        const Json args=Object(Parse(p.argsJson));
        std::string expiry;
        try{expiry=p.action=="banPlayer"?Expiry(args.value("expiresAt",Json())):std::string();}
        catch(const std::exception& e){return {false,e.what()};}
        const std::string reason=p.action=="banPlayer"?Str(args,"reason"):
            ActionsUtil::Rest(Str(args,"command"),2);
        desired=Json{{"gameId",id},{"reason",reason},{"expiresAt",expiry}};
    }
    return store_.BeginBanIntent(Json{{"version",1},{"requestId",p.requestId},{"gameId",id},{"action",p.action},
                                      {"mutation",unban?"unban":"ban"},{"revision",current.revision},
                                      {"beforeBans",before},{"beforeRecord",beforeRecord},{"desired",desired}}.dump());
}
ActionOutcome Engine::ExecuteAction(const PreparedAction& p){
    ActionOutcome out;Json a=Object(Parse(p.argsJson));auto call=[&](const Actions::Result& r){return ResultBody(r);};
    try{
        std::string action=p.action,id;
        if(p.internalExpiry&&(state::BanRevision()!=p.expectedBanRevision||Actions::PendingBanJobs()!=0)){out.payloadJson="{}";return out;}
        if(action=="testReachability"){
            Json caps=Object(Parse(PluginState::Get().CapabilitiesJson()));
            std::vector<std::string> degraded;
            for(auto it=caps.begin();it!=caps.end();++it){
                const std::string status=it.value().is_string()?it.value().get<std::string>():"unknown";
                if(status!="ok"&&status!="unimplemented")degraded.push_back(it.key()+"="+status);
            }
            std::string capText;
            for(const auto& cap:degraded){if(!capText.empty())capText+=", ";capText+=cap;}
            if(!capText.empty())capText="capabilities not ok: "+capText;
            const bool healthDegraded=!GameThread::Alive()||!Reflect::Validated()||
                PluginState::Get().Capability("reflection")!="ok"||
                !p.view->persistenceError.empty()||!state::BanPersistenceError().empty();
            std::string reason=healthDegraded?"Vein plugin degraded":"";
            if(!capText.empty()){if(!reason.empty())reason+="; ";reason+=capText;}
            if(reason.empty()&&healthDegraded)reason="Vein plugin degraded";
            out.payloadJson=Json{{"connectable",true},{"reason",reason.empty()?Json(nullptr):Json(reason)}}.dump();
        }
        else if(action=="getPlayers"){
            Actions::Result raw=Actions::Players();
            Json rows=raw.status<400?Parse(raw.body):Json();
            if(!rows.is_array()){
                auto fallback=NativeTransport::FetchGamePlayers();
                if(fallback){auto normalized=NormalizeGamePlayers(*fallback);if(normalized)rows=Parse(*normalized);}
            }
            if(!rows.is_array())rows=raw.status<400?Json::array():Rows(call(raw),"players");
            Json mapped=Json::array();for(auto& r:rows){if(Object(r).value("online",true)!=false){Json x=Player(r);mapped.push_back(x);out.effects.push_back({Effect::Kind::RememberPlayer,x.dump()});}}out.payloadJson=mapped.dump();
        }else if(action=="getPlayer"){
            id=PlayerId(a);Json found;
            Actions::Result online=Actions::Players();
            Json onlineRows=online.status<400?Parse(online.body):Json();
            if(!onlineRows.is_array()){
                auto fallback=NativeTransport::FetchGamePlayers();
                if(fallback){auto normalized=NormalizeGamePlayers(*fallback);if(normalized)onlineRows=Parse(*normalized);}
            }
            if(onlineRows.is_array())for(const auto& row:onlineRows){Json candidate=Player(row);
                for(const char* key:{"gameId","steamId","platformId","name","characterName"})
                    if(Lower(StripPlatform(Str(Object(row),key)))==Lower(StripPlatform(id))||
                       Lower(StripPlatform(Str(candidate,key)))==Lower(StripPlatform(id))){found=candidate;break;}
                if(!found.is_null())break;
            }
            if(!found.is_null())out.effects.push_back({Effect::Kind::RememberPlayer,found.dump()});
            else {
                auto r=Actions::Player(ResolveKnownId(p,id));
                if(r.status<400){found=Player(Parse(r.body));out.effects.push_back({Effect::Kind::RememberPlayer,found.dump()});}
                else if(r.status==404){for(auto& x:Array(Parse(p.view->knownPlayersJson))) {Json row=Object(x);for(const char* k:{"gameId","steamId","platformId","name","characterName"})if(Lower(Str(row,k))==Lower(id)||Lower(Str(row,k))==Lower(StripPlatform(id))){found=row;break;}if(!found.is_null())break;}if(found.is_null())found=Player(Json{{"gameId",StripPlatform(id)}});found["online"]=false;}
                else found=call(r);
            }
            out.payloadJson=found.dump();
        }else if(action=="getPlayerLocation"){
            id=ResolveId(p,a);Actions::Result raw=Actions::PlayerLocation(id);
            if(raw.status<400){Json position=Position(Parse(raw.body));out.payloadJson=position.dump();out.effects.push_back({Effect::Kind::RememberLocation,Json{{"gameId",id},{"position",position}}.dump()});}
            else {auto it=p.view->locationWindowUntilMs.find(id);if(it==p.view->locationWindowUntilMs.end()||it->second<WallMs())call(raw);
                auto pos=p.view->lastLocationsJson.find(id);out.payloadJson=pos==p.view->lastLocationsJson.end()?R"({"x":0,"y":0,"z":0})":pos->second;}
        }
        else if(action=="getPlayerInventory")out.payloadJson=Inventory(call(Actions::PlayerInventory(ResolveId(p,a)))).dump();
        else if(action=="giveItem"){
            id=ResolveId(p,a);Json item=Object(a.value("item",Json::object()));std::string code=Str(a,"item");if(code.empty())code=Str(a,"itemCode");if(code.empty())code=Str(a,"code");if(code.empty())code=Str(item,"code");if(code.empty())throw std::runtime_error("giveItem requires 'item'");double amount=HasNum(a,"amount")?Num(a,"amount"):Num(a,"quantity",1);if(amount<=0)throw std::runtime_error("giveItem amount must be positive");call(Actions::Give(NativeBody(Json{{"gameId",id},{"code",code},{"amount",amount}})));out.payloadJson="{}";
        }else if(action=="listItems"){
            Json mapped=Json::array();for(auto& row:Rows(call(Actions::Items(Str(a,"search"))),"items"))mapped.push_back(Item(row,true));out.payloadJson=mapped.dump();
        }else if(action=="listEntities"){
            Json mapped=Json::array();for(auto& row:Rows(call(Actions::Entities()),"entities"))mapped.push_back(Entity(row));out.payloadJson=mapped.dump();
        }else if(action=="listLocations"){
            Json mapped=Json::array();for(auto& row:Rows(call(Actions::Locations()),"locations"))mapped.push_back(Location(row));out.payloadJson=mapped.dump();
        }else if(action=="executeConsoleCommand"){
            std::string cmd=Str(a,"command");if(cmd.empty())throw std::runtime_error("executeConsoleCommand requires 'command'");
            if(ShutdownCommand(cmd)){out.payloadJson=Json{{"success",true},{"rawResult","saving, then SIGTERM"},{"errorMessage",nullptr}}.dump();out.deferredShutdown=true;}
            else {const auto words=ActionsUtil::Words(cmd);
                if(!ConsoleBanId(p).empty()){
                    if(p.canonicalBanId.empty())throw std::runtime_error("ban target was not resolved");
                    cmd=words[0]+" "+p.canonicalBanId;
                    const std::string tail=ActionsUtil::Rest(Str(a,"command"),2);
                    if(!tail.empty())cmd+=" "+tail;
                }
                Actions::Result r=Actions::Command(NativeBody(Json{{"command",cmd}}));
                if(!ConsoleBanId(p).empty()&&r.status>=400)out.mutationVerified=false;
                if(!ConsoleBanId(p).empty()){
                    out.effects.push_back({Effect::Kind::BanChanged,Json{{"gameId",p.canonicalBanId}}.dump()});
                }
                Json body=Parse(r.body);if(r.status>=400&&r.status<500)out.payloadJson=Json{{"success",false},{"rawResult",""},{"errorMessage",Str(body,"error").empty()?r.body:Str(body,"error")}}.dump();else{body=call(r);bool success=body.is_object()?body.value("success",true):true;if(!ConsoleBanId(p).empty()&&!success)out.mutationVerified=false;std::string text=Str(Object(body),"output");out.payloadJson=Json{{"success",success},{"rawResult",text},{"errorMessage",success?Json(nullptr):Json(text.empty()?"Command failed":text)}}.dump();}}
        }else if(action=="sendMessage"){
            std::string msg=Str(a,"message");if(msg.empty())throw std::runtime_error("sendMessage requires 'message'");Json opts=Object(a.value("opts",Json::object())),recipient=Object(opts.value("recipient",Json::object()));std::string sender=Str(opts,"senderNameOverride");if(sender.empty())sender=p.view->senderName;if(sender.empty())sender=p.view->serverName;if(sender.empty())sender="Server";std::string to=Str(recipient,"gameId");if(to.empty())to=Str(recipient,"steamId");if(to.empty())to=Str(recipient,"platformId");if(to.empty())to=Str(a,"recipientGameId");if(!to.empty())to=ResolveId(p,Json{{"gameId",to}});call(Actions::Message(NativeBody(Json{{"text",msg},{"recipientGameId",to},{"senderName",sender}})));out.payloadJson="{}";
        }else if(action=="teleportPlayer"){
            id=ResolveId(p,a);Json b={{"gameId",id}};std::string target=Str(a,"target");if(!target.empty())b["target"]=target;else{if(!HasNum(a,"x")||!HasNum(a,"y")||!HasNum(a,"z"))throw std::runtime_error("teleportPlayer requires numeric x, y, z");b["x"]=Num(a,"x");b["y"]=Num(a,"y");b["z"]=Num(a,"z");}if(HasNum(a,"yaw"))b["yaw"]=Num(a,"yaw");call(Actions::Teleport(NativeBody(b)));out.payloadJson="{}";
        }else if(action=="kickPlayer"){
            Json b={{"gameId",ResolveId(p,a)}};if(!Str(a,"reason").empty())b["reason"]=Str(a,"reason");call(Actions::Kick(NativeBody(b)));out.payloadJson="{}";
        }else if(action=="banPlayer"){
            id=p.canonicalBanId;if(id.empty())throw std::runtime_error("ban target was not resolved");Json b={{"gameId",id},{"reason",Str(a,"reason")}};std::string expiry=Expiry(a.value("expiresAt",Json()));if(!expiry.empty())b["expiresAt"]=expiry;Actions::Result r=Actions::Ban(NativeBody(b));Json answer=Parse(r.body);std::string actual=Str(Object(answer),"gameId");if(!actual.empty())id=actual;out.effects.push_back({Effect::Kind::BanChanged,Json{{"gameId",id}}.dump()});call(r);out.payloadJson="{}";
        }else if(action=="unbanPlayer"||p.internalExpiry){
            id=p.internalExpiry?p.expiryPlayer:p.canonicalBanId;if(id.empty())throw std::runtime_error("unban target was not resolved");
            const JsonValue body=NativeBody(Json{{"gameId",id}});
            Actions::Result r=p.internalExpiry?Actions::UnbanIfRevision(body,p.expectedBanRevision):Actions::Unban(body);
            Json answer=Parse(r.body);std::string actual=Str(Object(answer),"gameId");if(!actual.empty())id=actual;out.effects.push_back({Effect::Kind::BanChanged,Json{{"gameId",id}}.dump()});call(r);out.payloadJson="{}";
        }else if(action=="listBans"){
            Json pending=Array(Parse(p.view->timedBansJson)),mapped=Json::array();std::map<std::string,Json> byId;for(auto& b:pending)byId[Str(Object(b),"gameId")]=b;
            for(auto& row:Rows(call(Actions::Bans()),"bans")){Json b=Ban(row);std::string key=Str(b["player"],"gameId");auto it=byId.find(key);if(it!=byId.end()){b["expiresAt"]=it->second.value("expiresAt",Json());if(Str(b,"reason").empty())b["reason"]=Str(it->second,"reason");byId.erase(it);}mapped.push_back(b);}
            for(auto& kv:byId)mapped.push_back(Json{{"player",Player(Json{{"gameId",kv.first}})},{"reason",Str(kv.second,"reason")},{"expiresAt",kv.second.value("expiresAt",Json())}});out.payloadJson=mapped.dump();
        }else if(action=="shutdown"){out.payloadJson="{}";out.deferredShutdown=true;}
        else throw std::runtime_error("Unknown Takaro action '"+action+"'");
    }catch(const std::exception& e){out.errorText=e.what();}
    return out;
}
ApplyResult Engine::ApplyOutcome(const PreparedAction& p,ActionOutcome out){
    ApplyResult r;r.payloadJson=std::move(out.payloadJson);r.errorText=std::move(out.errorText);r.deferredShutdown=out.deferredShutdown;
    const bool failedUnban=(p.internalExpiry||p.action=="unbanPlayer"||ConsoleUnban(p))&&
        (!r.errorText.empty()||!out.mutationVerified);
    if(failedUnban){
        // The plugin record may already be removed while VEIN's own ban remains.
        // Keep the timed expiry and intent until a read of both lists proves
        // absence or a later serialized unban attempt succeeds.
        return r;
    }
    if((p.action=="banPlayer"||p.action=="unbanPlayer"||p.internalExpiry||!ConsoleBanId(p).empty())&&
       std::none_of(out.effects.begin(),out.effects.end(),[](const Effect& e){return e.kind==Effect::Kind::BanChanged;}))
        out.effects.push_back({Effect::Kind::BanChanged,Json{{"gameId",p.internalExpiry?p.expiryPlayer:p.canonicalBanId}}.dump()});
    for(const auto& e:out.effects){
        if(e.kind==Effect::Kind::RememberLocation){Json j=Parse(e.json);std::string id=Str(j,"gameId");if(!id.empty()){auto v=std::make_shared<View>(*view_);v->lastLocationsJson[id]=j.value("position",Json::object()).dump();if(v->lastLocationsJson.size()>500)v->lastLocationsJson.erase(v->lastLocationsJson.begin());v->revision=++revision_;view_=v;}}
        if(e.kind==Effect::Kind::RememberPlayer){Json rows=Array(Parse(view_->knownPlayersJson)),player=Parse(e.json);std::string id=Str(player,"gameId");if(id.empty()||id.size()>1024)continue;if(player.dump().size()>8192)player=Json{{"gameId",id},{"name",id}};Json next=Json::array();for(auto& x:rows)if(Lower(Str(Object(x),"gameId"))!=Lower(id))next.push_back(x);next.push_back(player);while(next.size()>500)next.erase(next.begin());auto sr=store_.SaveKnownPlayers(next.dump());if(sr){auto v=std::make_shared<View>(*view_);v->knownPlayersJson=next.dump();v->revision=++revision_;view_=v;}else{r.persistenceOk=false;r.errorText=sr.error;}}
        if(e.kind==Effect::Kind::BanChanged){
            const std::string id=Str(Parse(e.json),"gameId");
            const std::string input=p.internalExpiry?p.expiryPlayer:p.canonicalBanId;
            const auto bans=state::ReadBans();
            Json next=Json::array();
            for(const auto& row:Array(Parse(view_->timedBansJson)))
                if(Str(Object(row),"gameId")!=id&&Str(Object(row),"gameId")!=input)next.push_back(row);
            for(const auto& ban:bans.records)if(ban.gameId==id&&!ban.expiresAt.empty())
                next.push_back({{"gameId",id},{"expiresAt",ban.expiresAt},{"reason",ban.reason}});
            auto saved=store_.SaveLegacy("timedBans",next.dump());
            if(saved&&state::BanRevision()!=bans.revision)
                saved={false,"ban list changed while applying timed state"};
            if(saved){
                auto v=std::make_shared<View>(*view_);
                v->timedBansJson=next.dump();v->banRevision=bans.revision;
                v->revision=++revision_;view_=v;
                if(r.errorText.empty()&&out.mutationVerified){
                    if(state::BanRevision()!=bans.revision)
                        saved={false,"ban list changed before retiring mutation intent"};
                    else saved=store_.FinishBanIntentsForPlayer(id.empty()?input:id);
                }
            }
            if(!saved){r.persistenceOk=false;r.errorText=saved.error;}
        }
    }
    return r;
}
MappedEvent Engine::MapEvent(const std::string& json) const{
    MappedEvent m;Json e=Parse(json);try{if(!e.is_object())throw std::runtime_error("malformed ring event");m.source={Str(e,"bootId"),e.value("seq",uint64_t{0})};m.type=Str(e,"type");Json data=MapEventData(m.type,e.value("data",Json::object()));if(e.contains("ts")){int64_t ms=DateMs(e["ts"]);if(ms>0)data["timestamp"]=IsoMs(ms);}m.frame=std::make_shared<const std::string>(Json{{"type","gameEvent"},{"payload",{{"type",m.type},{"data",data}}}}.dump());if(data.contains("player"))m.playerId=Str(data["player"],"gameId");m.valid=true;}catch(const std::exception& ex){m.error=ex.what();}return m;
}
bool Engine::SuppressRingConnection(const std::string& type) const{
    return (type=="player-connected"||type=="player-disconnected")&&log_->parser.TailConnections();
}
NativePersistence::Result Engine::ObserveAdmitted(const MappedEvent& e){
    if(!e.valid)return {false,e.error};if(e.type!="player-connected"&&e.type!="player-disconnected")return {true,{}};
    auto v=std::make_shared<View>(*view_);
    v->onlinePlayersJson=store_.Current().derivedOnlineJson;
    v->knownPlayersJson=store_.Current().derivedKnownJson;
    if(!e.playerId.empty())v->locationWindowUntilMs[StripPlatform(e.playerId)]=WallMs()+60000;
    if(v->locationWindowUntilMs.size()>500)v->locationWindowUntilMs.erase(v->locationWindowUntilMs.begin());
    v->revision=++revision_;view_=v;
    auto r=store_.SaveLegacy("online",v->onlinePlayersJson);if(!r)return r;
    return store_.SaveLegacy("knownPlayers",v->knownPlayersJson);
}
void Engine::NoteEventQueued(const std::string& playerId){if(playerId.empty())return;auto v=std::make_shared<View>(*view_);v->locationWindowUntilMs[StripPlatform(playerId)]=WallMs()+60000;if(v->locationWindowUntilMs.size()>500)v->locationWindowUntilMs.erase(v->locationWindowUntilMs.begin());v->revision=++revision_;view_=v;}
NativePersistence::Result Engine::SyncBanMetadata(){
    std::string raw;auto read=store_.ReadBanIntent(raw);
    if(!read)return read;
    std::set<std::string> protectedIds;
    if(!raw.empty()){
        Json journal=Parse(raw);
        if(!journal.is_object()||!journal.contains("intents")||!journal["intents"].is_array())
            return {false,"corrupt ban intent journal"};
        for(const auto& intent:journal["intents"]){
            const std::string id=Str(Object(intent),"gameId");
            if(id.empty())return {false,"corrupt ban intent: missing gameId"};
            protectedIds.insert(id);
        }
    }
    const auto current=state::ReadBans();
    Json synchronized=Json::array();
    for(const auto& row:Array(Parse(view_->timedBansJson)))
        if(protectedIds.count(Str(Object(row),"gameId")))synchronized.push_back(row);
    for(const auto& ban:current.records)
        if(!ban.expiresAt.empty()&&!protectedIds.count(ban.gameId))
            synchronized.push_back({{"gameId",ban.gameId},{"expiresAt",ban.expiresAt},{"reason",ban.reason}});
    const std::string content=synchronized.dump();
    // An action may have copied the latest global revision while updating
    // only its own player. Compare the complete derived state even when the
    // revision matches, so an external timed ban for another player is seen.
    if(content==view_->timedBansJson&&current.revision==view_->banRevision)return {true,{}};
    if(content!=view_->timedBansJson){
        auto saved=store_.SaveLegacy("timedBans",content);
        if(!saved)return saved;
    }
    if(state::BanRevision()!=current.revision)
        return {false,"ban list changed while synchronizing timed metadata"};
    auto v=std::make_shared<View>(*view_);
    v->timedBansJson=content;v->banRevision=current.revision;
    v->revision=++revision_;view_=v;
    return {true,{}};
}
std::vector<PreparedAction> Engine::DueTimedBans(int64_t nowMs){
    std::vector<PreparedAction> due;
    if(Actions::PendingBanJobs()!=0)return due;
    auto sync=SyncBanMetadata();
    if(!sync){lastError_=sync.error;return due;}
    std::string intent;auto read=store_.ReadBanIntent(intent);
    if(!read){lastError_=read.error;return due;}
    std::map<std::string,std::string> retryIds;
    if(!intent.empty()){
        Json journal=Parse(intent);
        if(!journal.is_object()||!journal.contains("intents")||!journal["intents"].is_array()){
            lastError_="corrupt ban intent journal";return due;
        }
        for(const auto& row:journal["intents"]){
            if(!UnbanIntent(row))return due; // A new permanent ban may still be in flight.
            retryIds[Str(Object(row),"gameId")]=Str(Object(row),"requestId");
        }
    }
    const auto current=state::ReadBans();
    if(current.revision!=view_->banRevision)return due;
    Json kept=Json::array();
    std::vector<std::string> staleExpiryIntents;
    bool changed=false;
    for(const auto& row:Array(Parse(view_->timedBansJson))){
        const Json b=Object(row);
        const std::string id=Str(b,"gameId"),expires=Str(b,"expiresAt");
        if(id.empty()||DateMs(b.value("expiresAt",Json()))>nowMs){kept.push_back(row);continue;}
        const auto ban=std::find_if(current.records.begin(),current.records.end(),[&](const state::BanRecord& item){return item.gameId==id;});
        const auto old=retryIds.find(id);
        if(ban!=current.records.end()&&ban->expiresAt!=expires){
            // An HTTP/native replacement ban superseded this expiry. A new
            // permanent ban must never be lifted by the old scheduled job.
            changed=true;
            if(!ban->expiresAt.empty())
                kept.push_back({{"gameId",id},{"expiresAt",ban->expiresAt},{"reason",ban->reason}});
            if(old!=retryIds.end()&&old->second.rfind("expiry:",0)==0)staleExpiryIntents.push_back(old->second);
            continue;
        }
        kept.push_back(row);
        if(ban==current.records.end()&&old==retryIds.end()){
            lastError_="timed ban lacks game enforcement record: "+id;
            continue;
        }
        PreparedAction p=PrepareAction("","unbanPlayer",Json{{"gameId",id}}.dump(),0);
        p.internalExpiry=true;p.expiryPlayer=id;p.expectedBanRevision=current.revision;
        p.requestId=old==retryIds.end()?"expiry:"+id+":"+std::to_string(p.expectedBanRevision):old->second;
        due.push_back(std::move(p));
    }
    if(changed){
        auto saved=store_.SaveLegacy("timedBans",kept.dump());
        if(!saved){lastError_=saved.error;return {};}
        if(state::BanRevision()!=current.revision){lastError_="ban list changed while replacing expiry";return {};}
        auto v=std::make_shared<View>(*view_);v->timedBansJson=kept.dump();v->banRevision=current.revision;v->revision=++revision_;view_=v;
        for(const auto& id:staleExpiryIntents){saved=store_.FinishBanIntent(id);if(!saved){lastError_=saved.error;return {};}}
    }
    return due;
}
bool Engine::NeedsBanVerification(){
    std::string raw;auto read=store_.ReadBanIntent(raw);
    if(!read){lastError_=read.error;return false;}
    return !raw.empty();
}
std::string Engine::VerifyBanState(){
    const uint64_t revisionBefore=state::BanRevision();
    Actions::Result result=Actions::Bans();
    if(result.status!=200||PluginState::Get().Capability("listBans")!="ok")
        throw std::runtime_error("cannot verify both game ban lists");
    Json rows=Parse(result.body);
    if(!rows.is_array())throw std::runtime_error("invalid verified game ban list");
    Json ids=Json::array();
    for(const auto& row:rows){const std::string id=Str(Object(row),"gameId");
        if(id.empty())throw std::runtime_error("game ban verification row has no gameId");
        ids.push_back(id);
    }
    const uint64_t revisionAfter=state::BanRevision();
    if(revisionAfter!=revisionBefore)throw std::runtime_error("ban list changed during verification");
    return Json{{"ids",ids},{"bans",rows},{"revision",revisionAfter}}.dump();
}
NativePersistence::Result Engine::ReconcileBanIntents(const std::string& verificationJson){
    std::string raw;auto read=store_.ReadBanIntent(raw);if(!read)return read;
    if(raw.empty()||Actions::PendingBanJobs()!=0)return {true,{}};
    Json journal=Parse(raw);if(!journal.is_object()||!journal.contains("intents")||!journal["intents"].is_array())return {false,"corrupt ban intent journal"};
    Json verification=Parse(verificationJson);
    if(!verification.is_object()||!verification.contains("ids")||!verification["ids"].is_array()||
       !verification.contains("bans")||!verification["bans"].is_array()||
       !verification.contains("revision")||!verification["revision"].is_number_unsigned())
        return {false,"invalid game ban verification"};
    const uint64_t verifiedRevision=verification["revision"].get<uint64_t>();
    const auto verifiedCurrent=state::ReadBans();
    if(verifiedCurrent.revision!=verifiedRevision)return {false,"ban list changed after verification"};
    std::set<std::string> verifiedIds;
    for(const auto& row:verification["ids"]){if(!row.is_string())return {false,"invalid game ban verification id"};verifiedIds.insert(row.get<std::string>());}
    if(!state::FlushBans())return {false,state::BanPersistenceError()};
    for(const auto& intent:journal["intents"])if(UnbanIntent(intent)){
        std::string id;
        try{id=IntentTarget(intent,verifiedCurrent.records);}catch(const std::exception& e){return {false,e.what()};}
        if(verifiedIds.count(id)){
            const std::string requestId=Str(Object(intent),"requestId");
            bool timed=false;for(const auto& row:Array(Parse(view_->timedBansJson)))
                if(Str(Object(row),"gameId")==id){timed=true;break;}
            const bool replacement=std::any_of(verifiedCurrent.records.begin(),verifiedCurrent.records.end(),
                [&](const state::BanRecord& b){return b.gameId==id;});
            if(requestId.rfind("expiry:",0)==0&&!timed&&replacement)
                return store_.FinishBanIntent(requestId); // stale expiry superseded by a newer ban
            return {false,"unban remains enforced in a game ban list: "+id};
        }
    }
    for(const auto& intent:journal["intents"]){
        const auto current=state::ReadBans();
        if(current.revision!=verifiedRevision)return {false,"ban list changed during intent reconciliation"};
        std::string id;
        try{id=IntentTarget(intent,current.records);}catch(const std::exception& e){return {false,e.what()};}
        const std::string rawId=Str(Object(intent),"gameId");
        const auto found=std::find_if(current.records.begin(),current.records.end(),[&](const state::BanRecord& b){return b.gameId==id;});
        bool repairedPluginRecord=false;
        if(!UnbanIntent(intent)){
            const Json desired=Object(Object(intent).value("desired",Json()));
            if(desired.empty()||Str(desired,"gameId")!=rawId)
                return {false,"ban intent lacks canonical desired state: "+rawId};
            const Json before=Object(intent).value("beforeRecord",Json());
            const bool hadBefore=before.is_object();
            const bool sameBefore=found!=current.records.end()&&hadBefore&&SameBanRecord(*found,before);
            const bool sameDesired=found!=current.records.end()&&found->expiresAt==Str(desired,"expiresAt")&&
                                   found->reason==Str(desired,"reason");
            if(sameBefore&&!sameDesired)
                return {false,"ambiguous ban intent: old plugin record remains for "+id};
            if(found==current.records.end()&&!hadBefore){
                const auto verified=std::find_if(verification["bans"].begin(),verification["bans"].end(),
                    [&](const Json& b){return Str(Object(b),"gameId")==id;});
                if(verified!=verification["bans"].end()){
                    if(Str(Object(*verified),"enforcedBy")!="game"||
                       Str(Object(*verified),"reason")!=Str(desired,"reason"))
                        return {false,"ban intent lacks matching game enforcement proof: "+id};
                    state::BanRecord recovered;
                    recovered.gameId=id;recovered.name=Str(Object(*verified),"name");
                    recovered.reason=Str(desired,"reason");recovered.expiresAt=Str(desired,"expiresAt");
                    if(!state::BanAddIfRevision(recovered,verifiedRevision))
                        return {false,"ban changed before intent recovery: "+id};
                    if(!state::FlushBans())return {false,state::BanPersistenceError()};
                    repairedPluginRecord=true;
                }
            }else if(found==current.records.end()&&hadBefore&&verifiedIds.count(id)){
                return {false,"ambiguous ban intent: prior plugin record disappeared while game ban remains for "+id};
            }
            // A different current record is a later replacement. Never
            // restore the intent's old expiry over that newer ban.
        }
        const auto now=state::ReadBans();
        if(now.revision!=(repairedPluginRecord?verifiedRevision+1:verifiedRevision))
            return {false,"ban list changed before timed state reconciliation"};
        Json next=Json::array();for(const auto& b:Array(Parse(view_->timedBansJson)))
            if(Str(Object(b),"gameId")!=id&&Str(Object(b),"gameId")!=rawId)next.push_back(b);
        for(const auto& b:now.records)if(b.gameId==id&&!b.expiresAt.empty())
            next.push_back({{"gameId",id},{"expiresAt",b.expiresAt},{"reason",b.reason}});
        auto saved=store_.SaveLegacy("timedBans",next.dump());if(!saved)return saved;
        if(state::BanRevision()!=now.revision)return {false,"ban list changed while writing timed state"};
        auto v=std::make_shared<View>(*view_);v->timedBansJson=next.dump();v->banRevision=state::BanRevision();v->revision=++revision_;view_=v;
        if(state::BanRevision()!=now.revision)return {false,"ban list changed before retiring intent"};
        saved=store_.FinishBanIntent(Str(Object(intent),"requestId"));if(!saved)return saved;
        if(repairedPluginRecord)return {true,{}}; // Re-verify before applying another journal entry.
    }
    return {true,{}};
}
std::vector<MappedEvent> Engine::ReconcileOnline(const std::string& liveJson,const std::string& bootId){std::vector<MappedEvent> out;Json live=Array(Parse(liveJson));for(auto& p:Array(Parse(view_->onlinePlayersJson))){std::string id=Str(Object(p),"gameId");bool found=false;for(auto& q:live)if(Str(Object(q),"gameId")==id){found=true;break;}if(!found){Json row={{"seq",0},{"bootId",bootId},{"type","player-disconnected"},{"data",{{"player",p}}},{"ts",IsoNowUtc()}};out.push_back(MapEvent(row.dump()));}}return out;}
std::vector<MappedEvent> Engine::OnRawLogLine(const std::string& line){std::vector<MappedEvent> out;for(auto& p:log_->parser.Feed(line)){Json row={{"seq",0},{"type",p.type},{"data",Parse(p.dataJson)},{"ts",IsoNowUtc()}};out.push_back(MapEvent(row.dump()));}return out;}
bool Engine::CustomLogJoin() const{return log_->parser.CustomJoin();}
bool Engine::CustomLogChat() const{return log_->parser.CustomChat();}
std::string Engine::HealthJson() const{return Json{{"revision",revision_},{"knownPlayers",Array(Parse(view_->knownPlayersJson)).size()},{"onlinePlayers",Array(Parse(view_->onlinePlayersJson)).size()},{"timedBans",Array(Parse(view_->timedBansJson)).size()},{"banPersistenceError",state::BanPersistenceError()},{"logParserError",log_->parser.LastError()},{"lastError",lastError_}}.dump();}
NativePersistence::Result Engine::Publish(){return {true,{}};}
} // namespace NativeBehavior
