// Generic UE reflection helpers on top of the pinned globals (UE::ObjObjectsAddr / NameBlocksAddr):
// names by string, object and class walks, property and parameter offsets, FString reads and the
// FText -> string conversion through KismetTextLibrary.Conv_TextToString.
//
// Thread rules:
//   - NameString / LookupNames read the FNamePool, which the engine only appends to: any thread.
//   - Everything that dereferences a UObject is game thread only (objects can be freed at any
//     other time). Hook handlers already run there.
//   - Never dereference a pointer field (FText, sub-objects) of an object that is not Live(): S4
//     crashed the server reading m_KillerName of a PendingKill pawn.
//
// Layout constants are the ones proven on build 25639945 (HANDOFF-native-stage2.md and the probe's
// reflection dump); everything above them is resolved by name at runtime.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace UER {

constexpr uint32_t kNoName = 0xffffffffu;

// ---- names (any thread) ----
// Looks up many ANSI names in one pass over the pool (case-insensitive, as FNames compare).
// idx[i] = comparison index, or kNoName when the name does not exist (yet). Returns the number found.
size_t LookupNames(const std::vector<std::string>& names, std::vector<uint32_t>& idx);
// The string of a comparison index ("" when out of range). Number suffixes are not included.
std::string NameString(uint32_t idx);

// ---- objects (game thread) ----
uintptr_t ClassOf(uintptr_t obj);
uintptr_t OuterOf(uintptr_t obj);
uintptr_t SuperOf(uintptr_t structObj);
uint32_t NameIndexOf(uintptr_t objOrField);  // FName comparison index at +0x18 (UObject)
bool NameNumberIsZero(uintptr_t obj);
std::string ObjectName(uintptr_t obj);
// Registered at its own index and not unreachable/garbage or being destroyed.
bool Alive(uintptr_t obj);
// Alive and not flagged as garbage (RF_MirroredGarbage / PendingKill 0x40000000): only then may
// pointer fields (FText, components) be followed.
bool Live(uintptr_t obj);
// The class chain of `cls` contains a class whose name index is `nameIdx`.
bool ClassIsA(uintptr_t cls, uint32_t nameIdx);
inline bool IsA(uintptr_t obj, uint32_t classNameIdx) { return obj && ClassIsA(ClassOf(obj), classNameIdx); }
// Object array: number of slots, and the object in slot i (0 when empty or dead).
int32_t ObjectCount();
uintptr_t ObjectAt(int32_t i);

// ---- properties (game thread) ----
// Offset of the property named `nameIdx` on a struct/class/function (supers included), or -1.
// For a UFunction the properties are its parameters, so this gives parameter offsets.
int32_t PropertyOffset(uintptr_t structObj, uint32_t nameIdx);
// A UFunction declared on `cls` or a superclass (UStruct::Children chain), or 0.
uintptr_t FindFunction(uintptr_t cls, uint32_t nameIdx);
// ParmsSize of a UFunction.
uint16_t FunctionParmsSize(uintptr_t func);
// An FString (TArray<TCHAR>) at `at`, UTF-8. Empty when null, too long or malformed.
std::string ReadFString(uintptr_t at, int maxChars = 4096);

// ---- FText -> string (game thread) ----
// Conv_TextToString on the KismetTextLibrary CDO. `textFunc`/`cdo` come from ResolveTextConv().
// The engine allocates the returned FString and this library cannot free it (no FMemory pin):
// callers cache the result per stable key so the leak stays bounded (a few bytes per distinct name).
struct TextConv {
    uintptr_t cdo = 0, func = 0;
    int32_t inOff = -1, retOff = -1;
    uint16_t parmsSize = 0;
    bool Ok() const { return cdo && func && inOff >= 0 && retOff >= 0 && parmsSize <= 256; }
};
std::string TextToString(const TextConv& tc, uintptr_t ftextAddr);

}  // namespace UER
