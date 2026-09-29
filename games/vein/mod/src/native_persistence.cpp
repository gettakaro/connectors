#include "native_persistence.h"
#include "common.h"

#include <nlohmann/json.hpp>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fcntl.h>
#include <limits>
#include <regex>
#include <sys/stat.h>
#include <unistd.h>

namespace NativePersistence {
namespace {
using Json = nlohmann::json;
namespace fs = std::filesystem;
#ifdef TAKARO_BRIDGE_TEST
std::function<void(const std::string&)> beforeRenameHook;
std::function<bool(const std::string&)> directorySyncFailureHook;
#endif
std::string Env(const char* key) { const char* v = getenv(key); return v && *v ? v : ""; }
std::string Error(const std::string& what, const std::string& file) {
    return what + " " + file + ": " + std::strerror(errno);
}
Result FsyncDirectory(const std::string& path){
#ifdef TAKARO_BRIDGE_TEST
    if(directorySyncFailureHook&&directorySyncFailureHook(path))
        return {false,"injected directory fsync failure: "+path};
#endif
    const std::string dir = fs::path(path).parent_path().empty()?".":fs::path(path).parent_path().string();
    int dfd = open(dir.c_str(), O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    if (dfd < 0) return {false, Error("open directory",dir)};
    if (fsync(dfd) != 0) { auto e=Error("fsync directory",dir); close(dfd); return {false,e}; }
    close(dfd);return {true,{}};
}
Result AtomicWrite(const std::string& path, const std::string& content, bool* renamed=nullptr) {
    if(renamed)*renamed=false;
    try { fs::create_directories(fs::path(path).parent_path().empty()?fs::path("."):fs::path(path).parent_path()); }
    catch (const std::exception& e) { return {false, e.what()}; }
    std::string pattern=path+".tmp.XXXXXX";
    std::vector<char> tmpName(pattern.begin(),pattern.end());tmpName.push_back('\0');
    int fd=mkstemp(tmpName.data());
    const std::string tmp=tmpName.data();
    if (fd < 0) return {false, Error("open", tmp)};
    fcntl(fd,F_SETFD,FD_CLOEXEC);
    size_t pos = 0;
    while (pos < content.size()) {
        ssize_t n = write(fd, content.data()+pos, content.size()-pos);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { auto e=Error("write",tmp); close(fd); unlink(tmp.c_str()); return {false,e}; }
        pos += static_cast<size_t>(n);
    }
    if (fsync(fd) != 0) { auto e=Error("fsync",tmp); close(fd); unlink(tmp.c_str()); return {false,e}; }
    if (close(fd) != 0) { auto e=Error("close",tmp); unlink(tmp.c_str()); return {false,e}; }
#ifdef TAKARO_BRIDGE_TEST
    if(beforeRenameHook)beforeRenameHook(path);
#endif
    if (rename(tmp.c_str(), path.c_str()) != 0) { auto e=Error("rename",path); unlink(tmp.c_str()); return {false,e}; }
    if(renamed)*renamed=true;
    return FsyncDirectory(path);
}
Result Read(const std::string& path, std::string& out, bool missingOk,
            bool* found=nullptr, size_t maxBytes=256u*1024u*1024u) {
    out.clear();if(found)*found=false;
    int fd = open(path.c_str(), O_RDONLY|O_CLOEXEC);
    if (fd < 0) return errno == ENOENT && missingOk ? Result{true,{}} : Result{false,Error("read",path)};
    if(found)*found=true;
    char buf[8192];
    for (;;) {
        ssize_t n = read(fd,buf,sizeof buf);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { auto e=Error("read",path); close(fd); return {false,e}; }
        if (n == 0) break;
        if (out.size()+static_cast<size_t>(n) > maxBytes) { close(fd); return {false,"state file too large: "+path}; }
        out.append(buf,static_cast<size_t>(n));
    }
    close(fd); return {true,{}};
}
Json CursorJson(const SourceCursor& c) {
    Json j={{"seq",c.seq}}; if (!c.bootId.empty()) j["bootId"]=c.bootId; return j;
}
bool ParseCursor(const Json& j, SourceCursor& c) {
    if(!j.is_object()||!j.contains("seq")||!j["seq"].is_number_unsigned())return false;
    c.seq=j["seq"].get<uint64_t>();
    if(j.contains("bootId")){if(!j["bootId"].is_string())return false;c.bootId=j["bootId"].get<std::string>();}
    return true;
}
bool ValidExpiry(const std::string& s){
    if(s.size()>64)return false;
    static const std::regex re(R"(^(?:\d{4}|\+\d{6})-(0[1-9]|1[0-2])-(0[1-9]|[12]\d|3[01])T([01]\d|2[0-3]):[0-5]\d:[0-5]\d(?:\.\d{1,9})?(?:Z|[+-]([01]\d|2[0-3]):?[0-5]\d)$)");
    if(!std::regex_match(s,re))return false;
    int year=0,month=0,day=0;if(sscanf(s.c_str(),"%d-%d-%d",&year,&month,&day)!=3||year<1||year>275760)return false;
    static const int days[]={0,31,28,31,30,31,30,31,31,30,31,30,31};
    int max=days[month];if(month==2&&((year%4==0&&year%100!=0)||year%400==0))max=29;
    return day<=max;
}
bool WithinDepth(const std::string& s){
    unsigned depth=0;bool quoted=false,escaped=false;
    for(char c:s){if(quoted){if(escaped)escaped=false;else if(c=='\\')escaped=true;else if(c=='"')quoted=false;}
        else if(c=='"')quoted=true;else if(c=='{'||c=='['){if(++depth>64)return false;}
        else if(c=='}'||c==']'){if(!depth)return false;--depth;}}
    return depth==0&&!quoted;
}
bool ValidFrame(const std::string& s){
    if(!WithinDepth(s))return false;
    Json j=Json::parse(s,nullptr,false);
    if(!j.is_object()||j.value("type",std::string())!="gameEvent")return false;
    Json p=j.value("payload",Json::object());if(!p.is_object()||!p.contains("data")||!p["data"].is_object())return false;
    const std::string type=p.value("type",std::string());
    return type=="player-connected"||type=="player-disconnected"||type=="chat-message"||
           type=="player-death"||type=="entity-killed"||type=="log";
}
void ApplyDerived(Snapshot& s,const NativeTransport::Frame& frame){
    Json wire=Json::parse(*frame,nullptr,false);
    if(!wire.is_object()||wire.value("type",std::string())!="gameEvent")return;
    Json payload=wire.value("payload",Json::object());if(!payload.is_object())return;
    std::string type=payload.value("type",std::string());
    if(type!="player-connected"&&type!="player-disconnected")return;
    Json data=payload.value("data",Json::object());if(!data.is_object())return;
    Json player=data.value("player",Json::object());
    if(!player.is_object()||!player.contains("gameId")||!player["gameId"].is_string())return;
    const std::string id=player["gameId"].get<std::string>();
    if(id.empty()||id.size()>1024)return;
    // A 32 MiB event may carry a long chat/display field. Derived player
    // snapshots must remain independently reloadable within their 8 MiB cap.
    if(player.dump().size()>8192)player=Json{{"gameId",id},{"name",id}};
    Json online=Json::parse(s.derivedOnlineJson,nullptr,false);if(!online.is_array())online=Json::array();
    Json next=Json::array();for(auto& row:online)if(!row.is_object()||row.value("gameId",std::string())!=id)next.push_back(row);
    if(type=="player-connected")next.push_back(player);
    while(next.size()>500)next.erase(next.begin());
    s.derivedOnlineJson=next.dump();
    if(type=="player-connected"){
        Json known=Json::parse(s.derivedKnownJson,nullptr,false);if(!known.is_array())known=Json::array();
        next=Json::array();for(auto& row:known)if(!row.is_object()||row.value("gameId",std::string())!=id)next.push_back(row);
        next.push_back(player);while(next.size()>500)next.erase(next.begin());s.derivedKnownJson=next.dump();
    }
    s.hasDerivedState=true;
}
Json ToJson(const Snapshot& s) {
    Json j={{"version",s.version},{"nextOutboxId",s.nextOutboxId},{"scan",CursorJson(s.scan)},
            {"confirmed",CursorJson(s.confirmed)},{"deliveryLosses",s.deliveryLosses},
            {"legacyBanMigrationDone",s.legacyBanMigrationDone},
            {"derivedOnline",Json::parse(s.derivedOnlineJson)},{"derivedKnown",Json::parse(s.derivedKnownJson)},
            {"pending",Json::array()}};
    for (const auto& e:s.pending) j["pending"].push_back({{"outboxId",e.outboxId},{"source",CursorJson(e.source)},
                                                         {"frame",*e.frame}});
    return j;
}
bool FromJson(const Json& j, Snapshot& s, std::string& error) {
    if (!j.is_object() || !j.contains("version") || !j["version"].is_number_integer() ||
        j["version"].get<int>()!=1 || !j.contains("pending") || !j["pending"].is_array()) {
        error="unsupported or corrupt native outbox version"; return false;
    }
    try {
        if(!j.at("nextOutboxId").is_number_unsigned()||!j.at("deliveryLosses").is_number_unsigned())throw std::runtime_error("invalid unsigned outbox counter");
        s.version=1; s.nextOutboxId=j.at("nextOutboxId").get<uint64_t>();
        if(s.nextOutboxId==0||!ParseCursor(j.at("scan"),s.scan)||!ParseCursor(j.at("confirmed"),s.confirmed))throw std::runtime_error("invalid cursor");
        if(s.confirmed.bootId!=s.scan.bootId||s.confirmed.seq>s.scan.seq)throw std::runtime_error("inconsistent confirmed cursor");
        s.deliveryLosses=j.at("deliveryLosses").get<uint64_t>();
        // An outbox written by an earlier native candidate is already past
        // the one-time sidecar migration, even if this field predates it.
        if(j.contains("legacyBanMigrationDone")){
            if(!j["legacyBanMigrationDone"].is_boolean())throw std::runtime_error("invalid legacy ban migration marker");
            s.legacyBanMigrationDone=j["legacyBanMigrationDone"].get<bool>();
        }else s.legacyBanMigrationDone=true;
        if(j.contains("derivedOnline")||j.contains("derivedKnown")){
            if(!j.value("derivedOnline",Json()).is_array()||!j.value("derivedKnown",Json()).is_array())throw std::runtime_error("invalid derived players");
            if(j["derivedOnline"].size()>500||j["derivedKnown"].size()>500||
               j["derivedOnline"].dump().size()>8u*1024u*1024u||j["derivedKnown"].dump().size()>8u*1024u*1024u)
                throw std::runtime_error("derived player state exceeds bound");
            s.derivedOnlineJson=j["derivedOnline"].dump();s.derivedKnownJson=j["derivedKnown"].dump();s.hasDerivedState=true;
        }
        uint64_t prev=0; size_t bytes=0;
        for (const auto& row:j.at("pending")) {
            DurableEvent e; e.outboxId=row.at("outboxId").get<uint64_t>();
            if(!row.at("outboxId").is_number_unsigned()||!ParseCursor(row.at("source"),e.source))throw std::runtime_error("invalid event cursor");
            e.frame=std::make_shared<const std::string>(row.at("frame").get<std::string>());
            if(!ValidFrame(*e.frame))throw std::runtime_error("invalid durable event frame");
            if (e.outboxId<=prev || e.outboxId>=s.nextOutboxId) throw std::runtime_error("invalid outbox order");
            if(e.source.bootId==s.scan.bootId&&e.source.seq>s.scan.seq)throw std::runtime_error("event ahead of scan cursor");
            prev=e.outboxId; bytes+=e.frame->size(); s.pending.push_back(std::move(e));
        }
        if (s.pending.size()>5000 || bytes>32u*1024u*1024u) throw std::runtime_error("outbox exceeds bound");
        return true;
    } catch (const std::exception& e) { error=e.what(); return false; }
}
}

#ifdef TAKARO_BRIDGE_TEST
void TestBeforeRename(std::function<void(const std::string& path)> hook){beforeRenameHook=std::move(hook);}
void TestDirectorySyncFailure(std::function<bool(const std::string& path)> fail){directorySyncFailureHook=std::move(fail);}
#endif
Paths ResolvePaths() {
    Paths p;
    std::string cursor=Env("TAKARO_CURSOR_FILE");
    p.directory=Env("TAKARO_STATE_DIR");
    if (p.directory.empty()) p.directory=(fs::path(PluginDataDir())/"connector-state").string();
    // Legacy sidecar derived every other default from the explicit cursor path.
    const std::string sibling=cursor.empty()?p.directory:fs::path(cursor).parent_path().string();
    p.cursor=cursor.empty()?(fs::path(p.directory)/"event-cursor.json").string():cursor;
    p.online=Env("TAKARO_ONLINE_FILE"); if (p.online.empty()) p.online=(fs::path(sibling)/"online-players.json").string();
    p.timedBans=Env("TAKARO_BAN_FILE"); if (p.timedBans.empty()) p.timedBans=(fs::path(sibling)/"timed-bans.json").string();
    p.knownPlayers=Env("TAKARO_KNOWN_PLAYERS_FILE"); if (p.knownPlayers.empty()) p.knownPlayers=(fs::path(sibling)/"known-players.json").string();
    p.outbox=(fs::path(p.directory)/"event-outbox.json").string();
    p.banIntent=(fs::path(p.directory)/"ban-intent.json").string();
    return p;
}

Store::Store(Paths paths):paths_(std::move(paths)){}
Result Store::Failure(std::string error,const std::string& category) {
    ++errors_;activeErrors_[category]=std::move(error);lastError_=activeErrors_[category];return {false,lastError_};
}
void Store::Recover(const std::string& category){activeErrors_.erase(category);lastError_=activeErrors_.empty()?"":activeErrors_.rbegin()->second;}
Result Store::Commit(Snapshot next) {
    if(outboxDurabilityPending_)return Failure("outbox directory fsync remains pending","outbox");
    const bool cursorChanged=next.confirmed.bootId!=snapshot_.confirmed.bootId||next.confirmed.seq!=snapshot_.confirmed.seq;
    bool renamed=false;auto r=AtomicWrite(paths_.outbox,ToJson(next).dump(),&renamed);
    if (!r) {
        if(renamed){
            snapshot_=std::move(next);outboxDurabilityPending_=true;cursorDirty_|=cursorChanged;
            Failure(r.error,"outbox");
            return {true,{}}; // visible admission; delivery waits for RetryDurability
        }
        return Failure(r.error,"outbox");
    }
    snapshot_=std::move(next);
    Recover("outbox");
    // Cursor is a derived compatibility mirror. Outbox remains authoritative.
    if(cursorChanged||cursorDirty_){auto mirror=AtomicWrite(paths_.cursor,CursorJson(snapshot_.confirmed).dump());
        if (!mirror) {cursorDirty_=true;Failure(mirror.error,"cursor");} // Derived mirror; authoritative outbox committed.
        else {cursorDirty_=false;Recover("cursor");}
    }
    return {true,{}};
}
Result Store::RetryDurability(){
    if(outboxDurabilityPending_){
        auto synced=FsyncDirectory(paths_.outbox);
        if(!synced)return Failure(synced.error,"outbox");
        outboxDurabilityPending_=false;Recover("outbox");
    }
    if(cursorDirty_){auto mirror=AtomicWrite(paths_.cursor,CursorJson(snapshot_.confirmed).dump());
        if(!mirror){Failure(mirror.error,"cursor");return {true,{}};}
        cursorDirty_=false;Recover("cursor");}
    return {true,{}};
}
Result Store::Load() {
    std::string raw;bool found=false;auto r=Read(paths_.outbox,raw,true,&found); if (!r) return Failure(r.error,"outbox");
    if(found&&raw.empty())return Failure("empty native outbox: "+paths_.outbox,"outbox");
    if (!raw.empty()) {
        if(!WithinDepth(raw))return Failure("corrupt native outbox: JSON exceeds depth 64 or is malformed","outbox");
        Snapshot s; std::string error; Json j=Json::parse(raw,nullptr,false);
        if (j.is_discarded() || !FromJson(j,s,error)) return Failure("corrupt "+paths_.outbox+": "+error,"outbox");
        snapshot_=std::move(s);Recover("outbox"); return {true,{}};
    }
    r=Read(paths_.cursor,raw,true,&found,8u*1024u*1024u); if (!r) return Failure(r.error,"cursor");
    if(found){if(!WithinDepth(raw))return Failure("corrupt legacy cursor: JSON exceeds depth 64","cursor");Json j=Json::parse(raw,nullptr,false);SourceCursor c;if(j.is_discarded()||!ParseCursor(j,c))return Failure("corrupt legacy cursor: "+paths_.cursor,"cursor");snapshot_.scan=snapshot_.confirmed=c;}
    Recover("cursor");
    std::string online,known;r=ReadLegacy("online",online);if(!r)return Failure(r.error);
    r=ReadLegacy("knownPlayers",known);if(!r)return Failure(r.error);
    snapshot_.derivedOnlineJson=online;snapshot_.derivedKnownJson=known;snapshot_.hasDerivedState=true;
    return {true,{}};
}
AdmitResult Store::Admit(SourceCursor source, NativeTransport::Frame frame,size_t externalBytes,size_t externalCount) {
    auto r=AdmitMany({{std::move(source),std::move(frame)}},externalBytes,externalCount);
    return {r.ok,r.error,r.ok?r.outboxIds.front():0};
}
AdmitManyResult Store::AdmitMany(const std::vector<std::pair<SourceCursor,NativeTransport::Frame>>& events,size_t externalBytes,size_t externalCount){
    if(events.empty())return {true,{}, {}};
    if(externalBytes>32u*1024u*1024u||externalCount>5000)return {false,"external event staging exceeds bound",{}};
    Snapshot n=snapshot_;
    std::vector<uint64_t> ids;ids.reserve(events.size());
    for(const auto& [source,frame]:events){
        if(!frame||frame->size()>32u*1024u*1024u)return {false,"event exceeds 32 MiB",{}};
        if(source.bootId!=n.scan.bootId||source.seq<=n.scan.seq)return {false,"out-of-order event cursor",{}};
        n.scan=source;const uint64_t id=n.nextOutboxId++;ids.push_back(id);
        n.pending.push_back({id,source,frame});ApplyDerived(n,frame);
    }
    size_t bytes=0; for (const auto& e:n.pending) bytes+=e.frame->size();
    while (!n.pending.empty() && (n.pending.size()+externalCount>5000 || bytes+externalBytes>32u*1024u*1024u)) {
        bytes-=n.pending.front().frame->size();
        if (n.pending.front().source.bootId==n.scan.bootId && n.pending.front().source.seq>n.confirmed.seq)
            n.confirmed=n.pending.front().source;
        n.pending.erase(n.pending.begin()); ++n.deliveryLosses;
    }
    auto r=Commit(std::move(n)); return {r.ok,r.error,r.ok?ids:std::vector<uint64_t>{}};
}
AdmitResult Store::AdmitSynthetic(NativeTransport::Frame frame,size_t externalBytes,size_t externalCount) {
    if (!frame || frame->size()>32u*1024u*1024u) return {false,"event exceeds 32 MiB",0};
    if(externalBytes>32u*1024u*1024u||externalCount>5000)return {false,"external event staging exceeds bound",0};
    Snapshot n=snapshot_; const uint64_t id=n.nextOutboxId++;
    n.pending.push_back({id,{"",0},frame});
    ApplyDerived(n,frame);
    size_t bytes=0; for(const auto& e:n.pending)bytes+=e.frame->size();
    while(!n.pending.empty()&&(n.pending.size()+externalCount>5000||bytes+externalBytes>32u*1024u*1024u)){bytes-=n.pending.front().frame->size();
        if(n.pending.front().source.bootId==n.scan.bootId&&n.pending.front().source.seq>n.confirmed.seq)n.confirmed=n.pending.front().source;
        n.pending.erase(n.pending.begin());++n.deliveryLosses;}
    auto r=Commit(std::move(n));return {r.ok,r.error,r.ok?id:0};
}
Result Store::ConfirmThrough(uint64_t id) {
    Snapshot n=snapshot_;
    size_t count=0;
    while(count<n.pending.size() && n.pending[count].outboxId<=id) {
        if(n.pending[count].source.bootId==n.scan.bootId&&n.pending[count].source.seq>n.confirmed.seq)
            n.confirmed=n.pending[count].source;
        ++count;
    }
    if (!count) return {true,{}};
    n.pending.erase(n.pending.begin(),n.pending.begin()+count);
    // Skipped connection rows have no outbox entry of their own. Once all
    // current-boot durable rows before the scan are confirmed, their cursor
    // can advance without inventing a delivery loss.
    bool currentPending=false;
    for(const auto& e:n.pending)if(e.source.bootId==n.scan.bootId&&e.source.seq>0){currentPending=true;break;}
    if(!currentPending)n.confirmed=n.scan;
    return Commit(std::move(n));
}
Result Store::Skip(SourceCursor source){
    if(source.bootId!=snapshot_.scan.bootId||source.seq<=snapshot_.scan.seq)
        return {false,"out-of-order suppressed event cursor"};
    Snapshot n=snapshot_;n.scan=std::move(source);
    bool currentPending=false;
    for(const auto& e:n.pending)if(e.source.bootId==n.scan.bootId&&e.source.seq>0){currentPending=true;break;}
    if(!currentPending)n.confirmed=n.scan;
    return Commit(std::move(n));
}
Result Store::RecordLoss(SourceCursor source,const std::string& reason,uint64_t lostCount) {
    (void)reason;
    if(lostCount==0||source.bootId!=snapshot_.scan.bootId||source.seq<=snapshot_.scan.seq||
       lostCount>std::numeric_limits<uint64_t>::max()-snapshot_.deliveryLosses)
        return {false,"invalid event loss interval"};
    Snapshot n=snapshot_; n.scan=source; n.deliveryLosses+=lostCount;
    if (n.pending.empty()) n.confirmed=source;
    return Commit(std::move(n));
}
Result Store::SwitchBoot(const std::string& bootId) {
    if (snapshot_.scan.bootId==bootId) return {true,{}};
    Snapshot n=snapshot_; n.scan={bootId,0}; n.confirmed={bootId,0}; return Commit(std::move(n));
}
Result Store::MarkLegacyBanMigrationDone() {
    if(snapshot_.legacyBanMigrationDone)return {true,{}};
    Snapshot next=snapshot_;next.legacyBanMigrationDone=true;
    return Commit(std::move(next));
}
Result Store::ReadLegacy(const std::string& key,std::string& json) {
    const std::string* path=nullptr;
    if(key=="online")path=&paths_.online; else if(key=="timedBans")path=&paths_.timedBans;
    else if(key=="knownPlayers")path=&paths_.knownPlayers; else return Failure("unknown legacy key: "+key,key);
    bool found=false;auto r=Read(*path,json,true,&found,8u*1024u*1024u); if(!r)return Failure(r.error,key);
    if(!found) { json="[]";Recover(key); return {true,{}}; }
    if(json.empty()||json.find_first_not_of(" \t\r\n")==std::string::npos){
        if(key=="timedBans")return Failure("corrupt timedBans store: empty existing file "+*path,key);
        json="[]";Recover(key);return {true,{}};
    }
    if(!WithinDepth(json))return Failure("corrupt "+key+" store: JSON exceeds depth 64 or is malformed",key);
    Json j=Json::parse(json,nullptr,false);
    if(!j.is_array())return Failure("corrupt "+key+" store: "+*path,key);
    if(key=="timedBans"){
        for(const auto& row:j){
            if(!row.is_object()||!row.contains("gameId")||!row["gameId"].is_string()||
               row["gameId"].get<std::string>().empty()||row["gameId"].get<std::string>().size()>1024||!row.contains("expiresAt")||
               !row["expiresAt"].is_string()||!ValidExpiry(row["expiresAt"].get<std::string>()))
                return Failure("corrupt timedBans store: invalid row in "+*path,key);
        }
    }else{
        Json filtered=Json::array();for(const auto& row:j)if(row.is_object()&&row.contains("gameId")&&row["gameId"].is_string()&&row.contains("name")&&row["name"].is_string())filtered.push_back(row);
        json=filtered.dump();
    }
    Recover(key);return {true,{}};
}
Result Store::SaveLegacy(const std::string& key,const std::string& json) {
    const std::string* path=nullptr;
    if(key=="online")path=&paths_.online; else if(key=="timedBans")path=&paths_.timedBans;
    else if(key=="knownPlayers")path=&paths_.knownPlayers; else return Failure("unknown legacy key: "+key,key);
    Json j=Json::parse(json,nullptr,false); if(!j.is_array())return Failure("invalid "+key+" JSON",key);
    auto r=AtomicWrite(*path,json);if(!r)return Failure(r.error,key);Recover(key);return r;
}
Result Store::SaveKnownPlayers(const std::string& json){
    Json j=Json::parse(json,nullptr,false);if(!j.is_array()||j.size()>500||json.size()>8u*1024u*1024u)
        return Failure("invalid or oversized knownPlayers JSON","knownPlayers");
    Snapshot n=snapshot_;n.derivedKnownJson=json;n.hasDerivedState=true;
    auto r=Commit(std::move(n));if(!r)return r;
    auto mirror=AtomicWrite(paths_.knownPlayers,json);if(!mirror)return Failure(mirror.error,"knownPlayers");Recover("knownPlayers");return mirror;
}
Result Store::BeginBanIntent(const std::string& json) {
    auto j=Json::parse(json,nullptr,false); if(!j.is_object())return Failure("invalid ban intent JSON","banIntent");
    std::string raw;bool found=false;auto old=Read(paths_.banIntent,raw,true,&found,8u*1024u*1024u);if(!old)return Failure(old.error,"banIntent");
    if(found&&raw.empty())return Failure("corrupt ban intent journal: empty existing file","banIntent");
    Json intents=Json::array();if(!raw.empty()){if(!WithinDepth(raw))return Failure("corrupt ban intent journal: JSON depth exceeds 64","banIntent");auto prior=Json::parse(raw,nullptr,false);if(!prior.is_object()||!prior.contains("intents")||!prior["intents"].is_array())return Failure("corrupt ban intent journal","banIntent");intents=prior["intents"];}
    for(auto& x:intents)if(x.value("requestId",std::string())==j.value("requestId",std::string())){
        // A prior attempt may have renamed the journal but failed the directory
        // fsync. Prove that visible entry durable before allowing the mutation.
        auto synced=FsyncDirectory(paths_.banIntent);
        if(!synced)return Failure(synced.error,"banIntent");
        Recover("banIntent");return {true,{}};
    }
    intents.push_back(j);auto r=AtomicWrite(paths_.banIntent,Json{{"version",1},{"intents",intents}}.dump());if(!r)return Failure(r.error,"banIntent");Recover("banIntent");return r;
}
Result Store::ReadBanIntent(std::string& json) const {
    bool found=false;auto read=Read(paths_.banIntent,json,true,&found,8u*1024u*1024u);
    if(!read)return read;
    if(found&&(json.empty()||!WithinDepth(json)))return {false,"corrupt ban intent journal: empty or JSON depth exceeds 64"};
    return {true,{}};
}
Result Store::FinishBanIntent(const std::string& requestId) {
    std::string raw;bool found=false;auto rr=Read(paths_.banIntent,raw,true,&found,8u*1024u*1024u);if(!rr)return Failure(rr.error,"banIntent");
    if(found&&raw.empty())return Failure("corrupt ban intent journal: empty existing file","banIntent");
    if(raw.empty()){Recover("banIntent");return {true,{}};}
    if(!WithinDepth(raw))return Failure("corrupt ban intent journal: JSON depth exceeds 64","banIntent");
    Json j=Json::parse(raw,nullptr,false);if(!j.is_object()||!j.contains("intents")||!j["intents"].is_array())return Failure("corrupt ban intent journal","banIntent");
    Json keep=Json::array();for(auto& x:j["intents"])if(x.value("requestId",std::string())!=requestId)keep.push_back(x);
    if(!keep.empty()){auto w=AtomicWrite(paths_.banIntent,Json{{"version",1},{"intents",keep}}.dump());if(!w)return Failure(w.error,"banIntent");Recover("banIntent");return w;}
    if(unlink(paths_.banIntent.c_str())!=0 && errno!=ENOENT)return Failure(Error("unlink",paths_.banIntent),"banIntent");
    const std::string dir=fs::path(paths_.banIntent).parent_path().string();
    int fd=open(dir.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    if(fd<0)return Failure(Error("open directory",dir),"banIntent");
    if(fsync(fd)!=0){auto e=Error("fsync directory",dir);close(fd);return Failure(e,"banIntent");}
    close(fd);Recover("banIntent");return {true,{}};
}
Result Store::FinishBanIntentsForPlayer(const std::string& gameId) {
    std::string raw;bool found=false;
    auto rr=Read(paths_.banIntent,raw,true,&found,8u*1024u*1024u);
    if(!rr)return Failure(rr.error,"banIntent");
    if(found&&raw.empty())return Failure("corrupt ban intent journal: empty existing file","banIntent");
    if(raw.empty()){Recover("banIntent");return {true,{}};}
    if(!WithinDepth(raw))return Failure("corrupt ban intent journal: JSON depth exceeds 64","banIntent");
    Json j=Json::parse(raw,nullptr,false);
    if(!j.is_object()||!j.contains("intents")||!j["intents"].is_array())
        return Failure("corrupt ban intent journal","banIntent");
    Json keep=Json::array();
    for(const auto& intent:j["intents"])
        if(!intent.is_object()||intent.value("gameId",std::string())!=gameId)keep.push_back(intent);
    if(keep.size()==j["intents"].size()){Recover("banIntent");return {true,{}};}
    if(!keep.empty()){
        auto write=AtomicWrite(paths_.banIntent,Json{{"version",1},{"intents",keep}}.dump());
        if(!write)return Failure(write.error,"banIntent");
        Recover("banIntent");return write;
    }
    if(unlink(paths_.banIntent.c_str())!=0&&errno!=ENOENT)
        return Failure(Error("unlink",paths_.banIntent),"banIntent");
    const std::string dir=fs::path(paths_.banIntent).parent_path().string();
    int fd=open(dir.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    if(fd<0)return Failure(Error("open directory",dir),"banIntent");
    if(fsync(fd)!=0){auto e=Error("fsync directory",dir);close(fd);return Failure(e,"banIntent");}
    close(fd);Recover("banIntent");return {true,{}};
}
} // namespace NativePersistence
