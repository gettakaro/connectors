#include "scan.h"
#include "common.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace scan {
namespace {
struct Range {
    uint32_t begin = 0, end = 0;
};
uintptr_t g_base = 0;
uint32_t g_timestamp = 0;
std::vector<Range> g_text, g_rdata;
const RUNTIME_FUNCTION* g_pdata = nullptr;
size_t g_pdataCount = 0;
std::vector<uint32_t> g_pdataBegins;  // sorted copy index

const uint8_t* B(uint32_t rva) { return (const uint8_t*)(g_base + rva); }
}  // namespace

bool Init() {
    g_base = (uintptr_t)GetModuleHandleA(nullptr);
    auto* dos = (IMAGE_DOS_HEADER*)g_base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto* nt = (IMAGE_NT_HEADERS64*)(g_base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) return false;
    g_timestamp = nt->FileHeader.TimeDateStamp;
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
        Range r{sec->VirtualAddress, sec->VirtualAddress + sec->Misc.VirtualSize};
        if (sec->Characteristics & IMAGE_SCN_MEM_EXECUTE) g_text.push_back(r);
        else if ((sec->Characteristics & IMAGE_SCN_MEM_READ) && !(sec->Characteristics & IMAGE_SCN_MEM_WRITE))
            g_rdata.push_back(r);
    }
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    g_pdata = (const RUNTIME_FUNCTION*)(g_base + dir.VirtualAddress);
    g_pdataCount = dir.Size / sizeof(RUNTIME_FUNCTION);
    PluginLog("scan: base=%p ts=0x%08x text=%zu rdata=%zu pdata=%zu", (void*)g_base, g_timestamp, g_text.size(),
              g_rdata.size(), g_pdataCount);
    return !g_text.empty() && !g_rdata.empty() && g_pdataCount > 0;
}

uintptr_t Base() { return g_base; }
uint32_t TimeDateStamp() { return g_timestamp; }

bool InText(uint32_t rva) {
    for (auto& r : g_text)
        if (rva >= r.begin && rva < r.end) return true;
    return false;
}

uint32_t FindCString(const char* s) {
    size_t n = strlen(s) + 1;  // include terminating NUL
    for (auto& r : g_rdata) {
        const uint8_t* p = B(r.begin);
        size_t len = r.end - r.begin;
        if (len < n + 1) continue;
        const uint8_t* end = p + len - n;
        for (const uint8_t* q = p + 1; q <= end;) {
            const uint8_t* hit = (const uint8_t*)memchr(q, (unsigned char)s[0], end - q + 1);
            if (!hit) break;
            if (hit[-1] == 0 && memcmp(hit, s, n) == 0) return (uint32_t)(hit - (const uint8_t*)g_base);
            q = hit + 1;
        }
    }
    return 0;
}

std::vector<uint32_t> FindLeaXrefs(uint32_t target) {
    std::vector<uint32_t> out;
    for (auto& r : g_text) {
        const uint8_t* p = B(r.begin);
        uint32_t len = r.end - r.begin;
        for (uint32_t i = 0; i + 7 <= len; i++) {
            uint8_t rex = p[i];
            if ((rex != 0x48 && rex != 0x4C) || p[i + 1] != 0x8D || (p[i + 2] & 0xC7) != 0x05) continue;
            int32_t disp;
            memcpy(&disp, p + i + 3, 4);
            if ((int64_t)r.begin + i + 7 + disp == (int64_t)target) out.push_back(r.begin + i);
        }
    }
    return out;
}

std::vector<uint32_t> FindDirectCalls(uint32_t target) {
    std::vector<uint32_t> out;
    for (auto& r : g_text) {
        const uint8_t* p = B(r.begin);
        uint32_t len = r.end - r.begin;
        for (uint32_t i = 0; i + 5 <= len; i++) {
            if (p[i] != 0xE8) continue;
            int32_t disp;
            memcpy(&disp, p + i + 1, 4);
            if ((int64_t)r.begin + i + 5 + disp == (int64_t)target) out.push_back(r.begin + i);
        }
    }
    return out;
}

std::vector<uint32_t> CallTargetsIn(uint32_t start, uint32_t end) {
    std::vector<uint32_t> out;
    for (uint32_t a = start; a + 5 <= end; a++) {
        if (*B(a) != 0xE8) continue;
        int32_t disp;
        memcpy(&disp, B(a + 1), 4);
        uint32_t t = (uint32_t)((int64_t)a + 5 + disp);
        if (InText(t)) out.push_back(t);
    }
    return out;
}

uint32_t FunctionRoot(uint32_t rva, uint32_t* primaryEnd) {
    // .pdata is sorted by BeginAddress.
    size_t lo = 0, hi = g_pdataCount;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (g_pdata[mid].BeginAddress <= rva) lo = mid + 1;
        else hi = mid;
    }
    if (lo == 0) return 0;
    const RUNTIME_FUNCTION* rf = &g_pdata[lo - 1];
    if (!(rva >= rf->BeginAddress && rva < rf->EndAddress)) return 0;
    for (int guard = 0; guard < 32; guard++) {
        const uint8_t* ui = B(rf->UnwindData);
        uint8_t flags = ui[0] >> 3;
        uint8_t count = ui[2];
        if (!(flags & 0x4)) break;  // UNW_FLAG_CHAININFO
        size_t off = 4 + ((count + 1) & ~1) * 2;
        rf = (const RUNTIME_FUNCTION*)(ui + off);
    }
    if (primaryEnd) *primaryEnd = rf->EndAddress;
    return rf->BeginAddress;
}

bool MatchAt(uint32_t rva, const char* pattern) {
    const uint8_t* p = B(rva);
    size_t k = 0;
    for (const char* c = pattern; *c;) {
        while (*c == ' ') c++;
        if (!*c) break;
        if (c[0] == '?') {
            c += (c[1] == '?') ? 2 : 1;
            k++;
            continue;
        }
        unsigned v = 0;
        for (int j = 0; j < 2; j++) {
            char h = c[j];
            v <<= 4;
            if (h >= '0' && h <= '9') v |= h - '0';
            else if (h >= 'a' && h <= 'f') v |= h - 'a' + 10;
            else if (h >= 'A' && h <= 'F') v |= h - 'A' + 10;
            else return false;
        }
        c += 2;
        if (p[k++] != v) return false;
    }
    return true;
}

std::vector<uint32_t> FindPattern(const char* pattern, size_t maxHits) {
    std::vector<int> bytes;  // -1 = wildcard
    for (const char* c = pattern; *c;) {
        while (*c == ' ') c++;
        if (!*c) break;
        if (c[0] == '?') {
            c += (c[1] == '?') ? 2 : 1;
            bytes.push_back(-1);
            continue;
        }
        bytes.push_back((int)strtoul(std::string(c, 2).c_str(), nullptr, 16));
        c += 2;
    }
    std::vector<uint32_t> out;
    if (bytes.empty() || bytes[0] < 0) return out;
    for (auto& r : g_text) {
        const uint8_t* p = B(r.begin);
        size_t len = r.end - r.begin;
        if (len < bytes.size()) continue;
        const uint8_t* end = p + len - bytes.size();
        for (const uint8_t* q = p; q <= end;) {
            const uint8_t* hit = (const uint8_t*)memchr(q, bytes[0], end - q + 1);
            if (!hit) break;
            size_t k = 1;
            while (k < bytes.size() && (bytes[k] < 0 || hit[k] == (uint8_t)bytes[k])) k++;
            if (k == bytes.size()) {
                out.push_back((uint32_t)(hit - (const uint8_t*)g_base));
                if (out.size() >= maxHits) return out;
            }
            q = hit + 1;
        }
    }
    return out;
}

uint32_t CallDest(uint32_t rva) {
    if (!InText(rva) || *B(rva) != 0xE8) return 0;
    int32_t disp;
    memcpy(&disp, B(rva + 1), 4);
    return (uint32_t)((int64_t)rva + 5 + disp);
}

uint32_t FindImportSlot(const char* dll, const char* fn) {
    auto* dos = (IMAGE_DOS_HEADER*)g_base;
    auto* nt = (IMAGE_NT_HEADERS64*)(g_base + dos->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return 0;
    for (auto* d = (IMAGE_IMPORT_DESCRIPTOR*)(g_base + dir.VirtualAddress); d->Name; d++) {
        if (lstrcmpiA((const char*)(g_base + d->Name), dll) != 0) continue;
        uint32_t names = d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk;
        for (uint32_t k = 0;; k++) {
            auto* t = (IMAGE_THUNK_DATA64*)(g_base + names + k * 8);
            if (!t->u1.AddressOfData) break;
            if (IMAGE_SNAP_BY_ORDINAL64(t->u1.Ordinal)) continue;
            auto* ibn = (IMAGE_IMPORT_BY_NAME*)(g_base + (uint32_t)t->u1.AddressOfData);
            if (strcmp((const char*)ibn->Name, fn) == 0) return d->FirstThunk + k * 8;
        }
    }
    return 0;
}

std::vector<uint32_t> FindIndirectCalls(uint32_t slot) {
    std::vector<uint32_t> out;
    for (auto& r : g_text) {
        const uint8_t* p = B(r.begin);
        uint32_t len = r.end - r.begin;
        for (uint32_t i = 0; i + 6 <= len; i++) {
            if (p[i] != 0xFF || p[i + 1] != 0x15) continue;
            int32_t disp;
            memcpy(&disp, p + i + 2, 4);
            if ((int64_t)r.begin + i + 6 + disp == (int64_t)slot) out.push_back(r.begin + i);
        }
    }
    return out;
}

}  // namespace scan
