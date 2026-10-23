// Link stubs for the #298 research probe: the engine hook and symbol resolver are not needed to
// exercise GameThread's queue, so these satisfy the linker and are never called by the probe.
#include "hooks.h"
#include "resolve.h"

namespace Hooks {
std::atomic<uint64_t>* FiredCounter(const std::string&) { static std::atomic<uint64_t> c{0}; return &c; }
bool SwapVTableSlot(const std::string&, void*, size_t, void*, void**, std::string& err) { err = "stub"; return false; }
bool HookObjectVTable(const std::string&, void*, size_t, void*, void**, std::string& err) { err = "stub"; return false; }
}  // namespace Hooks
const std::vector<std::pair<std::string, std::pair<uint64_t, uint64_t>>>& VTableSymbols() {
    static std::vector<std::pair<std::string, std::pair<uint64_t, uint64_t>>> v;
    return v;
}
bool MemReadable(const void*, size_t) { return false; }
namespace Resolve { uint64_t Addr(const char*) { return 0; } }
