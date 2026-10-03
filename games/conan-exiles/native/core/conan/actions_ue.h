// Reflection toolkit for the mutation actions (lane L2b): FName decoding, UFunction and property
// lookup by name on a class chain, a typed parameter frame for ProcessEvent calls, a one-time
// sliced object search (the static-function CDOs and the item-name tables), and the world
// helpers every mutation needs (a ready world, a player's controller by Steam64).
//
// Every layout constant is the one proven on build 25639945 (El-Limon
// context/games/conan-exiles/evidence/reflection-25639945/README.md); every function and
// property is looked up by name and type-checked at runtime, so a layout change fails one action
// with a clear error instead of writing into the wrong field.
//
// Threads: functions marked "game thread" must run inside a GameThread::Run job. Name decoding
// reads the append-only FNamePool and is safe from any thread.
#pragma once

#include "ue/ue.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace conan {
namespace rx {

// ---- layout (build 25639945) ----
constexpr size_t kObjFlags = 0x08, kObjIndex = 0x0C, kObjClass = 0x10, kObjName = 0x18, kObjOuter = 0x20;
constexpr size_t kFieldNext = 0x28;  // UField::Next (the UFunction list of a class)
constexpr size_t kStructSuper = 0x40, kStructChildren = 0x48, kStructChildProps = 0x50, kStructSize = 0x58;
constexpr size_t kFuncFlags = 0xB0, kFuncParmsSize = 0xB6, kFuncRetOffset = 0xB8;
constexpr size_t kFFieldClass = 0x08, kFFieldNext = 0x18, kFFieldName = 0x20;
constexpr size_t kFFieldClassName = 0x08;
constexpr size_t kPropArrayDim = 0x2C, kPropElementSize = 0x30, kPropFlags = 0x38, kPropOffset = 0x44;
constexpr size_t kBoolFieldSize = 0x70, kBoolByteOffset = 0x71, kBoolByteMask = 0x72, kBoolFieldMask = 0x73;
constexpr size_t kItemStride = 0x18, kItemFlags = 0x04, kItemObject = 0x08;
constexpr int kChunkItems = 65536;
constexpr uint32_t kInternalDead = 0x10000000 | 0x20000000;
constexpr uint32_t kRfBeginDestroyed = 0x8000, kRfFinishDestroyed = 0x10000;
constexpr uint64_t kCpfReturnParm = 0x400;
// DataTable (UE 5.8 on this build): RowStruct +0x28, RowMap TMap<FName, uint8*> +0x30. The
// TSparseArray: Data +0x30, Num +0x38, AllocationFlags inline words +0x40 / secondary +0x50,
// FirstFreeIndex +0x60, NumFreeIndices +0x64. Element stride 24: FName +0, row pointer +8.
constexpr size_t kDtRowStruct = 0x28, kDtData = 0x30, kDtNum = 0x38, kDtFlagsInline = 0x40,
                 kDtFlagsSecondary = 0x50, kDtNumFree = 0x64, kDtStride = 24;

// ---- memory and names (any thread) ----
template <typename T>
T Rd(uintptr_t a) {
    T v;
    __builtin_memcpy(&v, (const void*)a, sizeof v);
    return v;
}
template <typename T>
void Wr(uintptr_t a, const T& v) {
    __builtin_memcpy((void*)a, &v, sizeof v);
}

// The FNamePool entry of a comparison index; "" when it is out of range or wide.
std::string NameText(uint32_t index);
// Case-insensitive FName comparison against an ANSI name (number must be 0).
bool NameIs(uintptr_t fnameAddr, const char* name);
// Comparison indices of ANSI names (case-insensitive), one pool scan for the whole list; -1 when
// a name is not in the pool yet. Any thread (the pool is append-only).
std::vector<int64_t> FindNameIndices(const std::vector<std::string>& names);

// ---- objects (game thread) ----
bool Alive(uintptr_t obj);
uintptr_t ClassOf(uintptr_t obj);
bool IsA(uintptr_t obj, const char* className);  // walks the class chain by name

// ---- reflection ----
struct Param {
    int32_t offset = -1;
    int32_t size = 0;
    std::string type;  // FFieldClass name: IntProperty, StrProperty, ...
    uint64_t flags = 0;
    uint8_t boolByteOffset = 0, boolByteMask = 0, boolFieldMask = 0;
};

struct Func {
    uintptr_t fn = 0;
    std::string name;
    uint16_t parmsSize = 0;
    std::vector<std::pair<std::string, Param>> params;
    const Param* P(const char* name) const;
};

// The first UFunction called `name` on `cls` or a superclass (most derived first, so a Blueprint
// override wins). 0 when there is none.
uintptr_t FindFunction(uintptr_t cls, const char* name);
bool LoadFunc(uintptr_t fn, Func& out);
// Looks up `cls`.`name` and checks every expected parameter: {name, type, size}. `error` names
// the first mismatch.
struct Expect {
    const char* name;
    const char* type;
    int32_t size;
};
bool ResolveFunc(uintptr_t cls, const char* name, const std::vector<Expect>& expect, Func& out, std::string& error);
// A property declared on `cls` or a superclass, checked against type and size.
bool FindProperty(uintptr_t cls, const char* name, const char* type, int32_t size, Param& out);

// A zeroed, 16-byte aligned parameter buffer for one ProcessEvent call. Setters check the
// parameter's reflected type and record the first error; Call() refuses a frame with an error.
class Frame {
public:
    explicit Frame(const Func& f);
    Frame& Int(const char* name, int32_t v);
    Frame& Float(const char* name, float v);
    Frame& Bool(const char* name, bool v);
    Frame& Obj(const char* name, uintptr_t v);
    Frame& NameNone(const char* name);
    // The FString points at `storage` (UTF-16, NUL-terminated by this call): keep it alive
    // until the call returns. The engine copies it; it never frees or resizes our buffer.
    Frame& Str(const char* name, std::u16string& storage);
    Frame& Vec(const char* name, double x, double y, double z);  // FVector / FRotator (3 doubles)
    Frame& Raw(const char* name, const void* bytes, size_t n);
    int32_t GetInt(const char* name) const;
    int64_t GetInt64(const char* name) const;
    bool GetBool(const char* name) const;
    uintptr_t GetObj(const char* name) const;
    void GetVec(const char* name, double& x, double& y, double& z) const;
    const uint8_t* At(const char* name) const;  // nullptr when missing
    uint8_t* data() { return buf_; }
    const std::string& error() const { return error_; }

private:
    const Param* Need(const char* name, const char* type);
    const Func& f_;
    std::vector<uint8_t> storage_;
    uint8_t* buf_;
    std::string error_;
};

// Game thread: ProcessEvent(obj, f, frame). False (and nothing called) when obj is not alive
// or the frame recorded an error.
bool Call(uintptr_t obj, const Func& f, Frame& frame, std::string* error = nullptr);

// ---- world (any thread; they queue game-thread jobs) ----
// Makes sure the stage 1 discovery found a live world (same steps as the chat path).
bool EnsureWorld(std::string& error);

// The static-function CDOs and the item-name tables, found once by a sliced object walk
// (16384 objects per game-thread job) and cached.
struct Lookups {
    uintptr_t systemLibrary = 0;  // Default__KismetSystemLibrary
    uintptr_t textLibrary = 0;    // Default__KismetTextLibrary
    std::vector<uintptr_t> nameTables;  // DataTables named *ItemNameToTemplateID*
    double walkMs = 0;           // game-thread time of the walk
};
bool EnsureLookups(Lookups& out, std::string& error);

// Lower-cased item code -> template id, from every ItemNameToTemplateID table (built once).
bool ItemNameMap(const std::unordered_map<std::string, int32_t>*& out, std::string& error);

// Game thread, after EnsureWorld: the controller of an online player by Steam64 (or the
// player name as a fallback), 0 when not online.
uintptr_t ControllerFor(const std::string& steam64OrName);

}  // namespace rx
}  // namespace conan
