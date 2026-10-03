#include "ue/mem.h"

#include "conan/text.h"

namespace UE {

void Mem::ReadMany(Span* spans, size_t count) const {
    for (size_t i = 0; i < count; i++) spans[i].ok = Read(spans[i].addr, spans[i].out, spans[i].n);
}

bool Mem::ReadFString(uintptr_t at, std::string& out, int32_t maxChars) const {
    out.clear();
    struct {
        uintptr_t data;
        int32_t num, max;
    } h;
    if (!Read(at, &h, sizeof h)) return false;
    if (h.num == 0) return true;  // empty, never allocated
    if (h.num < 0 || h.num > maxChars + 1 || h.max < h.num || !h.data) return false;
    if (h.num == 1) return true;
    std::u16string s((size_t)h.num - 1, u'\0');
    if (!Read(h.data, &s[0], ((size_t)h.num - 1) * 2)) return false;
    out = conan::Utf16To8(s);
    return true;
}

}  // namespace UE
