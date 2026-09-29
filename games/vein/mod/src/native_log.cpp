#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>
#include "native_log.h"
#include "common.h"
#include "events_parse.h"
#include "state.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <map>
#include <set>

namespace NativeLog {
namespace {
using Json=nlohmann::json;
std::string Env(const char* k){const char* v=getenv(k);return v?v:"";}
std::string Trim(const std::string& s){auto b=s.find_first_not_of(" \t\r\n");return b==std::string::npos?"":s.substr(b,s.find_last_not_of(" \t\r\n")-b+1);}
std::string Lower(std::string s){std::transform(s.begin(),s.end(),s.begin(),[](unsigned char c){return std::tolower(c);});return s;}
struct Re {
    pcre2_code* code=nullptr;
    std::string label;
    ~Re(){if(code)pcre2_code_free(code);}
    Re()=default;Re(const Re&)=delete;Re& operator=(const Re&)=delete;
    Re(Re&& x)noexcept:code(x.code){x.code=nullptr;}
    Re& operator=(Re&& x)noexcept{if(this!=&x){if(code)pcre2_code_free(code);code=x.code;x.code=nullptr;}return *this;}
    bool Compile(const std::string& pattern,bool insensitive,std::string& error){int e=0;PCRE2_SIZE offset=0;code=pcre2_compile(reinterpret_cast<PCRE2_SPTR>(pattern.data()),pattern.size(),(insensitive?PCRE2_CASELESS:0)|PCRE2_UTF,&e,&offset,nullptr);if(code)return true;PCRE2_UCHAR text[256];pcre2_get_error_message(e,text,sizeof text);error="PCRE2 at "+std::to_string(offset)+": "+reinterpret_cast<const char*>(text);return false;}
    bool Has(const char* name)const{return code&&pcre2_substring_number_from_name(code,reinterpret_cast<PCRE2_SPTR>(name))>0;}
    unsigned Captures()const{uint32_t n=0;if(code)pcre2_pattern_info(code,PCRE2_INFO_CAPTURECOUNT,&n);return n;}
    struct Hit{bool matched=false,limitExceeded=false;std::map<std::string,std::string> fields;std::vector<std::string> groups;};
    Hit Match(const std::string& line)const{
        Hit h;if(!code)return h;
        auto* context=pcre2_match_context_create(nullptr);
        pcre2_set_match_limit(context,200000);pcre2_set_depth_limit(context,1000);
        auto* m=pcre2_match_data_create_from_pattern(code,nullptr);
        int rc=pcre2_match(code,reinterpret_cast<PCRE2_SPTR>(line.data()),line.size(),0,0,m,context);
        pcre2_match_context_free(context);
        if(rc<0){h.limitExceeded=rc==PCRE2_ERROR_MATCHLIMIT||rc==PCRE2_ERROR_DEPTHLIMIT;pcre2_match_data_free(m);return h;}
        h.matched=true;PCRE2_SIZE* v=pcre2_get_ovector_pointer(m);uint32_t count=0;pcre2_pattern_info(code,PCRE2_INFO_CAPTURECOUNT,&count);for(uint32_t i=1;i<=count;i++){if(v[2*i]!=PCRE2_UNSET)h.groups.push_back(line.substr(v[2*i],v[2*i+1]-v[2*i]));else h.groups.push_back("");}for(const char* name:{"gameId","gameId2","name","name2","msg","channel"}){int n=pcre2_substring_number_from_name(code,reinterpret_cast<PCRE2_SPTR>(name));if(n>0&&static_cast<uint32_t>(n)<=count&&v[2*n]!=PCRE2_UNSET)h.fields[name]=line.substr(v[2*n],v[2*n+1]-v[2*n]);}pcre2_match_data_free(m);return h;
    }
};
std::string Field(const Re::Hit& h,const char* key){auto it=h.fields.find(key);return it==h.fields.end()?"":Trim(it->second);}
std::string Id(const Re::Hit& h){auto s=Field(h,"gameId");if(s.empty())s=Field(h,"gameId2");if(s.empty()&&!h.groups.empty()&&h.groups[0].size()==17)s=h.groups[0];return s;}
std::string Name(const Re::Hit& h){auto s=Field(h,"name");if(s.empty())s=Field(h,"name2");if(s.empty()){auto id=Id(h);size_t pos=id.empty()?0:1;if(pos<h.groups.size())s=Trim(h.groups[pos]);}return s;}
std::string Msg(const Re::Hit& h){auto s=Field(h,"msg");if(s.empty()&&h.fields.empty()&&h.groups.size()>1)s=Trim(h.groups[1]);return s;}
const char* kLogin=R"((?:LogNet:\s*Login request:.*?\?Name=(?<name>[^?\s]+).*?userId:\s*(?:Steam:)?(?<gameId>7656\d{13})|LogVein:\s*PlayerState ID changed to\s*(?<gameId2>7656\d{13})))";
const char* kJoin=R"((?:LogVein:\s*(?:\[[^\]]*\]\s*)?Player (?<name>.+?) selected character (?<characterId>[0-9A-Fa-f]{32})|LogNet:\s*Join succeeded:\s*(?<name2>.+?)\s*$))";
const char* kLeave=R"(LogNet:.*?(?:UNetConnection::Close|UChannel::CleanUp).*?UniqueId:\s*(?:Steam:)?(?<gameId>7656\d{13}))";
const char* kLeaveName=R"(LogNet:.*?(?:UNetConnection::Close|Connection closed).*?PlayerName:\s*(?<name>[^\s,]+))";
const char* kChat=R"(LogVeinChat:\s*\[(?<gameId>7656\d{13})\]\s*(?<name>.+?)(?:\s*\(aka (?<character>[^)]*)\))?:\s?(?<msg>.*)$)";
const char* kReady=R"(Created session GameSession\.)";
bool WordChar(char c){return std::isalnum(static_cast<unsigned char>(c))||c=='_';}
bool Word(const std::string& s,const std::string& token){
    size_t pos=0;while((pos=s.find(token,pos))!=std::string::npos){
        const size_t end=pos+token.size();if((pos==0||!WordChar(s[pos-1]))&&(end==s.size()||!WordChar(s[end])))return true;
        pos=end;
    }return false;
}
bool Noise(const std::string& s){
    if(Trim(s).empty())return true;
    for(const char* k:{"LogHttp","LogSteamShared","LogStreaming","LogNetTraffic","LogGarbage"})if(Word(s,k))return true;
    size_t eos=s.find("LogEOS");if(eos!=std::string::npos&&(eos==0||!WordChar(s[eos-1])))return true;
    for(const char* k:{"LogOnline:","LogOnlineSession:","LogOnlineIdentity:","LogOnlinePresence:","LogOnlineFriend:","LogOnlineSteam:"}){
        size_t pos=s.find(k);if(pos==std::string::npos)continue;pos+=std::strlen(k);
        while(pos<s.size()&&std::isspace(static_cast<unsigned char>(s[pos])))++pos;
        if(s.compare(pos,7,"Verbose")==0)return true;
    }
    return false;
}
bool CommonJsPcreSubset(const std::string& pattern){
    // These PCRE-specific constructs either fail under the legacy JavaScript
    // RegExp engine or have materially different capture/backtracking rules.
    for(const char* unsupported:{"\\K","\\R","\\C","\\p{","\\P{","(*","(?>","(?|","(?R","(?0","(?P"})
        if(pattern.find(unsupported)!=std::string::npos)return false;
    return true;
}
}
struct Parser::Impl {
    Re login,join,leave,leaveName,chat,ready;
    std::map<std::string,std::string> loginIds,present;
    std::string pendingId,mode="auto",events="filtered";
    bool readySeen=false,customJoin=false,customChat=false;
    std::string lastError;
};
Parser::Parser():impl_(std::make_unique<Impl>()){}
Parser::~Parser()=default;
bool Parser::Configure(std::string& key,std::string& detail){
    struct Spec{const char* env;const char* def;Re* re;const char* fixture;const char* required;bool defaultInsensitive;};
    auto& s=*impl_;
    Spec list[]={
        {"VEIN_LOG_LOGIN_RE",kLogin,&s.login,"LogVein: PlayerState ID changed to 76561198000000001","gameId",true},
        {"VEIN_LOG_JOIN_RE",kJoin,&s.join,"LogVein: [] Player SteamPersona selected character 0123456789abcdef0123456789abcdef (aka Char)","name",false},
        {"VEIN_LOG_LEAVE_RE",kLeave,&s.leave,"LogNet: UNetConnection::Close UniqueId: Steam:76561198000000001","gameId",true},
        {"VEIN_LOG_CHAT_RE",kChat,&s.chat,"LogVeinChat: [76561198000000001] SteamPersona (aka Char): hello world","msg",false},
        {"VEIN_LOG_READY_RE",kReady,&s.ready,"Created session GameSession.","",false},
    };
    for(auto& x:list){key=x.env;std::string custom=Trim(Env(x.env)),pattern=custom.empty()?x.def:custom;
        x.re->label=x.env;
        if(!custom.empty()&&!CommonJsPcreSubset(custom)){detail="uses a PCRE-only or incompatible construct";return false;}
        if(!x.re->Compile(pattern,!custom.empty()||x.defaultInsensitive,detail))return false;
        if(!custom.empty()){
            bool structural=false;
            if(std::string(x.required)=="gameId")structural=x.re->Has("gameId")||x.re->Has("gameId2")||x.re->Captures()>0;
            else if(std::string(x.required)=="name")structural=x.re->Has("name")||x.re->Has("name2")||x.re->Has("gameId")||x.re->Captures()>0;
            else if(std::string(x.required)=="msg")structural=x.re->Has("msg")||x.re->Captures()>=2;
            else structural=true;
            if(!structural){detail="missing required capture "+std::string(x.required);return false;}
            auto h=x.re->Match(x.fixture);
            if(h.matched&&*x.required){
                std::string got=std::string(x.required)=="gameId"?Id(h):std::string(x.required)=="name"?Name(h):Msg(h);
                if(got.empty()){detail="legacy fixture matched but required capture "+std::string(x.required)+" is empty";return false;}
            }
            if(std::string(x.env)=="VEIN_LOG_JOIN_RE"||std::string(x.env)=="VEIN_LOG_LOGIN_RE"||std::string(x.env)=="VEIN_LOG_LEAVE_RE")s.customJoin=true;
            if(std::string(x.env)=="VEIN_LOG_CHAT_RE")s.customChat=true;
        }
    }
    key="VEIN_LOG_LEAVE_RE";if(!s.leaveName.Compile(kLeaveName,true,detail))return false;
    s.mode=Lower(Trim(Env("VEIN_LOG_TAIL")));if(s.mode.empty())s.mode="auto";
    if(s.mode!="auto"&&s.mode!="always"&&s.mode!="never"){key="VEIN_LOG_TAIL";detail="must be auto|always|never";return false;}
    s.events=Lower(Trim(Env("VEIN_LOG_EVENTS")));if(s.events.empty())s.events="filtered";
    if(s.events!="all"&&s.events!="filtered"&&s.events!="none"){key="VEIN_LOG_EVENTS";detail="must be all|filtered|none";return false;}
    return true;
}
bool Parser::Ready() const{return impl_->readySeen;}
bool Parser::TailConnections() const{
    if(impl_->mode=="never")return false;
    if(impl_->mode=="always")return true;
    // Legacy auto mode activates the connection tail when the player getter
    // capability degrades. The chat hook/fallback remains authoritative.
    return PluginState::Get().Capability("players")!="ok";
}
bool Parser::CustomJoin() const{return TailConnections();}
bool Parser::CustomChat() const{return false;}
std::string Parser::LastError() const{return impl_->lastError;}
std::string Parser::Redact(const std::string& line) const{
    std::string s=EventsParse::RedactLogLine(line);
    std::string lower=line;std::transform(lower.begin(),lower.end(),lower.begin(),[](unsigned char c){return std::tolower(c);});
    if(lower.find("password")!=std::string::npos&&s==line)return "[redacted: line mentions a password]";
    return s;
}
std::vector<Parsed> Parser::Feed(const std::string& raw){
    auto& s=*impl_;std::vector<Parsed> out;std::string line=raw;if(!line.empty()&&line.back()=='\r')line.pop_back();
    auto match=[&](const Re& re){auto h=re.Match(line);if(h.limitExceeded)s.lastError=re.label+": PCRE2 match/depth limit exceeded";return h;};
    if(s.events!="none"&&(s.events=="all"||!Noise(line)))out.push_back({"log",Json{{"msg",Redact(line)}}.dump()});
    if(s.mode=="never")return out;
    if(match(s.ready).matched){s.readySeen=true;s.present.clear();s.pendingId.clear();return out;}
    auto h=match(s.login);if(h.matched){auto id=Id(h),name=Name(h);if(id.size()>1024||name.size()>1024)return out;if(!id.empty()&&!name.empty()){s.loginIds[name]=id;if(s.loginIds.size()>500)s.loginIds.erase(s.loginIds.begin());}else if(!id.empty())s.pendingId=id;return out;}
    h=match(s.chat);if(h.matched){return out;} // Legacy grammar observes chat; game hooks/fallback emit it.
    h=match(s.join);if(h.matched){auto id=Id(h),name=Name(h);if(id.size()>1024||name.size()>1024)return out;if(id.empty()&&!name.empty()){auto it=s.loginIds.find(name);if(it!=s.loginIds.end())id=it->second;}if(id.empty())id=s.pendingId;s.pendingId.clear();auto key=id.empty()?name:id;if(!key.empty()&&!s.present.count(key)){s.present[key]=name;if(s.present.size()>500)s.present.erase(s.present.begin());if(TailConnections()){Json p=Json::object();if(!id.empty())p["gameId"]=id;if(!name.empty())p["name"]=name;out.push_back({"player-connected",Json{{"player",p}}.dump()});}}return out;}
    h=match(s.leave);if(!h.matched)h=match(s.leaveName);
    if(h.matched){auto id=Id(h),name=Name(h);if(id.size()>1024||name.size()>1024)return out;if(id.empty()&&!name.empty()){auto it=s.loginIds.find(name);if(it!=s.loginIds.end())id=it->second;}auto key=id.empty()?name:id;auto it=s.present.find(key);if(it!=s.present.end()){if(name.empty())name=it->second;s.present.erase(it);if(TailConnections()){Json p=Json::object();if(!id.empty())p["gameId"]=id;if(!name.empty())p["name"]=name;out.push_back({"player-disconnected",Json{{"player",p}}.dump()});}}}
    return out;
}
} // namespace NativeLog
