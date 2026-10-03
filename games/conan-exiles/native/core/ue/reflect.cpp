#include "ue/reflect.h"

#include "conan/text.h"

#include <cstring>

namespace UE {

using namespace Layout;

std::string Reflection::Lower(const std::string& s) const {
    std::string o = s;
    for (auto& c : o)
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return o;
}

// ---------------------------------------------------------------- names
std::string Reflection::Name(uint32_t idx) const {
    {
        std::lock_guard<std::mutex> g(lock_);
        auto it = names_.find(idx);
        if (it != names_.end()) return it->second;
    }
    uintptr_t blk = m_.Rd<uintptr_t>(nameBlocks_ + 8 * (uintptr_t)(idx >> 16));
    if (!blk) return "";
    uintptr_t e = blk + (uintptr_t)(idx & 0xffff) * 2;
    uint16_t h = 0;
    if (!m_.Get(e, h)) return "";
    unsigned len = h >> 6;
    if (!len || len > 1024) return "";
    std::string s;
    if (h & 1) {
        std::u16string w(len, u'\0');
        if (!m_.Read(e + 2, &w[0], len * 2)) return "";
        s = conan::Utf16To8(w);
    } else {
        s.resize(len);
        if (!m_.Read(e + 2, &s[0], len)) return "";
    }
    std::lock_guard<std::mutex> g(lock_);
    if (names_.size() < 200000) names_[idx] = s;
    return s;
}

std::string Reflection::FName(uintptr_t at) const {
    uint32_t v[2];
    if (!m_.Read(at, v, sizeof v)) return "";
    std::string s = Name(v[0]);
    if (v[1]) s += "_" + std::to_string(v[1] - 1);
    return s;
}

bool Reflection::FNameIs(uintptr_t at, const char* name) const {
    uint32_t v[2];
    if (!m_.Read(at, v, sizeof v) || v[1] != 0) return false;
    return conan::EqualsIgnoreCase(Name(v[0]), name);
}

// ---------------------------------------------------------------- objects
int32_t Reflection::NumObjects() const { return m_.Rd<int32_t>(objObjects_ + 8); }

uintptr_t Reflection::ObjectAt(int32_t index) const {
    uintptr_t objects = m_.Rd<uintptr_t>(objObjects_);
    if (!objects || index < 0 || index >= NumObjects()) return 0;
    uintptr_t chunk = m_.Rd<uintptr_t>(objects + 8 * (uintptr_t)(index / kChunkItems));
    if (!chunk) return 0;
    uint8_t item[kItemStride];
    if (!m_.Read(chunk + (uintptr_t)(index % kChunkItems) * kItemStride, item, sizeof item)) return 0;
    uint32_t fl;
    uintptr_t o;
    memcpy(&fl, item + kItemFlags, 4);
    memcpy(&o, item + kItemObject, 8);
    return (fl & kInternalDead) ? 0 : o;
}

bool Reflection::ReadHeader(uintptr_t a, ObjHeader& o) const {
    uint8_t b[kObjHeader];
    if (!Plausible(a) || !m_.Read(a, b, sizeof b)) return false;
    o.addr = a;
    memcpy(&o.flags, b + kObjFlags, 4);
    memcpy(&o.index, b + kObjIndex, 4);
    memcpy(&o.cls, b + kObjClass, 8);
    memcpy(&o.nameIdx, b + kObjName, 4);
    memcpy(&o.nameNum, b + kObjName + 4, 4);
    memcpy(&o.outer, b + kObjOuter, 8);
    return true;
}

bool Reflection::Alive(uintptr_t obj) const {
    ObjHeader h;
    if (!ReadHeader(obj, h)) return false;
    if (h.flags & (kRfBeginDestroyed | kRfFinishDestroyed)) return false;
    return ObjectAt(h.index) == obj;
}

bool Reflection::IsA(uintptr_t cls, uintptr_t base) const {
    if (!base) return false;
    int d = 0;
    for (uintptr_t c = cls; c && d < 64; c = m_.Rd<uintptr_t>(c + kStructSuper), d++)
        if (c == base) return true;
    return false;
}

bool Reflection::InstanceOf(uintptr_t obj, uintptr_t base) const {
    return IsA(m_.Rd<uintptr_t>(obj + kObjClass), base);
}

std::string Reflection::ObjName(uintptr_t obj) const { return obj ? FName(obj + kObjName) : ""; }
std::string Reflection::ClassName(uintptr_t obj) const { return ObjName(m_.Rd<uintptr_t>(obj + kObjClass)); }

std::string Reflection::Path(uintptr_t obj) const {
    std::vector<std::string> parts;
    for (uintptr_t o = obj; o && parts.size() < 32; o = m_.Rd<uintptr_t>(o + kObjOuter)) parts.push_back(ObjName(o));
    std::string s;
    for (size_t i = parts.size(); i-- > 0;) {
        s += parts[i];
        if (i) s += '.';
    }
    return s;
}

// Every non-null object slot, header read in batches. f(header, itemFlags).
template <typename F>
bool Reflection::ForEachObject(F f) const {
    uintptr_t objects = m_.Rd<uintptr_t>(objObjects_);
    int32_t num = NumObjects();
    if (!objects || num <= 0 || num > 16 * 1024 * 1024) return false;
    std::vector<uint8_t> items((size_t)kChunkItems * kItemStride);
    std::vector<uintptr_t> ptrs;
    std::vector<uint32_t> flags;
    std::vector<uint8_t> headers;
    std::vector<Mem::Span> spans;
    for (int32_t base = 0; base < num; base += kChunkItems) {
        uintptr_t chunk = m_.Rd<uintptr_t>(objects + 8 * (uintptr_t)(base / kChunkItems));
        int32_t n = num - base < kChunkItems ? num - base : kChunkItems;
        if (!chunk || !m_.Read(chunk, items.data(), (size_t)n * kItemStride)) continue;
        ptrs.clear();
        flags.clear();
        for (int32_t i = 0; i < n; i++) {
            const uint8_t* it = items.data() + (size_t)i * kItemStride;
            uintptr_t o;
            uint32_t fl;
            memcpy(&o, it + kItemObject, 8);
            memcpy(&fl, it + kItemFlags, 4);
            if (!o || (fl & kInternalDead) || !Plausible(o)) continue;
            ptrs.push_back(o);
            flags.push_back(fl);
        }
        headers.assign(ptrs.size() * kObjHeader, 0);
        spans.resize(ptrs.size());
        for (size_t i = 0; i < ptrs.size(); i++) spans[i] = {ptrs[i], headers.data() + i * kObjHeader, kObjHeader, false};
        m_.ReadMany(spans.data(), spans.size());
        for (size_t i = 0; i < ptrs.size(); i++) {
            if (!spans[i].ok) continue;
            const uint8_t* b = headers.data() + i * kObjHeader;
            ObjHeader h;
            h.addr = ptrs[i];
            memcpy(&h.flags, b + kObjFlags, 4);
            memcpy(&h.index, b + kObjIndex, 4);
            memcpy(&h.cls, b + kObjClass, 8);
            memcpy(&h.nameIdx, b + kObjName, 4);
            memcpy(&h.nameNum, b + kObjName + 4, 4);
            memcpy(&h.outer, b + kObjOuter, 8);
            f(h, flags[i]);
        }
    }
    return true;
}

int64_t Reflection::Scan(const std::vector<std::string>& wanted) {
    // Resolve the wanted names to pool indices with one pass over the name pool.
    std::unordered_map<std::string, std::string> want;  // lower -> original
    for (auto& w : wanted) want[Lower(w)] = w;
    std::unordered_map<uint32_t, std::string> idxToName;
    uint32_t cur = m_.Rd<uint32_t>(nameBlocks_ - 8), cursor = m_.Rd<uint32_t>(nameBlocks_ - 4);
    if (cur > 8192) return -1;
    std::vector<uint8_t> buf(0x20000);
    for (uint32_t b = 0; b <= cur && idxToName.size() < want.size(); ++b) {
        uintptr_t base = m_.Rd<uintptr_t>(nameBlocks_ + 8 * (uintptr_t)b);
        uint32_t size = b < cur ? 0x20000 : cursor;
        if (!base || size > 0x20000 || !m_.Read(base, buf.data(), size)) continue;
        for (uint32_t off = 0; off + 2 <= size;) {
            uint16_t h;
            memcpy(&h, buf.data() + off, 2);
            unsigned len = h >> 6;
            if (!len) break;
            bool wide = h & 1;
            size_t bytes = len * (wide ? 2 : 1);
            if (off + 2 + bytes > size) break;
            if (!wide) {
                std::string s((const char*)buf.data() + off + 2, len);
                auto it = want.find(Lower(s));
                if (it != want.end()) idxToName[(b << 16) | (off / 2)] = it->second;
            }
            off += 2 + (uint32_t)bytes;
            off += off & 1;
        }
    }
    std::unordered_map<std::string, std::vector<uintptr_t>> found;
    int64_t scanned = 0;
    bool ok = ForEachObject([&](const ObjHeader& h, uint32_t) {
        scanned++;
        if (h.nameNum != 0) return;
        auto it = idxToName.find(h.nameIdx);
        if (it != idxToName.end()) found[Lower(it->second)].push_back(h.addr);
    });
    if (!ok) return -1;
    named_ = std::move(found);
    return scanned;
}

std::vector<uintptr_t> Reflection::Named(const std::string& name) const {
    auto it = named_.find(Lower(name));
    return it == named_.end() ? std::vector<uintptr_t>() : it->second;
}

uintptr_t Reflection::Type(const std::string& name) const {
    for (uintptr_t o : Named(name)) {
        std::string meta = ClassName(o);
        if ((meta == "Class" || meta == "BlueprintGeneratedClass" || meta == "ScriptStruct" ||
             meta == "UserDefinedStruct") &&
            Alive(o))
            return o;
    }
    return 0;
}

uintptr_t Reflection::ByPath(const std::string& path) const {
    size_t dot = path.rfind('.');
    std::string last = dot == std::string::npos ? path : path.substr(dot + 1);
    for (uintptr_t o : Named(last))
        if (conan::EqualsIgnoreCase(Path(o), path) && Alive(o)) return o;
    return 0;
}

std::vector<uintptr_t> Reflection::Instances(uintptr_t cls, size_t limit) const {
    std::vector<uintptr_t> out;
    if (!cls) return out;
    std::unordered_map<uintptr_t, bool> cache;
    ForEachObject([&](const ObjHeader& h, uint32_t) {
        if (out.size() >= limit || !h.cls) return;
        if (h.flags & (kRfClassDefault | kRfArchetype | kRfBeginDestroyed | kRfFinishDestroyed)) return;
        auto it = cache.find(h.cls);
        if (it == cache.end()) it = cache.emplace(h.cls, IsA(h.cls, cls)).first;
        if (it->second) out.push_back(h.addr);
    });
    return out;
}

// ---------------------------------------------------------------- members
PropInfo Reflection::Prop(uintptr_t st, const std::string& name) const {
    PropInfo out;
    int d = 0;
    for (uintptr_t c = st; c && d < 64; c = m_.Rd<uintptr_t>(c + kStructSuper), d++) {
        int n = 0;
        for (uintptr_t p = m_.Rd<uintptr_t>(c + kStructChildProps); p && n < 8192;
             p = m_.Rd<uintptr_t>(p + kFieldNext), n++) {
            if (!conan::EqualsIgnoreCase(FName(p + kFieldName), name)) continue;
            uintptr_t fc = m_.Rd<uintptr_t>(p + kFieldClass);
            out.type = fc ? FName(fc + kFieldClassName) : "";
            out.size = m_.Rd<int32_t>(p + kPropElementSize);
            out.offset = m_.Rd<int32_t>(p + kPropOffset);
            return out;
        }
    }
    return out;
}

int32_t Reflection::Offset(uintptr_t st, const std::string& name, const char* type, int32_t size) const {
    PropInfo p = Prop(st, name);
    if (!p.ok() || p.type != type || (size > 0 && p.size != size)) return -1;
    return p.offset;
}

FuncInfo Reflection::Function(uintptr_t cls, const std::string& name) const {
    FuncInfo out;
    int d = 0;
    for (uintptr_t c = cls; c && d < 64; c = m_.Rd<uintptr_t>(c + kStructSuper), d++) {
        int n = 0;
        for (uintptr_t f = m_.Rd<uintptr_t>(c + kStructChildren); f && n < 16384;
             f = m_.Rd<uintptr_t>(f + kUFieldNext), n++) {
            if (!FNameIs(f + kObjName, name.c_str()) || ClassName(f) != "Function") continue;
            out.fn = f;
            out.parmsSize = m_.Rd<uint16_t>(f + kFuncParmsSize);
            return out;
        }
    }
    return out;
}

PropInfo Reflection::Param(uintptr_t fn, const std::string& name) const {
    PropInfo out;
    int n = 0;
    for (uintptr_t p = m_.Rd<uintptr_t>(fn + kStructChildProps); p && n < 256; p = m_.Rd<uintptr_t>(p + kFieldNext), n++) {
        if (!conan::EqualsIgnoreCase(FName(p + kFieldName), name)) continue;
        uintptr_t fc = m_.Rd<uintptr_t>(p + kFieldClass);
        out.type = fc ? FName(fc + kFieldClassName) : "";
        out.size = m_.Rd<int32_t>(p + kPropElementSize);
        out.offset = m_.Rd<int32_t>(p + kPropOffset);
        return out;
    }
    return out;
}

}  // namespace UE
