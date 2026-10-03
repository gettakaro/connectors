#include "anchor_scan.h"

#include "common.h"
#include "pe_image.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <vector>

namespace winplat {

namespace {

// Same core layout as Linux (same engine build, platform-neutral reflection layout).
constexpr size_t kItemStride = 0x18, kItemObject = 0x08;
constexpr size_t kObjIndex = 0x0C, kObjClass = 0x10;
// UFunction::FunctionFlags is at +0xB0 (NumParms +0xB4, ParmsSize +0xB6); ScoreProcessEvent tests it.
constexpr int kLinuxProcessEventSlot = 78;

struct Range {
    uintptr_t lo = 0, hi = 0;
    bool ok = false;
};
thread_local Range t_last;

bool ReadableUncached(uintptr_t a, size_t len) {
    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t end = a + len;
    while (a < end) {
        if (!VirtualQuery((LPCVOID)a, &mbi, sizeof mbi)) return false;
        const DWORD okProt = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                             PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        if (mbi.State != MEM_COMMIT || !(mbi.Protect & okProt) || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
            return false;
        uintptr_t regionEnd = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        t_last = {(uintptr_t)mbi.BaseAddress, regionEnd, true};
        a = regionEnd;
    }
    return true;
}

template <typename T>
bool Rd(uintptr_t a, T& v) {
    if (!Readable(a, sizeof v)) return false;
    memcpy(&v, (const void*)a, sizeof v);
    return true;
}

std::string Hex(uintptr_t v) {
    char b[32];
    snprintf(b, sizeof b, "0x%llx", (unsigned long long)v);
    return b;
}

std::string Rva(uintptr_t v) { return v ? "rva " + Hex(v - ImageBase()) : "none"; }

std::string Bytes(uintptr_t a, size_t n) {
    std::string s;
    for (size_t i = 0; i < n; i++) {
        uint8_t b;
        if (!Rd(a + i, b)) break;
        char t[4];
        snprintf(t, sizeof t, "%02X", b);
        s += (i ? " " : "") + std::string(t);
    }
    return s;
}

std::vector<Section> WritableSections() {
    std::vector<Section> out;
    for (auto& s : Sections())
        if (s.Writable() && !s.Executable() && s.size) out.push_back(s);
    return out;
}

// ---- GUObjectArray.ObjObjects ----
int ScoreObjArray(uintptr_t c, int32_t& numOut) {
    uintptr_t objects = 0;
    int32_t num = 0, max = 0, nchunks = 0, maxChunks = 0;
    if (!Rd(c, objects) || !Rd(c + 8, num) || !Rd(c + 0xC, max) || !Rd(c + 0x10, nchunks) || !Rd(c + 0x14, maxChunks))
        return 0;
    if (num < 1000 || num > max || max > (64 << 20) || nchunks != (num + 65535) / 65536 || nchunks > maxChunks ||
        maxChunks > 4096 || InImage(objects) || !Readable(objects, 8 * (size_t)nchunks))
        return 0;
    uintptr_t chunk0 = 0;
    if (!Rd(objects, chunk0) || !chunk0) return 0;
    int checked = 0, good = 0;
    for (int32_t i = 0; i < 2000 && i < num; i++) {
        uintptr_t obj = 0, vt = 0, cls = 0;
        int32_t idx = -1;
        if (!Rd(chunk0 + (uintptr_t)i * kItemStride + kItemObject, obj) || !obj) continue;
        checked++;
        if (Rd(obj, vt) && InImage(vt) && Rd(obj + kObjIndex, idx) && idx == i && Rd(obj + kObjClass, cls) && cls)
            good++;
    }
    numOut = num;
    return checked >= 100 && good == checked ? good : 0;
}

uintptr_t FindObjObjects(std::string& report, int32_t& num) {
    uintptr_t best = 0;
    int bestScore = 0, hits = 0;
    for (auto& s : WritableSections()) {
        for (uintptr_t c = s.address; c + 0x18 <= s.address + s.size; c += 8) {
            int32_t n = 0;
            int score = ScoreObjArray(c, n);
            if (!score) continue;
            hits++;
            report += "objObjects candidate " + Rva(c) + " num=" + std::to_string(n) + " score=" +
                      std::to_string(score) + " section=" + s.name + "\n";
            if (score > bestScore) {
                bestScore = score;
                best = c;
                num = n;
            }
        }
    }
    if (hits > 1) report += "objObjects: " + std::to_string(hits) + " candidates, taking the best\n";
    return best;
}

// ---- FNamePool blocks ----
bool EntryIs(uintptr_t entry, const char* want) {
    uint16_t h = 0;
    if (!Rd(entry, h) || (h & 1)) return false;  // wide
    size_t len = h >> 6;
    if (len != strlen(want) || !Readable(entry + 2, len)) return false;
    return _strnicmp((const char*)(entry + 2), want, len) == 0;
}

uintptr_t FindNameBlocks(std::string& report) {
    uintptr_t best = 0;
    int hits = 0;
    for (auto& s : WritableSections()) {
        for (uintptr_t b = s.address + 8; b + 8 <= s.address + s.size; b += 8) {
            uint32_t cur = 0, cursor = 0;
            uintptr_t block0 = 0;
            if (!Rd(b - 8, cur) || !Rd(b - 4, cursor) || cur > 8192 || cursor > 0x20000 * 2 || !Rd(b, block0) ||
                !block0 || InImage(block0))
                continue;
            if (!EntryIs(block0, "None")) continue;
            // FName 1 follows "None": header (2) + 4 chars = 6 bytes, 2-aligned.
            if (!EntryIs(block0 + 6, "ByteProperty")) continue;
            // Every block up to CurrentBlock is allocated.
            bool all = true;
            for (uint32_t i = 0; i <= cur && all; i++) {
                uintptr_t bi = 0;
                all = Rd(b + 8 * i, bi) && bi && Readable(bi, 2);
            }
            if (!all) continue;
            hits++;
            report += "nameBlocks candidate " + Rva(b) + " currentBlock=" + std::to_string(cur) + " cursor=" +
                      Hex(cursor) + " section=" + s.name + "\n";
            if (!best) best = b;
        }
    }
    if (hits > 1) report += "nameBlocks: " + std::to_string(hits) + " candidates, taking the first\n";
    return best;
}

// ---- ProcessEvent ----
uintptr_t FollowThunk(uintptr_t f) {
    for (int i = 0; i < 4; i++) {
        uint8_t op = 0;
        int32_t rel = 0;
        if (!Rd(f, op) || op != 0xE9 || !Rd(f + 1, rel)) break;
        // An incremental-link thunk jumps within .text; a detour (our own MinHook patch when the
        // signature path already hooked) jumps out of the image, and the function is still here.
        uintptr_t to = f + 5 + (intptr_t)rel;
        if (!InSection(to, ".text")) break;
        f = to;
    }
    return f;
}

// How strongly the function's first bytes test FUNC_Native in UFunction::FunctionFlags.
int ScoreProcessEvent(uintptr_t f) {
    const size_t n = 0x400;
    if (!InSection(f, ".text") || !Readable(f, n)) return 0;
    const uint8_t* p = (const uint8_t*)f;
    int score = 0;
    for (size_t i = 0; i + 10 <= n; i++) {
        // test dword ptr [reg+0xB0], 0x400   F7 /0 modrm(10 000 rrr) B0 00 00 00 00 04 00 00
        if (p[i] == 0xF7 && (p[i + 1] & 0xF8) == 0x80 && memcmp(p + i + 2, "\xB0\x00\x00\x00\x00\x04\x00\x00", 8) == 0)
            score += 10;
        // test byte ptr [reg+0xB1], 4        F6 /0 modrm B1 00 00 00 04
        if (p[i] == 0xF6 && (p[i + 1] & 0xF8) == 0x80 && memcmp(p + i + 2, "\xB1\x00\x00\x00\x04", 5) == 0)
            score += 10;
        // any [reg+0xB0] dword access, a weak hint
        if ((p[i] & 0xC0) == 0x80 && memcmp(p + i + 1, "\xB0\x00\x00\x00", 4) == 0) score += 1;
    }
    return score;
}

uintptr_t FindProcessEvent(uintptr_t objObjects, int& slotOut, std::string& report) {
    uintptr_t objects = 0, chunk0 = 0, obj = 0, vt = 0;
    if (!Rd(objObjects, objects) || !Rd(objects, chunk0) || !Rd(chunk0 + kItemObject, obj) || !Rd(obj, vt)) {
        report += "processEvent: cannot read object 0's vtable\n";
        return 0;
    }
    report += "object 0 vtable " + Rva(vt) + "\n";
    uintptr_t best = 0;
    int bestScore = 0;
    for (int slot = kLinuxProcessEventSlot - 8; slot <= kLinuxProcessEventSlot + 8; slot++) {
        uintptr_t f = 0;
        if (!Rd(vt + 8 * (uintptr_t)slot, f)) continue;
        f = FollowThunk(f);
        int score = ScoreProcessEvent(f);
        if (score) report += "  slot " + std::to_string(slot) + " " + Rva(f) + " score=" + std::to_string(score) + "\n";
        if (score > bestScore) {
            bestScore = score;
            best = f;
            slotOut = slot;
        }
    }
    if (best)
        report += "processEvent = slot " + std::to_string(slotOut) + " " + Rva(best) + " first bytes: " +
                  Bytes(best, 48) + "\n";
    return bestScore >= 10 ? best : 0;
}

}  // namespace

bool Readable(uintptr_t a, size_t len) {
    if (!a || a + len < a) return false;
    if (t_last.ok && a >= t_last.lo && a + len <= t_last.hi) return true;
    return ReadableUncached(a, len);
}

std::string NameText(uintptr_t nameBlocks, uint32_t id) {
    uintptr_t block = 0;
    if (!nameBlocks || (id >> 16) > 8192 || !Rd(nameBlocks + 8 * (uintptr_t)(id >> 16), block) || !block) return "";
    uintptr_t entry = block + 2 * (uintptr_t)(id & 0xFFFF);
    uint16_t h = 0;
    if (!Rd(entry, h) || (h & 1)) return "";
    size_t len = h >> 6;
    if (!len || !Readable(entry + 2, len)) return "";
    return std::string((const char*)(entry + 2), len);
}

std::string ObjectName(uintptr_t nameBlocks, uintptr_t obj) {
    uint32_t id = 0, number = 0;
    if (!Rd(obj + 0x18, id) || !Rd(obj + 0x1C, number)) return "";
    std::string s = NameText(nameBlocks, id);
    if (number) s += "_" + std::to_string(number - 1);
    return s;
}

std::string ClassName(uintptr_t nameBlocks, uintptr_t obj) {
    uintptr_t cls = 0;
    return Rd(obj + kObjClass, cls) && cls ? ObjectName(nameBlocks, cls) : "";
}

std::string DumpClass(uintptr_t objObjects, uintptr_t nameBlocks, const char* className) {
    uintptr_t objects = 0;
    int32_t num = 0;
    if (!Rd(objObjects, objects) || !Rd(objObjects + 8, num)) return "cannot read the object array\n";
    uintptr_t cls = 0;
    for (int32_t i = 0; i < num && !cls; i++) {
        uintptr_t chunk = 0, obj = 0;
        if (!Rd(objects + 8 * (uintptr_t)(i / 65536), chunk) || !chunk) continue;
        if (!Rd(chunk + (uintptr_t)(i % 65536) * kItemStride + kItemObject, obj) || !obj) continue;
        if (ObjectName(nameBlocks, obj) == className &&
            (ClassName(nameBlocks, obj) == "Class" || ClassName(nameBlocks, obj) == "ScriptStruct"))
            cls = obj;
    }
    if (!cls) return std::string("class ") + className + " not found\n";
    std::string out = std::string("class ") + className + " at " + Hex(cls) + " header: " + Bytes(cls, 0x60) + "\n";
    int32_t structSize = 0;
    Rd(cls + 0x58, structSize);
    out += "  PropertiesSize(+0x58) " + std::to_string(structSize) + "\n";
    uintptr_t super = 0, props = 0;
    Rd(cls + 0x40, super);
    Rd(cls + 0x50, props);
    out += "  super(+0x40) " + (super ? ObjectName(nameBlocks, super) : std::string("null")) + ", childProperties(+0x50) " +
           Hex(props) + "\n";
    int n = 0;
    for (uintptr_t p = props; p && n < 40; n++) {
        uintptr_t fc = 0, next = 0;
        uint32_t nameId = 0, fcName = 0, fcName0 = 0;
        int32_t elem = 0, off = 0;
        Rd(p + 0x08, fc);
        Rd(p + 0x18, next);
        Rd(p + 0x20, nameId);
        Rd(p + 0x34, elem);
        Rd(p + 0x44, off);
        if (fc) {
            Rd(fc + 0x08, fcName);
            Rd(fc, fcName0);
        }
        out += "  field " + Hex(p) + " name(+0x20)='" + NameText(nameBlocks, nameId) + "' class(+8)->name(+8)='" +
               NameText(nameBlocks, fcName) + "' (+0)='" + NameText(nameBlocks, fcName0) + "' elem(+0x30)=" +
               std::to_string(elem) + " offset(+0x44)=" + std::to_string(off) + " raw: " + Bytes(p, 0x48) + "\n";
        p = next;
    }
    return out;
}

bool AutoDetectAnchors(AutoAnchors& out, unsigned timeoutMs) {
    out = AutoAnchors();
    const uint64_t t0 = NowMs();
    out.report = "image base " + Hex(ImageBase()) + ", identity " + PeIdentity() + "\n";
    for (;;) {
        std::string r;
        int32_t num = 0;
        uintptr_t oo = FindObjObjects(r, num);
        if (oo && num > 100000) {
            out.objObjects = oo;
            out.objectCount = num;
            out.report += r;
            break;
        }
        if (NowMs() - t0 > timeoutMs) {
            out.report += r + "objObjects: not found within the time limit\n";
            break;
        }
        Sleep(1000);
    }
    out.nameBlocks = FindNameBlocks(out.report);
    if (out.objObjects) out.processEvent = FindProcessEvent(out.objObjects, out.processEventSlot, out.report);
    out.report += "result: objObjects " + Rva(out.objObjects) + " (" + std::to_string(out.objectCount) +
                  " objects), nameBlocks " + Rva(out.nameBlocks) + ", processEvent " + Rva(out.processEvent) +
                  " (slot " + std::to_string(out.processEventSlot) + "), " + std::to_string(NowMs() - t0) + " ms\n";
    return out.objObjects && out.nameBlocks && out.processEvent;
}

}  // namespace winplat
