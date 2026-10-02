#include "hooks.h"

#include "sym.h"

#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <cstring>

namespace {

struct Record {
    uint64_t addr = 0;
    std::string how;
    bool hooked = false;
    std::atomic<uint64_t> fired{0};
    Record() = default;
    Record(const Record& o) : addr(o.addr), how(o.how), hooked(o.hooked), fired(o.fired.load()) {}
};

struct Installed {
    std::string name;
    void** slotAddr;
    void* original;
};

Mutex g_lock;
std::map<std::string, Record> g_records;
std::vector<Installed> g_installed;

bool WriteSlot(void** slot, void* value, std::string& err) {
    long ps = sysconf(_SC_PAGESIZE);
    if (ps <= 0) ps = 4096;
    uintptr_t page = (uintptr_t)slot & ~(uintptr_t)(ps - 1);
    size_t len = (size_t)ps;
    if ((uintptr_t)slot + sizeof(void*) > page + len) len += (size_t)ps;  // straddles a page boundary
    bool needProt = !MemWritable(slot, sizeof(void*));
    if (needProt && mprotect((void*)page, len, PROT_READ | PROT_WRITE) != 0) {
        err = "mprotect failed";
        return false;
    }
    *slot = value;
    __sync_synchronize();
    if (needProt) {
        if (mprotect((void*)page, len, PROT_READ) != 0) PluginLog("hooks: could not restore page protection at %p", (void*)page);
        MemMapsRefresh();
    }
    return true;
}

}  // namespace

void Hooks::RecordResolve(const std::string& name, uint64_t addr, const std::string& how, bool hooked) {
    Guard g(g_lock);
    Record& r = g_records[name];
    r.addr = addr;
    r.how = how;
    r.hooked = hooked;
}

void Hooks::MarkHooked(const std::string& name, bool hooked) {
    Guard g(g_lock);
    g_records[name].hooked = hooked;
}

void Hooks::MarkFired(const std::string& name) {
    Guard g(g_lock);
    g_records[name].fired++;
}

std::atomic<uint64_t>* Hooks::FiredCounter(const std::string& name) {
    Guard g(g_lock);
    return &g_records[name].fired;
}

bool Hooks::SwapVTableSlot(const std::string& name, void* ztvSymbolAddr, size_t slot, void* detour, void** origOut,
                           std::string& err) {
    if (!ztvSymbolAddr) { err = "vtable symbol not found"; return false; }
    if (slot == SIZE_MAX) { err = "slot index unknown"; return false; }
    void** slotAddr = (void**)ztvSymbolAddr + 2 + slot;  // skip offset-to-top + typeinfo
    if (!MemReadable(slotAddr, sizeof(void*))) { err = "vtable slot is not readable"; return false; }
    void* orig = *slotAddr;
    const ElfInfo& e = Elf();
    if (e.textSize && ((uint64_t)(uintptr_t)orig < e.textAddr || (uint64_t)(uintptr_t)orig >= e.textAddr + e.textSize)) {
        err = "vtable slot does not point into .text";
        return false;
    }
    if (!WriteSlot(slotAddr, detour, err)) return false;
    if (origOut) *origOut = orig;
    {
        Guard g(g_lock);
        g_installed.push_back({name, slotAddr, orig});
    }
    RecordResolve(name, (uint64_t)(uintptr_t)orig, "vtable slot " + std::to_string(slot), true);
    PluginLog("hooks: installed %s at vtable slot %zu (orig=%p detour=%p)", name.c_str(), slot, orig, detour);
    return true;
}

bool Hooks::HookVTableSymbol(const std::string& name, const char* ztvName, size_t slot, void* detour, void** origOut,
                             std::string& err) {
    uint64_t vt = DynSymAddr(ztvName);
    if (!vt) { err = std::string("dynsym ") + ztvName + " not found"; return false; }
    return SwapVTableSlot(name, (void*)(uintptr_t)vt, slot, detour, origOut, err);
}

bool Hooks::HookProcessEvent(const std::string& name, const char* ztvName, void* detour, void** origOut,
                             std::string& err) {
    size_t slot = Sym::ProcessEventSlot();
    if (slot == SIZE_MAX) { err = "ProcessEvent slot unknown"; return false; }
    return HookVTableSymbol(name, ztvName, slot, detour, origOut, err);
}

bool Hooks::HookObjectVTable(const std::string& name, void* obj, size_t slot, void* detour, void** origOut,
                             std::string& err) {
    if (!obj || !MemReadable(obj, sizeof(void*))) { err = "object pointer not readable"; return false; }
    void* vt = *(void**)obj;
    if (!vt) { err = "object has no vtable"; return false; }
    // The object's vtable pointer points at slot 0, not at the _ZTV symbol; compensate.
    return SwapVTableSlot(name, (char*)vt - 2 * sizeof(void*), slot, detour, origOut, err);
}

void Hooks::RestoreAll() {
    std::vector<Installed> copy;
    {
        Guard g(g_lock);
        copy.swap(g_installed);
    }
    for (auto& i : copy) {
        std::string err;
        if (!WriteSlot(i.slotAddr, i.original, err)) PluginLog("hooks: restore of %s failed: %s", i.name.c_str(), err.c_str());
        MarkHooked(i.name, false);
    }
    if (!copy.empty()) PluginLog("hooks: restored %zu vtable slots", copy.size());
}

size_t Hooks::InstalledCount() {
    Guard g(g_lock);
    return g_installed.size();
}

std::string Hooks::ResolvedJson() {
    Guard g(g_lock);
    std::string o = "[";
    bool first = true;
    auto emit = [&](const std::string& name, uint64_t addr, uint64_t rva, const std::string& how, bool hooked,
                    uint64_t fired, const std::string& extra) {
        char b[128];
        if (!first) o += ",";
        first = false;
        o += "{\"name\":" + JsonStr(name);
        snprintf(b, sizeof b, "\"0x%llx\"", (unsigned long long)rva);
        o += ",\"rva\":" + std::string(rva ? b : "null");
        snprintf(b, sizeof b, "\"0x%llx\"", (unsigned long long)addr);
        o += ",\"addr\":" + std::string(addr ? b : "null");
        o += ",\"how\":" + JsonStr(how) + ",\"hooked\":" + (hooked ? "true" : "false") +
             ",\"fired\":" + std::to_string(fired) + extra + "}";
    };
    uint64_t base = Elf().loadBase;
    for (auto& e : Sym::All()) {
        auto it = g_records.find(e.name);
        bool hooked = it != g_records.end() && it->second.hooked;
        uint64_t fired = it != g_records.end() ? it->second.fired.load() : 0;
        std::string extra = ",\"signature\":" + JsonStr(e.signature) +
                            ",\"records\":" + std::to_string(e.records) +
                            ",\"contiguous\":" + (e.contiguous ? "true" : "false");
        emit(e.name, e.addr, e.rva, e.how.empty() ? "unresolved" : e.how, hooked, fired, extra);
    }
    for (auto& kv : g_records) {
        if (Sym::Entry(kv.first.c_str())) continue;  // already emitted above
        emit(kv.first, kv.second.addr, kv.second.addr > base ? kv.second.addr - base : 0, kv.second.how,
             kv.second.hooked, kv.second.fired.load(), "");
    }
    return o + "]";
}
