// Reflection for worker threads: names, the object array, properties and UFunctions, read only
// through a Mem (safe self-reads). It is the off-game-thread counterpart of ue.h (which reads
// raw memory on the game thread for chat).
//
// Layout constants are the core UE layout proven on build 25639945 (the pins refuse every other
// build before anything here runs). Everything above that (class, property and function
// offsets) is looked up by name at runtime and type-checked.
#pragma once

#include "ue/mem.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace UE {

namespace Layout {
constexpr size_t kItemStride = 0x18, kItemFlags = 0x04, kItemObject = 0x08;
constexpr int kChunkItems = 65536;
constexpr size_t kObjFlags = 0x08, kObjIndex = 0x0C, kObjClass = 0x10, kObjName = 0x18, kObjOuter = 0x20;
constexpr size_t kObjHeader = 0x28;
constexpr size_t kStructSuper = 0x40, kStructChildren = 0x48, kStructChildProps = 0x50, kStructSize = 0x58;
constexpr size_t kUFieldNext = 0x28;
constexpr size_t kFieldClass = 0x08, kFieldNext = 0x18, kFieldName = 0x20, kFieldClassName = 0x08;
// ElementSize: +0x30 under clang (Linux server), +0x34 under MSVC (Windows server), which does not
// reuse FField's tail padding; see ue.cpp.
#ifdef _WIN32
constexpr size_t kPropElementSize = 0x34, kPropOffset = 0x44;
#else
constexpr size_t kPropElementSize = 0x30, kPropOffset = 0x44;
#endif
constexpr size_t kFuncParmsSize = 0xB6;
constexpr uint32_t kRfClassDefault = 0x10, kRfArchetype = 0x20, kRfBeginDestroyed = 0x8000,
                   kRfFinishDestroyed = 0x10000;
constexpr uint32_t kInternalDead = 0x10000000 | 0x20000000;  // unreachable | garbage
}  // namespace Layout

struct ObjHeader {
    uintptr_t addr = 0;
    uint32_t flags = 0;
    int32_t index = -1;
    uintptr_t cls = 0;
    uint32_t nameIdx = 0, nameNum = 0;
    uintptr_t outer = 0;
};

struct PropInfo {
    int32_t offset = -1;
    int32_t size = 0;
    std::string type;  // FFieldClass name, e.g. "ObjectProperty"
    bool ok() const { return offset >= 0; }
};

struct FuncInfo {
    uintptr_t fn = 0;
    uint16_t parmsSize = 0;
    bool ok() const { return fn != 0; }
};

class Reflection {
public:
    Reflection(const Mem& mem, uintptr_t objObjects, uintptr_t nameBlocks)
        : m_(mem), objObjects_(objObjects), nameBlocks_(nameBlocks) {}
    const Mem& mem() const { return m_; }

    // ---- names ----
    std::string Name(uint32_t comparisonIndex) const;  // "" when unreadable
    std::string FName(uintptr_t at) const;             // with the _N suffix
    bool FNameIs(uintptr_t at, const char* name) const;  // case-insensitive, number 0

    // ---- objects ----
    int32_t NumObjects() const;
    uintptr_t ObjectAt(int32_t index) const;
    bool ReadHeader(uintptr_t obj, ObjHeader& h) const;
    // Registered at its own index, not unreachable/garbage, not being destroyed.
    bool Alive(uintptr_t obj) const;
    bool IsA(uintptr_t cls, uintptr_t base) const;  // class chain
    bool InstanceOf(uintptr_t obj, uintptr_t base) const;
    std::string ObjName(uintptr_t obj) const;
    std::string ClassName(uintptr_t obj) const;
    std::string Path(uintptr_t obj) const;  // outermost first, '.'-joined

    // ---- index (one scan of the object array, worker thread) ----
    // Finds the objects named in `names` (exact, case-insensitive) and keeps them by name.
    // Returns the number of objects scanned, or -1 if the object array is unreadable.
    int64_t Scan(const std::vector<std::string>& names);
    std::vector<uintptr_t> Named(const std::string& name) const;  // after Scan
    // The UClass / UScriptStruct / UserDefinedStruct called `name` (from the scan).
    uintptr_t Type(const std::string& name) const;
    // The object whose full path is `path` (e.g. "/Game/Items/ItemTable.ItemTable") among Named(last part).
    uintptr_t ByPath(const std::string& path) const;
    // Live, non-template instances of `cls` (or subclasses): one more scan. Worker thread.
    std::vector<uintptr_t> Instances(uintptr_t cls, size_t limit = 64) const;

    // ---- members ----
    PropInfo Prop(uintptr_t st, const std::string& name) const;  // walks the super chain
    // Offset of a property that must have the given type and size, or -1.
    int32_t Offset(uintptr_t st, const std::string& name, const char* type, int32_t size) const;
    FuncInfo Function(uintptr_t cls, const std::string& name) const;
    PropInfo Param(uintptr_t fn, const std::string& name) const;

private:
    template <typename F>
    bool ForEachObject(F f) const;
    std::string Lower(const std::string& s) const;

    const Mem& m_;
    uintptr_t objObjects_, nameBlocks_;
    mutable std::mutex lock_;
    mutable std::unordered_map<uint32_t, std::string> names_;
    std::unordered_map<std::string, std::vector<uintptr_t>> named_;
};

}  // namespace UE
