#include "ue/reflect.h"

#include "conan/text.h"
#include "ue/ue.h"

#include <cstring>

namespace UER {
namespace {

// ---- core layout (build 25639945; see ue.cpp and the probe's reflection dump) ----
constexpr size_t kItemStride = 0x18, kItemFlags = 0x04, kItemObject = 0x08;
constexpr int kChunkItems = 65536;
constexpr size_t kObjFlags = 0x08, kObjIndex = 0x0C, kObjClass = 0x10, kObjName = 0x18, kObjOuter = 0x20;
constexpr size_t kStructSuper = 0x40, kStructChildren = 0x48, kStructChildProps = 0x50;
constexpr size_t kUFieldNext = 0x28;  // UField::Next (functions in UStruct::Children)
constexpr size_t kFieldNext = 0x18, kFieldName = 0x20;  // FField
constexpr size_t kPropOffset = 0x44;
constexpr size_t kFuncParmsSize = 0xB6;
constexpr uint32_t kInternalDead = 0x10000000 | 0x20000000;  // unreachable, garbage
constexpr uint32_t kRfDestroying = 0x8000 | 0x10000;         // BeginDestroyed, FinishDestroyed
constexpr uint32_t kRfGarbage = 0x40000000;                  // RF_MirroredGarbage (PendingKill)
constexpr int kMaxDepth = 64;

template <typename T>
T rd(uintptr_t a) {
    T v;
    memcpy(&v, (const void*)a, sizeof v);
    return v;
}

bool Plausible(uintptr_t p) { return p >= 0x10000 && p < 0x800000000000ULL && (p & 7) == 0; }

char Low(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

uintptr_t ItemAt(int32_t index) {
    uintptr_t arr = UE::ObjObjectsAddr();
    if (!arr) return 0;
    uintptr_t objects = rd<uintptr_t>(arr);
    int32_t num = rd<int32_t>(arr + 8);
    if (!objects || index < 0 || index >= num) return 0;
    uintptr_t chunk = rd<uintptr_t>(objects + 8 * (uintptr_t)(index / kChunkItems));
    return chunk ? chunk + (uintptr_t)(index % kChunkItems) * kItemStride : 0;
}
}  // namespace

// ------------------------------------------------------------------------------------------ names

size_t LookupNames(const std::vector<std::string>& names, std::vector<uint32_t>& idx) {
    idx.assign(names.size(), kNoName);
    const uintptr_t blocks = UE::NameBlocksAddr();
    if (!blocks || names.empty()) return 0;
    size_t missing = names.size();
    uint32_t cur = rd<uint32_t>(blocks - 8), cursor = rd<uint32_t>(blocks - 4);
    if (cur > 8192) return 0;
    for (uint32_t b = 0; b <= cur && missing; ++b) {
        uintptr_t base = rd<uintptr_t>(blocks + 8 * b);
        if (!base) continue;
        uint32_t size = b < cur ? 0x20000 : cursor;
        for (uint32_t off = 0; off + 2 <= size;) {
            uint16_t h = rd<uint16_t>(base + off);
            unsigned len = h >> 6;
            if (!len) break;
            bool wide = h & 1;
            if (!wide) {
                const char* s = (const char*)(base + off + 2);
                for (size_t i = 0; i < names.size(); i++) {
                    if (idx[i] != kNoName || names[i].size() != len) continue;
                    bool eq = true;
                    for (unsigned k = 0; k < len && eq; k++) eq = Low(s[k]) == Low(names[i][k]);
                    if (eq) {
                        idx[i] = (b << 16) | (off / 2);
                        missing--;
                    }
                }
            }
            off += 2 + len * (wide ? 2 : 1);
            off += off & 1;
        }
    }
    return names.size() - missing;
}

std::string NameString(uint32_t i) {
    const uintptr_t blocks = UE::NameBlocksAddr();
    if (!blocks || i == kNoName) return "";
    uint32_t cur = rd<uint32_t>(blocks - 8);
    if ((i >> 16) > cur) return "";
    uintptr_t base = rd<uintptr_t>(blocks + 8 * (uintptr_t)(i >> 16));
    if (!base) return "";
    uintptr_t e = base + (uintptr_t)(i & 0xffff) * 2;
    uint16_t h = rd<uint16_t>(e);
    unsigned len = h >> 6;
    if (!len || len > 1024) return "";
    if (h & 1) {
        std::u16string w(len, u'\0');
        memcpy(&w[0], (const void*)(e + 2), (size_t)len * 2);
        return conan::Utf16To8(w);
    }
    return std::string((const char*)(e + 2), len);
}

// ---------------------------------------------------------------------------------------- objects

uintptr_t ClassOf(uintptr_t obj) { return obj ? rd<uintptr_t>(obj + kObjClass) : 0; }
uintptr_t OuterOf(uintptr_t obj) { return obj ? rd<uintptr_t>(obj + kObjOuter) : 0; }
uintptr_t SuperOf(uintptr_t st) { return st ? rd<uintptr_t>(st + kStructSuper) : 0; }
uint32_t NameIndexOf(uintptr_t obj) { return obj ? rd<uint32_t>(obj + kObjName) : kNoName; }
bool NameNumberIsZero(uintptr_t obj) { return obj && rd<uint32_t>(obj + kObjName + 4) == 0; }
std::string ObjectName(uintptr_t obj) { return obj ? NameString(NameIndexOf(obj)) : ""; }

bool Alive(uintptr_t obj) {
    if (!Plausible(obj)) return false;
    uintptr_t item = ItemAt(rd<int32_t>(obj + kObjIndex));
    if (!item || rd<uintptr_t>(item + kItemObject) != obj) return false;
    if (rd<uint32_t>(item + kItemFlags) & kInternalDead) return false;
    return !(rd<uint32_t>(obj + kObjFlags) & kRfDestroying);
}

bool Live(uintptr_t obj) { return Alive(obj) && !(rd<uint32_t>(obj + kObjFlags) & kRfGarbage); }

bool ClassIsA(uintptr_t cls, uint32_t nameIdx) {
    if (nameIdx == kNoName) return false;
    int depth = 0;
    for (uintptr_t c = cls; c && depth < kMaxDepth; c = rd<uintptr_t>(c + kStructSuper), depth++)
        if (rd<uint32_t>(c + kObjName) == nameIdx) return true;
    return false;
}

int32_t ObjectCount() {
    uintptr_t arr = UE::ObjObjectsAddr();
    return arr ? rd<int32_t>(arr + 8) : 0;
}

uintptr_t ObjectAt(int32_t i) {
    uintptr_t item = ItemAt(i);
    if (!item || (rd<uint32_t>(item + kItemFlags) & kInternalDead)) return 0;
    return rd<uintptr_t>(item + kItemObject);
}

// ------------------------------------------------------------------------------------- properties

int32_t PropertyOffset(uintptr_t st, uint32_t nameIdx) {
    if (nameIdx == kNoName) return -1;
    int depth = 0;
    for (uintptr_t c = st; c && depth < kMaxDepth; c = rd<uintptr_t>(c + kStructSuper), depth++) {
        int n = 0;
        for (uintptr_t p = rd<uintptr_t>(c + kStructChildProps); p && n < 4096; p = rd<uintptr_t>(p + kFieldNext), n++)
            if (rd<uint32_t>(p + kFieldName) == nameIdx && rd<uint32_t>(p + kFieldName + 4) == 0)
                return rd<int32_t>(p + kPropOffset);
    }
    return -1;
}

uintptr_t FindFunction(uintptr_t cls, uint32_t nameIdx) {
    if (nameIdx == kNoName) return 0;
    int depth = 0;
    for (uintptr_t c = cls; c && depth < kMaxDepth; c = rd<uintptr_t>(c + kStructSuper), depth++) {
        int n = 0;
        for (uintptr_t f = rd<uintptr_t>(c + kStructChildren); f && n < 8192; f = rd<uintptr_t>(f + kUFieldNext), n++)
            if (rd<uint32_t>(f + kObjName) == nameIdx) return f;
    }
    return 0;
}

uint16_t FunctionParmsSize(uintptr_t func) { return func ? rd<uint16_t>(func + kFuncParmsSize) : 0; }

std::string ReadFString(uintptr_t at, int maxChars) {
    if (!at) return "";
    uintptr_t data = rd<uintptr_t>(at);
    int32_t num = rd<int32_t>(at + 8);
    if (!data || num <= 1 || num > maxChars || !Plausible(data & ~(uintptr_t)7)) return "";
    std::u16string s((size_t)num - 1, u'\0');
    memcpy(&s[0], (const void*)data, ((size_t)num - 1) * 2);
    return conan::Utf16To8(s);
}

std::string TextToString(const TextConv& tc, uintptr_t ftext) {
    if (!tc.Ok() || !ftext) return "";
    uintptr_t data = rd<uintptr_t>(ftext);  // ITextData* (TSharedRef object)
    if (!Plausible(data)) return "";
    alignas(16) uint8_t parms[256];
    memset(parms, 0, sizeof parms);
    // A bitwise copy of the FText: the reference count is not touched, and the copy is never
    // destroyed, so the net effect on the text is zero.
    memcpy(parms + tc.inOff, (const void*)ftext, 16);
    UE::CallProcessEvent((void*)tc.cdo, (void*)tc.func, parms);
    return ReadFString((uintptr_t)(parms + tc.retOff));
}

}  // namespace UER
