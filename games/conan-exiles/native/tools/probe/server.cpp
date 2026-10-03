// Loopback HTTP REPL (127.0.0.1 only, token in <ProbeDir>/token). One thread per connection;
// every request is answered with JSON. Only /call runs anything on the game thread.
#include "probe.h"

#include "gamethread.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <map>
#include <unordered_map>
#include <thread>

namespace Probe {
namespace {
std::string g_token;
std::atomic<int> g_conns{0};
std::atomic<uint64_t> g_calls{0};

struct Req {
    std::string method, path, body;
    std::map<std::string, std::string> query, headers;
    std::string q(const std::string& k, const std::string& def = "") const {
        auto it = query.find(k);
        return it == query.end() ? def : it->second;
    }
    long qi(const std::string& k, long def) const {
        auto it = query.find(k);
        return it == query.end() || it->second.empty() ? def : strtol(it->second.c_str(), nullptr, 0);
    }
};

std::string UrlDecode(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '+') o += ' ';
        else if (s[i] == '%' && i + 2 < s.size()) {
            o += (char)strtol(s.substr(i + 1, 2).c_str(), nullptr, 16);
            i += 2;
        } else o += s[i];
    }
    return o;
}

std::string Err(const std::string& msg) { return "{\"ok\":false,\"error\":" + JsonStr(msg) + "}"; }

std::string Lower(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

bool TokenOk(const Req& r) {
    auto it = r.headers.find("x-probe-token");
    std::string t = it != r.headers.end() ? it->second : r.q("token");
    if (t.size() != g_token.size()) return false;
    unsigned diff = 0;
    for (size_t i = 0; i < t.size(); i++) diff |= (unsigned)(t[i] ^ g_token[i]);
    return diff == 0;
}

void WriteObjInfo(JW& w, uintptr_t o) {
    Obj ob;
    ReadObj(o, ob);
    w.kstr("addr", Hex(o));
    w.kstr("name", ObjName(o));
    w.kstr("class", ClassNameOf(o));
    w.kstr("path", ObjPath(o));
    w.kstr("flags", Hex(ob.flags));
    w.knum("index", ob.index);
}

// ---------------------------------------------------------------- handlers
std::string Health() {
    JW w;
    w.beginObj();
    w.kbool("ok", true);
    w.kstr("version", kVersion);
    w.kbool("safeReads", Mem::Safe());
    w.knum("objects", NumObjects());
    std::string err;
    w.kbool("coreReady", EnsureCore(err));
    w.knum("calls", (long long)g_calls.load());
    w.kstr("probeDir", ProbeDir());
    w.endObj();
    return w.s;
}

std::string Names(const Req& r) {
    JW w;
    w.beginObj();
    w.kbool("ok", true);
    w.key("names");
    w.beginArr();
    for (auto& n : SearchNames(r.q("q"), (size_t)r.qi("limit", 200))) w.str(n);
    w.endArr();
    w.endObj();
    return w.s;
}

// Structs, enums and functions whose name contains q (walk of the object array).
std::string Search(const Req& r, bool functions) {
    std::string err;
    if (!EnsureCore(err)) return Err(err);
    const std::string q = Lower(r.q("q")), owner = Lower(r.q("owner"));
    const size_t limit = (size_t)r.qi("limit", 200);
    const Meta& m = Core();
    std::unordered_map<uintptr_t, int> kind;
    JW w;
    w.beginObj();
    w.kbool("ok", true);
    w.key("results");
    w.beginArr();
    size_t n = 0;
    ForEachObject([&](uintptr_t o, uint32_t fl) {
        if (n >= limit || (fl & 0x30000000)) return;
        Obj ob;
        if (!ReadObj(o, ob) || !ob.cls) return;
        auto it = kind.find(ob.cls);
        if (it == kind.end())
            it = kind.emplace(ob.cls, IsA(ob.cls, m.Function) ? 2 : (IsA(ob.cls, m.Struct) || IsA(ob.cls, m.Enum)) ? 1 : 0).first;
        if (it->second != (functions ? 2 : 1)) return;
        std::string name = NameStr(ob.nameIdx);
        if (Lower(name).find(q) == std::string::npos) return;
        if (functions && !owner.empty() && Lower(ObjName(ob.outer)).find(owner) == std::string::npos) return;
        n++;
        w.beginObj();
        w.kstr("name", name);
        w.kstr("kind", ObjName(ob.cls));
        w.kstr("path", ObjPath(o));
        w.kstr("addr", Hex(o));
        if (functions) {
            FuncInfo fi;
            if (ReadFunc(o, fi)) {
                w.kstr("owner", fi.owner);
                w.kstr("flagNames", FuncFlagNames(fi.flags));
                w.knum("parmsSize", fi.parmsSize);
                w.key("params");
                w.beginArr();
                for (auto& p : fi.params) {
                    std::string pf = ParamFlagNames(p.flags);
                    w.str(p.type + " " + p.name + " @" + std::to_string(p.offset) + (pf.empty() ? "" : " [" + pf + "]"));
                }
                w.endArr();
            }
        }
        w.endObj();
    });
    w.endArr();
    w.endObj();
    return w.s;
}

std::string ClassInfo(const Req& r) {
    std::string err;
    if (!EnsureCore(err)) return Err(err);
    uintptr_t t = r.q("addr").empty() ? FindType(r.q("name")) : ParseAddr(r.q("addr"));
    if (!t) return Err("type not found: " + r.q("name"));
    JW w;
    if (IsA(Mem::Rd<uintptr_t>(t + 0x10), Core().Enum)) WriteEnum(w, t);
    else WriteStruct(w, t, -1, FindCDO(t));
    if (r.q("supers") == "1") {  // own entry plus every super, as one array
        JW a;
        a.beginArr();
        a.raw(w.s);
        int d = 0;
        for (uintptr_t c = SuperOf(t); c && d < 64; c = SuperOf(c), d++) {
            JW s;
            WriteStruct(s, c, -1, 0);
            a.raw(s.s);
        }
        a.endArr();
        return a.s;
    }
    return w.s;
}

std::string Find(const Req& r) {
    std::string err;
    if (!EnsureCore(err)) return Err(err);
    const std::string clsName = r.q("class"), name = Lower(r.q("name"));
    const bool exact = r.q("exact") == "1", cdo = r.q("cdo") == "1";
    const size_t limit = (size_t)r.qi("limit", 50);
    uintptr_t cls = 0;
    if (!clsName.empty() && !exact) {
        cls = FindType(clsName);
        if (!cls) return Err("class not found: " + clsName);
    }
    std::unordered_map<uintptr_t, bool> match;
    JW w;
    w.beginObj();
    w.kbool("ok", true);
    w.key("objects");
    w.beginArr();
    size_t n = 0, total = 0;
    ForEachObject([&](uintptr_t o, uint32_t fl) {
        if (fl & 0x30000000) return;
        Obj ob;
        if (!ReadObj(o, ob) || !ob.cls) return;
        if ((ob.flags & 0x30) && !cdo) return;
        if (!clsName.empty()) {
            auto it = match.find(ob.cls);
            if (it == match.end())
                it = match.emplace(ob.cls, exact ? strcasecmp(ObjName(ob.cls).c_str(), clsName.c_str()) == 0
                                                 : IsA(ob.cls, cls)).first;
            if (!it->second) return;
        }
        if (!name.empty() && Lower(NameStr(ob.nameIdx)).find(name) == std::string::npos) return;
        total++;
        if (n >= limit) return;
        n++;
        w.beginObj();
        WriteObjInfo(w, o);
        w.endObj();
    });
    w.endArr();
    w.knum("total", (long long)total);
    w.endObj();
    return w.s;
}

std::string ObjRead(const Req& r) {
    std::string err;
    uintptr_t o = ResolveObjectSpec(r.q("addr", r.q("object")), err);
    if (!o) return Err(err);
    DecodeOpts opt;
    opt.depth = (int)r.qi("depth", 2);
    opt.arrayLimit = (int)r.qi("arrayLimit", 32);
    std::vector<std::string> want;
    std::string props = r.q("props");
    for (size_t a = 0; a < props.size();) {
        size_t b = props.find(',', a);
        if (b == std::string::npos) b = props.size();
        if (b > a) want.push_back(Lower(props.substr(a, b - a)));
        a = b + 1;
    }
    JW w;
    w.beginObj();
    w.kbool("ok", true);
    WriteObjInfo(w, o);
    w.key("props");
    w.beginObj();
    for (auto& p : PropsOf(Mem::Rd<uintptr_t>(o + 0x10), true)) {
        if (!want.empty() && std::find(want.begin(), want.end(), Lower(p.name)) == want.end()) continue;
        w.key(p.name.c_str());
        w.beginObj();
        w.kstr("type", p.type);
        w.knum("offset", p.offset);
        w.key("value");
        DecodeValue(w, p, o + (uintptr_t)p.offset, opt, 0);
        w.endObj();
    }
    w.endObj();
    w.endObj();
    return w.s;
}

std::string RawRead(const Req& r) {
    uintptr_t a = ParseAddr(r.q("addr"));
    long n = std::min(4096L, std::max(1L, r.qi("size", 64)));
    std::vector<uint8_t> b((size_t)n);
    if (!a || !Mem::Read(a, b.data(), b.size())) return Err("unreadable");
    std::string hex;
    char x[4];
    for (auto c : b) snprintf(x, sizeof x, "%02x", c), hex += x;
    return "{\"ok\":true,\"addr\":" + JsonStr(Hex(a)) + ",\"hex\":\"" + hex + "\"}";
}

std::string Call(const Req& r) {
    JsonValue v;
    if (!JsonParse(r.body, v) || v.type != JsonValue::Object) return Err("body must be a JSON object");
    const JsonValue* os = v.get("object");
    const JsonValue* fs = v.get("function");
    if (!os || !os->isStr() || !fs || !fs->isStr()) return Err("object and function (strings) are required");
    std::string err;
    if (!EnsureCore(err)) return Err(err);
    uintptr_t obj = ResolveObjectSpec(os->str, err);
    if (!obj) return Err(err);
    uintptr_t func = FindFunction(Mem::Rd<uintptr_t>(obj + 0x10), fs->str);
    if (!func) return Err("function " + fs->str + " not found on " + ClassNameOf(obj) + " or its supers");
    FuncInfo fi;
    if (!ReadFunc(func, fi)) return Err("unreadable function");
    Arena arena;
    uint8_t* parms = arena.alloc((size_t)fi.parmsSize + 64);
    const JsonValue* args = v.get("args");
    if (args && args->type == JsonValue::Object) {
        for (auto& kv : args->obj) {
            const Prop* target = nullptr;
            for (auto& p : fi.params)
                if (strcasecmp(p.name.c_str(), kv.first.c_str()) == 0) target = &p;
            if (!target) return Err("function " + fi.name + " has no parameter " + kv.first);
            if (!EncodeValue(*target, kv.second, parms + target->offset, arena, err)) return Err(err);
        }
    }
    int timeoutMs = 5000;
    if (const JsonValue* t = v.get("timeoutMs"); t && t->type == JsonValue::Number) timeoutMs = (int)t->num;
    bool alive = false;
    uint64_t ns = 0;
    const bool ran = GameThread::Run(
        [&] {
            alive = Alive(obj) && Alive(func);
            if (!alive) return;
            timespec a, b;
            clock_gettime(CLOCK_MONOTONIC, &a);
            CallProcessEvent((void*)obj, (void*)func, parms);
            clock_gettime(CLOCK_MONOTONIC, &b);
            ns = (uint64_t)(b.tv_sec - a.tv_sec) * 1000000000ULL + (uint64_t)(b.tv_nsec - a.tv_nsec);
        },
        timeoutMs);
    if (!ran) return Err("game thread did not run the call within " + std::to_string(timeoutMs) + " ms");
    if (!alive) return Err("object or function died before the call ran");
    g_calls++;
    NativeLog("call %s.%s on %s (%s): %.3f ms", fi.owner.c_str(), fi.name.c_str(), ObjName(obj).c_str(),
              Hex(obj).c_str(), ns / 1e6);
    DecodeOpts opt;
    if (const JsonValue* d = v.get("depth"); d && d->type == JsonValue::Number) opt.depth = (int)d->num;
    if (const JsonValue* d = v.get("arrayLimit"); d && d->type == JsonValue::Number) opt.arrayLimit = (int)d->num;
    JW w;
    w.beginObj();
    w.kbool("ok", true);
    w.kstr("function", fi.owner + "." + fi.name);
    w.kstr("flagNames", FuncFlagNames(fi.flags));
    w.key("object");
    w.beginObj();
    WriteObjInfo(w, obj);
    w.endObj();
    w.knum("gameThreadUs", (long long)(ns / 1000));
    w.key("params");
    w.beginObj();
    for (auto& p : fi.params) {
        if (p.flags & 0x400) {
            continue;
        }
        w.key(p.name.c_str());
        DecodeValue(w, p, (uintptr_t)parms + (uintptr_t)p.offset, opt, 0);
    }
    w.endObj();
    w.key("return");
    bool haveRet = false;
    for (auto& p : fi.params)
        if (p.flags & 0x400) {
            DecodeValue(w, p, (uintptr_t)parms + (uintptr_t)p.offset, opt, 0);
            haveRet = true;
        }
    if (!haveRet) w.null();
    w.endObj();
    return w.s;
}

std::string TraceReq(const Req& r) {
    if (r.method == "GET") return "{\"ok\":true,\"traces\":" + Trace::List() + "}";
    if (r.method == "DELETE") return Trace::Remove(r.q("id")) ? "{\"ok\":true}" : Err("no such trace");
    JsonValue v;
    if (!JsonParse(r.body, v) || v.type != JsonValue::Object) return Err("body must be a JSON object");
    std::string err;
    std::string id = Trace::Add(v, err);
    if (id.empty()) return Err(err);
    return "{\"ok\":true,\"id\":" + JsonStr(id) + ",\"file\":" + JsonStr(ProbeDir() + "/trace-" + id + ".log") + "}";
}

std::string DumpReq(const Req& r) {
    if (r.method == "GET") return Dump::Status();
    char b[64];
    time_t now = time(nullptr);
    strftime(b, sizeof b, "reflection-25639945-%Y%m%dT%H%M%SZ.json", gmtime(&now));
    std::string err;
    std::string path = Dump::Start(ProbeDir() + "/" + b, err);
    if (path.empty()) return Err(err);
    return "{\"ok\":true,\"path\":" + JsonStr(path) + "}";
}

std::string Route(const Req& r, int& status) {
    status = 200;
    if (!TokenOk(r)) {
        status = 401;
        return Err("missing or wrong X-Probe-Token");
    }
    const std::string& p = r.path;
    if (p == "/health") return Health();
    if (p == "/names") return Names(r);
    if (p == "/types") return Search(r, false);
    if (p == "/functions") return Search(r, true);
    if (p == "/class") return ClassInfo(r);
    if (p == "/find") return Find(r);
    if (p == "/obj") return ObjRead(r);
    if (p == "/read") return RawRead(r);
    if (p == "/call" && r.method == "POST") return Call(r);
    if (p == "/trace") return TraceReq(r);
    if (p == "/dump") return DumpReq(r);
    status = 404;
    return Err("unknown endpoint " + p);
}

void Serve(int fd) {
    timeval tv{15, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    std::string raw;
    char buf[8192];
    size_t hdrEnd = std::string::npos;
    while (hdrEnd == std::string::npos && raw.size() < 65536) {
        ssize_t n = recv(fd, buf, sizeof buf, 0);
        if (n <= 0) break;
        raw.append(buf, (size_t)n);
        hdrEnd = raw.find("\r\n\r\n");
    }
    Req r;
    int status = 400;
    std::string body = Err("bad request");
    if (hdrEnd != std::string::npos) {
        std::string head = raw.substr(0, hdrEnd);
        size_t le = head.find("\r\n");
        std::string line = head.substr(0, le);
        size_t s1 = line.find(' '), s2 = line.find(' ', s1 + 1);
        if (s1 != std::string::npos && s2 != std::string::npos) {
            r.method = line.substr(0, s1);
            std::string target = line.substr(s1 + 1, s2 - s1 - 1);
            size_t qm = target.find('?');
            r.path = target.substr(0, qm);
            if (qm != std::string::npos) {
                std::string qs = target.substr(qm + 1);
                for (size_t a = 0; a <= qs.size();) {
                    size_t b = qs.find('&', a);
                    if (b == std::string::npos) b = qs.size();
                    std::string kv = qs.substr(a, b - a);
                    size_t eq = kv.find('=');
                    if (!kv.empty()) r.query[UrlDecode(kv.substr(0, eq))] = eq == std::string::npos ? "" : UrlDecode(kv.substr(eq + 1));
                    a = b + 1;
                }
            }
            for (size_t a = le + 2; le != std::string::npos && a < head.size();) {
                size_t b = head.find("\r\n", a);
                if (b == std::string::npos) b = head.size();
                std::string h = head.substr(a, b - a);
                size_t c = h.find(':');
                if (c != std::string::npos) {
                    std::string v = h.substr(c + 1);
                    while (!v.empty() && v[0] == ' ') v.erase(0, 1);
                    r.headers[Lower(h.substr(0, c))] = v;
                }
                a = b + 2;
            }
            size_t len = r.headers.count("content-length") ? strtoul(r.headers["content-length"].c_str(), nullptr, 10) : 0;
            if (len <= (1 << 20)) {
                r.body = raw.substr(hdrEnd + 4);
                while (r.body.size() < len) {
                    ssize_t n = recv(fd, buf, sizeof buf, 0);
                    if (n <= 0) break;
                    r.body.append(buf, (size_t)n);
                }
                body = Route(r, status);
            }
        }
    }
    std::string resp = "HTTP/1.1 " + std::to_string(status) + (status == 200 ? " OK" : " Error") +
                       "\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: " +
                       std::to_string(body.size() + 1) + "\r\n\r\n" + body + "\n";
    for (size_t sent = 0; sent < resp.size();) {
        ssize_t n = send(fd, resp.data() + sent, resp.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) break;
        sent += (size_t)n;
    }
    close(fd);
}

std::string LoadToken() {
    std::string env = EnvOr("TAKARO_PROBE_TOKEN", "");
    if (env.size() >= 16) return env;
    const std::string path = ProbeDir() + "/token";
    if (FILE* f = fopen(path.c_str(), "r")) {
        char b[128] = {0};
        size_t n = fread(b, 1, sizeof b - 1, f);
        fclose(f);
        std::string t(b, n);
        while (!t.empty() && (t.back() == '\n' || t.back() == ' ')) t.pop_back();
        if (t.size() >= 16) return t;
    }
    uint8_t raw[16];
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0 || read(fd, raw, sizeof raw) != (ssize_t)sizeof raw) {
        if (fd >= 0) close(fd);
        return "";
    }
    close(fd);
    std::string t;
    char x[4];
    for (auto c : raw) snprintf(x, sizeof x, "%02x", c), t += x;
    int wf = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (wf >= 0) {
        std::string line = t + "\n";
        if (write(wf, line.data(), line.size()) < 0) {}
        close(wf);
    }
    return t;
}
}  // namespace

bool StartServer(std::string& err) {
    if (!EnsureCore(err)) return false;
    g_token = LoadToken();
    if (g_token.empty()) {
        err = "no token";
        return false;
    }
    int port = atoi(EnvOr("TAKARO_PROBE_PORT", "7979").c_str());
    int s = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(s, (sockaddr*)&a, sizeof a) != 0 || listen(s, 32) != 0) {
        err = "bind 127.0.0.1:" + std::to_string(port) + " failed";
        close(s);
        return false;
    }
    NativeLog("REPL listening on 127.0.0.1:%d (token in %s/token)", port, ProbeDir().c_str());
    std::thread([s] {
        for (;;) {
            int c = accept4(s, nullptr, nullptr, SOCK_CLOEXEC);
            if (c < 0) {
                usleep(10000);
                continue;
            }
            if (g_conns.load() >= 32) {
                close(c);
                continue;
            }
            g_conns++;
            std::thread([c] {
                Serve(c);
                g_conns--;
            }).detach();
        }
    }).detach();
    return true;
}

}  // namespace Probe
