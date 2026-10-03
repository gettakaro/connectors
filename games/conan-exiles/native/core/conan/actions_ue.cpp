#include "conan/actions_ue.h"

#include "common.h"
#include "conan/identity.h"
#include "conan/text.h"
#include "gamethread.h"
#include "ue/mem.h"

#include <algorithm>
#include <cstring>
#include <chrono>
#include <mutex>
#include <thread>

namespace conan {
namespace rx {

namespace {
constexpr int kJobTimeoutMs = 2000;
constexpr int kMaxDiscoverySteps = 1024;
constexpr int kWalkStep = 16384;  // objects per game-thread job (~0.5 ms on rig 349)
constexpr uint32_t kBlockBytes = 0x20000;

char LowerAscii(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

// The ANSI bytes of a name entry, or nullptr (wide, out of range, not yet written).
const char* EntryChars(uint32_t index, unsigned& len) {
    len = 0;
    const uintptr_t nb = UE::NameBlocksAddress();
    if (!nb) return nullptr;
    const uint32_t cur = Rd<uint32_t>(nb - 8), cursor = Rd<uint32_t>(nb - 4);
    const uint32_t block = index >> 16, off = (index & 0xffff) * 2;
    if (block > cur || cur > 8192) return nullptr;
    const uint32_t limit = block < cur ? kBlockBytes : cursor;
    if (off + 2 > limit) return nullptr;
    const uintptr_t base = Rd<uintptr_t>(nb + 8 * (uintptr_t)block);
    if (!base) return nullptr;
    const uint16_t h = Rd<uint16_t>(base + off);
    const unsigned n = h >> 6;
    if (!n || (h & 1) || off + 2 + n > limit) return nullptr;
    len = n;
    return (const char*)(base + off + 2);
}

bool EqualsAnsi(const char* a, unsigned alen, const char* b) {
    size_t blen = strlen(b);
    if (alen != blen) return false;
    for (unsigned i = 0; i < alen; i++)
        if (LowerAscii(a[i]) != LowerAscii(b[i])) return false;
    return true;
}

uintptr_t ItemAt(int32_t index) {
    const uintptr_t arr = UE::ObjObjectsAddress();
    if (!arr) return 0;
    const uintptr_t objects = Rd<uintptr_t>(arr);
    const int32_t num = Rd<int32_t>(arr + 8);
    if (!objects || index < 0 || index >= num) return 0;
    const uintptr_t chunk = Rd<uintptr_t>(objects + 8 * (uintptr_t)(index / kChunkItems));
    return chunk ? chunk + (uintptr_t)(index % kChunkItems) * kItemStride : 0;
}

std::string LowerStr(std::string s) {
    for (auto& c : s) c = LowerAscii(c);
    return s;
}
}  // namespace

std::string NameText(uint32_t index) {
    unsigned len = 0;
    const char* p = EntryChars(index, len);
    return p ? std::string(p, len) : std::string();
}

bool NameIs(uintptr_t fnameAddr, const char* name) {
    if (Rd<uint32_t>(fnameAddr + 4) != 0) return false;
    unsigned len = 0;
    const char* p = EntryChars(Rd<uint32_t>(fnameAddr), len);
    return p && EqualsAnsi(p, len, name);
}

std::vector<int64_t> FindNameIndices(const std::vector<std::string>& names) {
    std::vector<int64_t> found(names.size(), -1);
    const uintptr_t nb = UE::NameBlocksAddress();
    if (!nb) return found;
    size_t missing = names.size();
    const uint32_t cur = Rd<uint32_t>(nb - 8), cursor = Rd<uint32_t>(nb - 4);
    if (cur > 8192) return found;
    for (uint32_t b = 0; b <= cur && missing; ++b) {
        const uintptr_t base = Rd<uintptr_t>(nb + 8 * (uintptr_t)b);
        if (!base) continue;
        const uint32_t size = b < cur ? kBlockBytes : cursor;
        for (uint32_t off = 0; off + 2 <= size;) {
            const uint16_t h = Rd<uint16_t>(base + off);
            const unsigned len = h >> 6;
            if (!len) break;
            const bool wide = h & 1;
            if (!wide) {
                for (size_t i = 0; i < names.size(); i++)
                    if (found[i] < 0 && EqualsAnsi((const char*)(base + off + 2), len, names[i].c_str())) {
                        found[i] = (int64_t)((b << 16) | (off / 2));
                        missing--;
                    }
            }
            off += 2 + len * (wide ? 2 : 1);
            off += off & 1;
        }
    }
    return found;
}

bool Alive(uintptr_t obj) {
    if (!obj) return false;
    const uintptr_t item = ItemAt(Rd<int32_t>(obj + kObjIndex));
    if (!item || Rd<uintptr_t>(item + kItemObject) != obj) return false;
    if (Rd<uint32_t>(item + kItemFlags) & kInternalDead) return false;
    return !(Rd<uint32_t>(obj + kObjFlags) & (kRfBeginDestroyed | kRfFinishDestroyed));
}

uintptr_t ClassOf(uintptr_t obj) { return obj ? Rd<uintptr_t>(obj + kObjClass) : 0; }

bool IsA(uintptr_t obj, const char* className) {
    int depth = 0;
    for (uintptr_t c = ClassOf(obj); c && depth < 64; c = Rd<uintptr_t>(c + kStructSuper), depth++)
        if (NameIs(c + kObjName, className)) return true;
    return false;
}

// ---- reflection ----
const Param* Func::P(const char* n) const {
    for (auto& p : params)
        if (EqualsIgnoreCase(p.first, n)) return &p.second;
    return nullptr;
}

uintptr_t FindFunction(uintptr_t cls, const char* name) {
    int depth = 0;
    for (uintptr_t c = cls; c && depth < 64; c = Rd<uintptr_t>(c + kStructSuper), depth++) {
        int n = 0;
        for (uintptr_t f = Rd<uintptr_t>(c + kStructChildren); f && n < 8192; f = Rd<uintptr_t>(f + kFieldNext), n++) {
            if (!NameIs(f + kObjName, name)) continue;
            const uintptr_t fc = Rd<uintptr_t>(f + kObjClass);
            if (fc && (NameIs(fc + kObjName, "Function") || NameIs(fc + kObjName, "DelegateFunction"))) return f;
        }
    }
    return 0;
}

namespace {
void ReadParam(uintptr_t p, Param& out) {
    const uintptr_t fc = Rd<uintptr_t>(p + kFFieldClass);
    out.type = fc ? NameText(Rd<uint32_t>(fc + kFFieldClassName)) : "";
    out.offset = Rd<int32_t>(p + kPropOffset);
    out.size = Rd<int32_t>(p + kPropElementSize);
    out.flags = Rd<uint64_t>(p + kPropFlags);
    if (out.type == "BoolProperty") {
        out.boolByteOffset = Rd<uint8_t>(p + kBoolByteOffset);
        out.boolByteMask = Rd<uint8_t>(p + kBoolByteMask);
        out.boolFieldMask = Rd<uint8_t>(p + kBoolFieldMask);
    }
}
}  // namespace

bool LoadFunc(uintptr_t fn, Func& out) {
    out = Func();
    if (!fn) return false;
    out.fn = fn;
    out.name = NameText(Rd<uint32_t>(fn + kObjName));
    out.parmsSize = Rd<uint16_t>(fn + kFuncParmsSize);
    int n = 0;
    for (uintptr_t p = Rd<uintptr_t>(fn + kStructChildProps); p && n < 256; p = Rd<uintptr_t>(p + kFFieldNext), n++) {
        Param pr;
        ReadParam(p, pr);
        out.params.emplace_back(NameText(Rd<uint32_t>(p + kFFieldName)), pr);
    }
    return true;
}

bool ResolveFunc(uintptr_t cls, const char* name, const std::vector<Expect>& expect, Func& out, std::string& error) {
    const uintptr_t fn = FindFunction(cls, name);
    if (!fn) {
        error = std::string("UFunction ") + name + " not found on " + NameText(Rd<uint32_t>(cls + kObjName));
        return false;
    }
    LoadFunc(fn, out);
    for (auto& e : expect) {
        const Param* p = out.P(e.name);
        if (!p || p->type != e.type || p->size != e.size || p->offset < 0 || p->offset + p->size > out.parmsSize) {
            error = std::string("UFunction ") + name + " parameter " + e.name + " is not " + e.type + "/" +
                    std::to_string(e.size) + " on this build";
            return false;
        }
    }
    return true;
}

bool FindProperty(uintptr_t cls, const char* name, const char* type, int32_t size, Param& out) {
    int depth = 0;
    for (uintptr_t c = cls; c && depth < 64; c = Rd<uintptr_t>(c + kStructSuper), depth++) {
        int n = 0;
        for (uintptr_t p = Rd<uintptr_t>(c + kStructChildProps); p && n < 8192; p = Rd<uintptr_t>(p + kFFieldNext), n++) {
            if (!NameIs(p + kFFieldName, name)) continue;
            ReadParam(p, out);
            return out.type == type && out.size == size && out.offset >= 0;
        }
    }
    return false;
}

// ---- frame ----
Frame::Frame(const Func& f) : f_(f), storage_((size_t)f.parmsSize + 32, 0) {
    buf_ = (uint8_t*)(((uintptr_t)storage_.data() + 15) & ~(uintptr_t)15);
}

const Param* Frame::Need(const char* name, const char* type) {
    const Param* p = f_.P(name);
    if (!p || p->offset < 0 || p->offset + p->size > f_.parmsSize || (type && p->type != type)) {
        if (error_.empty())
            error_ = f_.name + "." + name + (p ? " has type " + p->type + ", expected " + (type ? type : "?")
                                              : std::string(" is not a parameter"));
        return nullptr;
    }
    return p;
}

Frame& Frame::Int(const char* name, int32_t v) {
    if (auto p = Need(name, "IntProperty")) memcpy(buf_ + p->offset, &v, 4);
    return *this;
}
Frame& Frame::Float(const char* name, float v) {
    if (auto p = Need(name, "FloatProperty")) memcpy(buf_ + p->offset, &v, 4);
    return *this;
}
Frame& Frame::Bool(const char* name, bool v) {
    if (auto p = Need(name, "BoolProperty")) {
        uint8_t& b = buf_[p->offset + p->boolByteOffset];
        const uint8_t mask = p->boolByteMask ? p->boolByteMask : 1;
        const uint8_t field = p->boolFieldMask ? p->boolFieldMask : 0xFF;
        b = (uint8_t)((b & ~field) | (v ? mask : 0));
    }
    return *this;
}
Frame& Frame::Obj(const char* name, uintptr_t v) {
    if (auto p = Need(name, "ObjectProperty")) memcpy(buf_ + p->offset, &v, 8);
    return *this;
}
Frame& Frame::NameNone(const char* name) {
    if (auto p = Need(name, "NameProperty")) memset(buf_ + p->offset, 0, (size_t)p->size);
    return *this;
}
Frame& Frame::Str(const char* name, std::u16string& s) {
    if (auto p = Need(name, "StrProperty")) {
        uintptr_t data = 0;
        int32_t num = 0;
        if (!s.empty()) {
            if (s.back() != u'\0') s.push_back(u'\0');
            data = (uintptr_t)s.data();
            num = (int32_t)s.size();
        }
        memcpy(buf_ + p->offset, &data, 8);
        memcpy(buf_ + p->offset + 8, &num, 4);
        memcpy(buf_ + p->offset + 12, &num, 4);
    }
    return *this;
}
Frame& Frame::Vec(const char* name, double x, double y, double z) {
    if (auto p = Need(name, "StructProperty")) {
        if (p->size != 24) {
            if (error_.empty()) error_ = f_.name + "." + name + " is not a double vector";
            return *this;
        }
        double v[3] = {x, y, z};
        memcpy(buf_ + p->offset, v, 24);
    }
    return *this;
}
Frame& Frame::Raw(const char* name, const void* bytes, size_t n) {
    if (auto p = Need(name, nullptr)) {
        if ((size_t)p->size != n) {
            if (error_.empty()) error_ = f_.name + "." + name + " size mismatch";
            return *this;
        }
        memcpy(buf_ + p->offset, bytes, n);
    }
    return *this;
}

const uint8_t* Frame::At(const char* name) const {
    const Param* p = f_.P(name);
    return p && p->offset >= 0 && p->offset + p->size <= f_.parmsSize ? buf_ + p->offset : nullptr;
}
int32_t Frame::GetInt(const char* name) const {
    const uint8_t* a = At(name);
    int32_t v = 0;
    if (a) memcpy(&v, a, 4);
    return v;
}
int64_t Frame::GetInt64(const char* name) const {
    const uint8_t* a = At(name);
    int64_t v = 0;
    if (a) memcpy(&v, a, 8);
    return v;
}
bool Frame::GetBool(const char* name) const {
    const Param* p = f_.P(name);
    if (!p || p->offset < 0) return false;
    return (buf_[p->offset + p->boolByteOffset] & (p->boolByteMask ? p->boolByteMask : 1)) != 0;
}
uintptr_t Frame::GetObj(const char* name) const {
    const uint8_t* a = At(name);
    uintptr_t v = 0;
    if (a) memcpy(&v, a, 8);
    return v;
}
void Frame::GetVec(const char* name, double& x, double& y, double& z) const {
    double v[3] = {0, 0, 0};
    if (const uint8_t* a = At(name)) memcpy(v, a, 24);
    x = v[0];
    y = v[1];
    z = v[2];
}

bool Call(uintptr_t obj, const Func& f, Frame& frame, std::string* error) {
    if (!frame.error().empty()) {
        if (error) *error = frame.error();
        return false;
    }
    if (!f.fn || !Alive(obj)) {
        if (error) *error = "target object of " + f.name + " is gone";
        return false;
    }
    UE::CallProcessEvent((void*)obj, (void*)f.fn, frame.data());
    return true;
}

// ---- world ----
bool EnsureWorld(std::string& error) {
    bool names = false, ready = false;
    if (!GameThread::Run([&] { names = UE::ResolveNames(); }, kJobTimeoutMs)) {
        error = "game thread did not respond";
        return false;
    }
    if (!names) {
        error = "Conan server is not ready: engine names not loaded yet";
        return false;
    }
    if (!GameThread::Run([&] { ready = UE::Ready(); }, kJobTimeoutMs)) {
        error = "game thread did not respond";
        return false;
    }
    if (ready) return true;
    GameThread::Run([] { UE::ResetDiscovery(); }, kJobTimeoutMs);
    for (int i = 0; i < kMaxDiscoverySteps; i++) {
        bool done = false;
        if (!GameThread::Run([&] { done = UE::DiscoverStep(); }, kJobTimeoutMs)) {
            error = "game thread did not respond";
            return false;
        }
        if (done) break;
    }
    std::string why = "game thread did not respond";
    GameThread::Run([&] { ready = UE::Ready(); why = UE::DiscoveryError(); }, kJobTimeoutMs);
    if (!ready) error = "Conan server is not ready: " + why;
    return ready;
}

namespace {
std::mutex& LookupLock() {
    static std::mutex* m = new std::mutex;
    return *m;
}
Lookups g_lookups;
bool g_haveLookups = false;
std::unordered_map<std::string, int32_t>* g_itemNames = nullptr;
}  // namespace

bool EnsureLookups(Lookups& out, std::string& error) {
    std::lock_guard<std::mutex> g(LookupLock());
    if (g_haveLookups) {
        bool alive = false;
        GameThread::Run([&] { alive = Alive(g_lookups.systemLibrary) && Alive(g_lookups.textLibrary); }, kJobTimeoutMs);
        if (alive) {
            out = g_lookups;
            return true;
        }
        g_haveLookups = false;
    }
    const auto idx = FindNameIndices({"Default__KismetSystemLibrary", "Default__KismetTextLibrary", "DataTable"});
    if (idx[0] < 0 || idx[1] < 0 || idx[2] < 0) {
        error = "engine names for the Kismet libraries / DataTable not found";
        return false;
    }
    Lookups l;
    std::vector<uintptr_t> tables;
    int32_t next = 0, num = 1;
    uint64_t ns = 0;
    while (next < num) {
        bool ran = GameThread::Run(
            [&] {
                const uint64_t t0 = NowNs();
                const uintptr_t arr = UE::ObjObjectsAddress();
                const uintptr_t objects = Rd<uintptr_t>(arr);
                num = Rd<int32_t>(arr + 8);
                const int32_t end = std::min(next + kWalkStep, num);
                for (int32_t i = next; objects && i < end; i++) {
                    const uintptr_t chunk = Rd<uintptr_t>(objects + 8 * (uintptr_t)(i / kChunkItems));
                    if (!chunk) {
                        i = (i / kChunkItems + 1) * kChunkItems - 1;
                        continue;
                    }
                    const uintptr_t item = chunk + (uintptr_t)(i % kChunkItems) * kItemStride;
                    const uintptr_t o = Rd<uintptr_t>(item + kItemObject);
                    if (!o || (Rd<uint32_t>(item + kItemFlags) & kInternalDead)) continue;
                    const uint32_t ni = Rd<uint32_t>(o + kObjName);
                    if (Rd<uint32_t>(o + kObjName + 4) == 0) {
                        if (ni == (uint32_t)idx[0]) l.systemLibrary = o;
                        else if (ni == (uint32_t)idx[1]) l.textLibrary = o;
                    }
                    const uintptr_t cls = Rd<uintptr_t>(o + kObjClass);
                    if (cls && Rd<uint32_t>(cls + kObjName) == (uint32_t)idx[2] && Rd<uint32_t>(cls + kObjName + 4) == 0)
                        tables.push_back(o);
                }
                next = end;
                ns += NowNs() - t0;
            },
            kJobTimeoutMs);
        if (!ran) {
            error = "game thread did not respond during the object walk";
            return false;
        }
        // The detour drains jobs on every ProcessEvent, so without a pause consecutive slices
        // would land in one frame; 10 ms spreads the ~45 ms walk over many frames.
        if (next < num) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    l.walkMs = ns / 1e6;
    for (uintptr_t t : tables)
        if (LowerStr(NameText(Rd<uint32_t>(t + kObjName))).find("itemnametotemplateid") != std::string::npos)
            l.nameTables.push_back(t);
    NativeLog("lookups: %d objects walked in %.2f ms game-thread total; KismetSystemLibrary=%s KismetTextLibrary=%s "
              "item-name tables=%zu",
              num, l.walkMs, l.systemLibrary ? "yes" : "no", l.textLibrary ? "yes" : "no", l.nameTables.size());
    if (!l.systemLibrary || !l.textLibrary) {
        error = "KismetSystemLibrary / KismetTextLibrary default objects not found";
        return false;
    }
    g_lookups = l;
    g_haveLookups = true;
    out = l;
    return true;
}

bool ItemNameMap(const std::unordered_map<std::string, int32_t>*& out, std::string& error) {
    {
        std::lock_guard<std::mutex> g(LookupLock());
        if (g_itemNames) {
            out = g_itemNames;
            return true;
        }
    }
    Lookups l;
    if (!EnsureLookups(l, error)) return false;
    struct Row {
        uint32_t idx, number;
        int32_t id;
    };
    std::vector<Row> rows;
    bool ran = GameThread::Run(
        [&] {
            for (uintptr_t t : l.nameTables) {
                if (!Alive(t)) continue;
                const uintptr_t rs = Rd<uintptr_t>(t + kDtRowStruct);
                if (!rs || Rd<int32_t>(rs + kStructSize) != 4) continue;  // ItemNameToTemplateIDStruct: int @0
                const uintptr_t data = Rd<uintptr_t>(t + kDtData);
                const int32_t num = Rd<int32_t>(t + kDtNum);
                const int32_t numFree = Rd<int32_t>(t + kDtNumFree);
                if (!data || num <= 0 || num > 200000) continue;
                uintptr_t bits = Rd<uintptr_t>(t + kDtFlagsSecondary);
                if (!bits) bits = t + kDtFlagsInline;
                for (int32_t i = 0; i < num; i++) {
                    if (numFree > 0 && !((Rd<uint32_t>(bits + 4 * (uintptr_t)(i / 32)) >> (i % 32)) & 1)) continue;
                    const uintptr_t e = data + (uintptr_t)i * kDtStride;
                    const uintptr_t row = Rd<uintptr_t>(e + 8);
                    if (!row) continue;
                    rows.push_back({Rd<uint32_t>(e), Rd<uint32_t>(e + 4), Rd<int32_t>(row)});
                }
            }
        },
        kJobTimeoutMs);
    if (!ran) {
        error = "game thread did not respond while reading the item-name tables";
        return false;
    }
    auto* map = new std::unordered_map<std::string, int32_t>;
    for (auto& r : rows) {
        std::string n = NameText(r.idx);
        if (n.empty()) continue;
        if (r.number) n += "_" + std::to_string(r.number - 1);
        map->emplace(LowerStr(n), r.id);
    }
    NativeLog("item names: %zu rows from %zu tables -> %zu codes", rows.size(), l.nameTables.size(), map->size());
    std::lock_guard<std::mutex> g(LookupLock());
    if (g_itemNames) {
        delete map;
    } else {
        if (map->empty()) {
            delete map;
            error = "no ItemNameToTemplateID rows found";
            return false;
        }
        g_itemNames = map;
    }
    out = g_itemNames;
    return true;
}

std::string Steam64Of(uintptr_t pc, const std::string& userIdFromUrl) {
    Param psProp, uidProp;
    UniqueIdProbe uid;
    if (FindProperty(ClassOf(pc), "PlayerState", "ObjectProperty", 8, psProp)) {
        const uintptr_t ps = Rd<uintptr_t>(pc + (uintptr_t)psProp.offset);
        if (Alive(ps) && FindProperty(ClassOf(ps), "UniqueID", "StructProperty", 48, uidProp))
            uid = Steam64FromUniqueId(UE::SelfMem(), ps + (uintptr_t)uidProp.offset);
    }
    return GameIdFrom(uid, userIdFromUrl);
}

std::vector<OnlinePc> OnlinePcs() {
    std::vector<OnlinePc> out;
    for (const auto& c : UE::OnlineControllers()) out.push_back({c.object, Steam64Of(c.object, c.userId), c.playerName});
    return out;
}

uintptr_t ControllerFor(const std::string& who) {
    for (const auto& c : OnlinePcs()) {
        if (IsSteam64(who) ? c.steam64 == who : (c.steam64 == who || EqualsIgnoreCase(c.name, who))) return c.pc;
    }
    return 0;
}

}  // namespace rx
}  // namespace conan
