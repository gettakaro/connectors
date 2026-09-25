// UE 5.6 object model, driven entirely by symbols resolved in sym.cpp.
//
// Layout facts (UObject/UStruct/UClass/UFunction/FProperty offsets) are declared in one place and
// *validated at boot* against the live process; when a validation fails the offset is re-discovered
// by scanning, and when that fails the affected capability degrades. Nothing here ever aborts.
//
// Property offsets of game structs are never constants: they come from UStruct::FindPropertyByName.
#pragma once
#include "common.h"
#include <functional>

namespace UE {

struct FName {
    uint32_t Comparison = 0;
    uint32_t Number = 0;
    bool operator==(const FName& o) const { return Comparison == o.Comparison && Number == o.Number; }
    bool operator!=(const FName& o) const { return !(*this == o); }
};
static_assert(sizeof(FName) == 8, "FName must be 8 bytes (no FNAME_OUTLINE_NUMBER)");

struct FString {
    char16_t* Data = nullptr;
    int32_t Num = 0;
    int32_t Max = 0;
};

template <typename T>
struct TArray {
    T* Data = nullptr;
    int32_t Num = 0;
    int32_t Max = 0;
};

struct FTopLevelAssetPath {
    FName PackageName;
    FName AssetName;
};

using UObjectPtr = void*;

}  // namespace UE

namespace Reflect {

// Offsets, either the UE 5.6 default or the value discovered at boot.
struct Layout {
    uint32_t objFlags = 0x08;
    uint32_t objIndex = 0x0C;
    uint32_t objClass = 0x10;
    uint32_t objName = 0x18;
    uint32_t objOuter = 0x20;
    uint32_t structSuper = 0x40;
    uint32_t structChildProperties = 0x50;
    uint32_t structPropertiesSize = 0x58;
    uint32_t classDefaultObject = 0x110;
    uint32_t funcFunctionFlags = 0xB0;
    uint32_t funcFunc = 0xD8;
    uint32_t fieldNext = 0x20;
    uint32_t fieldName = 0x28;
    uint32_t fieldClass = 0x08;  // FFieldClass*, whose FName is at +0
    uint32_t propOffsetInternal = 0x44;
    // FProperty::ElementSize. Not validated at boot (nothing reads it yet), so it is the one
    // offset here that is a constant: 0x30, taken from the depot's own DWARF
    // (`tools/dwarfoffsets.py`). The UE4SS-Vein ini says 0x34, which is ArrayDim on this build -
    // trust the compiler's debug info over the third-party table.
    uint32_t propElementSize = 0x30;
};
const Layout& Lay();

// Resolves the function pointers. Cheap, no game calls. Safe on any thread.
void Init();
bool Ready();

// Runs the boot validations. MUST be called on the game thread. Idempotent; returns a JSON array of
// {check, ok, detail}. Sets the `reflect*` capabilities.
void Validate(); // game-thread reads only; diagnostics serialize the copied checks off-thread
bool Validated();

// ---- string helpers ----
std::string Utf16To8(const char16_t* s, int len);
std::vector<char16_t> Utf8To16(const std::string& s);  // NUL-terminated
std::string ToStd(UE::FString& s, bool freeIt);        // consumes (and optionally frees) an FString
std::string NameToString(const UE::FName& n);
UE::FName MakeName(const std::string& s, bool add = false);  // FNAME_Find by default

// ---- object helpers (game thread only unless noted) ----
void* ObjClass(void* obj);
void* ObjOuter(void* obj);
std::string ObjName(void* obj);
std::string ObjPathName(void* obj);
std::string ClassName(void* obj);          // name of obj's class
void* SuperStruct(void* structPtr);
void* ClassDefaultObject(void* cls);
bool IsA(void* obj, void* cls);            // walks the class chain
void* FindFunction(void* obj, const std::string& name);
void* FunctionNative(void* func);          // UFunction::Func
void* FindProperty(void* structPtr, const std::string& name);
int32_t PropertyOffset(void* prop);
std::string PropertyTypeName(void* prop);

// UClass* from a resolved `X::StaticClass` symbol; nullptr when unresolved.
void* StaticClass(const char* symName);
// StaticFindObject(nullptr, FTopLevelAssetPath{pkg, name}, false)
void* FindObjectByPath(const std::string& package, const std::string& name);

bool GetObjectsOfClass(void* cls, std::vector<void*>& out, bool includeDerived = true);
bool GetObjectsWithOuter(void* outer, std::vector<void*>& out, bool includeNested = true);

// ---- diagnostics ----
// Capture owned property values on the game thread; invoke the returned renderer
// on a background thread. Renderers never retain or dereference game pointers.
using DebugSnapshot = std::function<std::string()>;
DebugSnapshot DumpObject(void* obj, int maxProps = 512);
DebugSnapshot DumpStruct(const std::string& name);
std::string LayoutJson();

// Cached build strings; filled by Validate() on the game thread, "" until then.
std::string GameBuild();
std::string EngineVersion();

}  // namespace Reflect
