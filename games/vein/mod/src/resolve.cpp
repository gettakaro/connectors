#include "resolve.h"

#include "state.h"

#include <cxxabi.h>
#include <dlfcn.h>
#include <atomic>
#include <elf.h>
#include <fcntl.h>
#include <link.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

// ---------------------------------------------------------------------------------------------
// The names we want.
//
// `sig` pins one overload as its full demangled signature; when it is null the lowest-address
// symbol whose demangled text before '(' equals `key` wins. `pattern`/`xref` feed the signature
// strategy and are only filled in for names that no symbol table on this build provides
// (docs/symbols-<buildid>.md records which). `required` names are the ones M0 insists on.
namespace {

struct Want {
    const char* key;
    const char* sig;      // exact demangled line, or nullptr for "base name, lowest address"
    bool required;
    bool data;            // data symbol (no .text plausibility check)
    const char* pattern;  // optional byte signature, "48 8B ?? E8 ?? ?? ?? ??"
    const char* xref;     // optional literal string the function must reference (narrows the scan)
    int adjust;           // added to a signature match address (e.g. to step back over a prologue)
};

#define FN(k, s, req) {k, s, req, false, nullptr, nullptr, 0}
#define DATA(k) {k, nullptr, false, true, nullptr, nullptr, 0}

const Want kWanted[] = {
    // --- anchors used by the boot cross-checks -------------------------------------------------
    FN("_init", "_init", false),
    FN("_fini", "_fini", false),
    FN("_start", "_start", false),

    // --- UE object model (the plugin cannot do anything at all without these) -------------------
    FN("UObject::ProcessEvent", "UObject::ProcessEvent(UFunction*, void*)", true),
    FN("UObject::FindFunction", "UObject::FindFunction(FName) const", true),
    FN("UStruct::FindPropertyByName", "UStruct::FindPropertyByName(FName) const", true),
    FN("StaticFindObject", "StaticFindObject(UClass*, UObject*, char16_t const*, bool)", false),
    FN("StaticFindObjectPath", "StaticFindObject(UClass*, FTopLevelAssetPath, bool)", true),
    FN("GetObjectsOfClass",
       "GetObjectsOfClass(UClass const*, TArray<UObject*, TSizedDefaultAllocator<32> >&, bool, EObjectFlags, "
       "EInternalObjectFlags)",
       true),
    FN("GetObjectsWithOuter",
       "GetObjectsWithOuter(UObjectBase const*, TArray<UObject*, TSizedDefaultAllocator<32> >&, bool, EObjectFlags, "
       "EInternalObjectFlags)",
       false),
    FN("UObjectBaseUtility::GetPathName", "UObjectBaseUtility::GetPathName(UObject const*) const", false),
    FN("FName::ToString", "FName::ToString() const", false),
    FN("FName::ToStringInto", "FName::ToString(FString&) const", true),
    FN("FName::FName", "FName::FName(char16_t const*, EFindName)", true),
    FN("FName::GetPlainNameString", nullptr, false),
    FN("FMemory::Malloc", "FMemory::Malloc(unsigned long, unsigned int)", false),
    FN("FMemory::Free", "FMemory::Free(void*)", true),

    // --- engine / world -------------------------------------------------------------------------
    // The live engine class is discovered at runtime; both the stock and a possible Vein subclass
    // are wanted so that whichever exists can be swept out of the exported vtables.
    FN("UGameEngine::Tick", "UGameEngine::Tick(float, bool)", false),
    FN("UEngine::Tick", "UEngine::Tick(float, bool)", false),
    FN("UVeinGameEngine::Tick", "UVeinGameEngine::Tick(float, bool)", false),
    DATA("GEngine"),
    FN("UGameplayStatics::GetGameMode", nullptr, false),
    FN("UGameplayStatics::GetGameState", nullptr, false),
    FN("UGameplayStatics::GetPlayerController", nullptr, false),
    FN("UGameplayStatics::GetGameInstance", nullptr, false),
    FN("AGameStateBase::GetServerWorldTimeSeconds", nullptr, false),
    FN("UEngine::Exec", "UEngine::Exec(UWorld*, char16_t const*, FOutputDevice&)", false),
    FN("UKismetSystemLibrary::ExecuteConsoleCommand", nullptr, false),
    FN("APlayerController::ConsoleCommand", nullptr, false),
    FN("APlayerController::EnableCheats", nullptr, false),
    FN("FOutputDeviceRedirector::Get", nullptr, false),
    FN("FOutputDeviceRedirector::AddOutputDevice", nullptr, false),
    FN("FOutputDeviceFile::Serialize",
       "FOutputDeviceFile::Serialize(char16_t const*, ELogVerbosity::Type, FName const&, double)", false),
    FN("RequestEngineExit", "RequestEngineExit(char16_t const*)", false),

    // --- version strings for /health -------------------------------------------------------------
    FN("FApp::GetBuildVersion", nullptr, false),
    FN("FEngineVersion::Current", nullptr, false),
    FN("FEngineVersion::ToString", nullptr, false),

    // --- text helpers ---------------------------------------------------------------------------
    FN("FText::FromString", "FText::FromString(FString const&)", false),
    FN("FTextInspector::GetDisplayString", "FTextInspector::GetDisplayString(FText const&)", false),

    // --- class objects used by the boot validations and by /health world detection ---------------
    // The `StaticClass()` thunks are per-class and let us name a UClass without a string lookup.
    // Everything Vein-specific is optional: /health falls back to StaticFindObject on the path.
    FN("UObject::StaticClass", nullptr, true),
    FN("UClass::StaticClass", nullptr, false),
    FN("UWorld::StaticClass", nullptr, false),
    FN("AActor::StaticClass", nullptr, true),
    FN("APlayerController::StaticClass", nullptr, false),
    FN("APlayerState::StaticClass", nullptr, false),
    FN("AGameModeBase::StaticClass", nullptr, false),
    FN("AGameStateBase::StaticClass", nullptr, false),
    FN("AGameSession::StaticClass", nullptr, false),
    FN("AVeinGameSession::StaticClass", nullptr, false),
    FN("AVeinBaseGameMode::StaticClass", nullptr, false),
    FN("AVeinGameStateBase::StaticClass", nullptr, false),
    FN("AVeinPlayerController::StaticClass", nullptr, false),
    FN("AVeinPlayerState::StaticClass", nullptr, false),
    FN("UVeinGameInstance::StaticClass", nullptr, false),
    FN("UVeinCheatManager::StaticClass", nullptr, false),

    // --- exec thunk used by the reflect boot validation ------------------------------------------
    // AActor::K2_TeleportTo is a stock engine UFUNCTION, so FindFunction(CDO,"K2_TeleportTo")->Func
    // must equal this address. That check is what proves UFunction::Func and the CDO offset.
    FN("AActor::execK2_TeleportTo", "AActor::execK2_TeleportTo(UObject*, FFrame&, void*)", false),
    FN("AActor::TeleportTo", nullptr, false),

    // --- player lifecycle / moderation (wired by lanes L2 and L3) ---------------------------------
    FN("AGameModeBase::PreLogin", nullptr, false),
    FN("AGameModeBase::PostLogin", nullptr, false),
    FN("AGameModeBase::Logout", nullptr, false),
    FN("AGameMode::Logout", "AGameMode::Logout(AController*)", false),
    FN("APlayerController::OnNetCleanup", "APlayerController::OnNetCleanup(UNetConnection*)", false),
    FN("AGameSession::KickPlayer", "AGameSession::KickPlayer(APlayerController*, FText const&)", false),
    FN("AGameSession::BanPlayer", "AGameSession::BanPlayer(APlayerController*, FText const&)", false),
    // LANE L3e: the kick fallback when AGameSession is not reachable. Same (this, FText const&)
    // shape, so it is called through the same FnKickBan pointer type minus the session argument.
    FN("APlayerController::ClientReturnToMainMenuWithTextReason",
       "APlayerController::ClientReturnToMainMenuWithTextReason(FText const&)", false),
    FN("AGameSession::NotifyLogout", nullptr, false),

    // --- lane L3: actions ------------------------------------------------------------------------
    // Every one of these is optional: a missing name degrades exactly one action capability.
    // Signatures are pinned wherever the base name has overloads (see docs/actions-design.md).

    // identity / player table
    FN("AVeinPlayerState::GetPlayerUniqueID", "AVeinPlayerState::GetPlayerUniqueID() const", false),

    // the admin-panel server RPC surface: give / kick / ban / teleport / exec / message / save
    FN("UAdminComponent::StaticClass", nullptr, false),
    FN("UAdminComponent::Server_GiveItem",
       "UAdminComponent::Server_GiveItem_Implementation(APlayerState*, TSubclassOf<UItem>, int)", false),
    FN("UAdminComponent::Server_KickPlayer", "UAdminComponent::Server_KickPlayer_Implementation(APlayerState*)", false),
    FN("UAdminComponent::Server_BanPlayer",
       "UAdminComponent::Server_BanPlayer_Implementation(APlayerState*, FString const&)", false),
    FN("UAdminComponent::Server_UnbanPlayer", "UAdminComponent::Server_UnbanPlayer_Implementation(FString const&)",
       false),
    FN("UAdminComponent::Server_MovePlayer",
       "UAdminComponent::Server_MovePlayer_Implementation(APlayerState*, UE::Math::TVector<double>)", false),
    FN("UAdminComponent::Server_TeleportTo", "UAdminComponent::Server_TeleportTo_Implementation(APlayerState*, FName)",
       false),
    FN("UAdminComponent::Server_Exec", "UAdminComponent::Server_Exec_Implementation(FString const&)", false),
    FN("UAdminComponent::Server_SendServerMessage",
       "UAdminComponent::Server_SendServerMessage_Implementation(FString const&)", false),
    FN("UAdminComponent::Server_RequestDedicatedServerSave",
       "UAdminComponent::Server_RequestDedicatedServerSave_Implementation()", false),
    FN("UAdminComponent::Server_Heal", "UAdminComponent::Server_Heal_Implementation(APlayerState*)", false),

    // --- LANE L3e: the UNGATED entry points the admin RPCs call *after* their IsAdmin() check ----
    // Disassembly of the three broken RPCs (2026-09-17, VeinServer-Linux-Test.debug + the runtime
    // binary; `.text` is NOBITS in the .debug file, so the disassembly is read from the runtime one):
    //
    //   Server_MovePlayer_Implementation  @0x90edc90 is 0x5c bytes and is *only* the gate: on
    //     success it tail-jumps to UAdminComponent::MovePlayer @0x90edcf0, which iterates
    //     AVeinPlayerCharacter, matches on Controller and fires
    //     AVeinCharacter::Client_StartTeleport -> SetActorLocation -> WaitForLoadPostTeleport.
    //     MovePlayer re-reads IsAdmin only to log, and proceeds either way - so it is the game's own
    //     complete teleport path with no gate on it.
    //   Server_GiveItem_Implementation    @0x90ee830 builds the item itself:
    //     FVirtualItemInstance::FromItem(TSubclassOf<UItem>) -> SetStack(n) ->
    //     character->Inventory (AVeinCharacter::Inventory, +0x978) ->AddItem(inst,false,true,0),
    //     splitting the count by the item CDO's MaxStack. There is no add-by-class-and-count
    //     function anywhere in the binary.
    //   Server_KickPlayer_Implementation  @0x90ed210 finds the controller whose PlayerState matches,
    //     requires a real UNetConnection, then Destroy()s the pawn, sends
    //     AVeinPlayerController::Client_Kicked() and Destroy()s the controller.
    //
    // Signatures are left unpinned (base-name match) on purpose: these are local `t` symbols whose
    // demangled parameter spelling (FVector vs UE::Math::TVector<double>) differs between demanglers.
    FN("UAdminComponent::MovePlayer", nullptr, false),
    FN("FVirtualItemInstance::FromItem", nullptr, false),
    FN("FVirtualItemInstance::SetStack", nullptr, false),
    FN("FVirtualItemInstance::~FVirtualItemInstance", nullptr, false),
    FN("UBaseInventoryComponent::AddItem", nullptr, false),
    FN("AActor::SetActorLocation", nullptr, false),
    FN("AVeinPlayerController::Client_Kicked", nullptr, false),
    FN("AActor::Destroy", nullptr, false),

    // chat: the replicated multicast entry (NOT _Implementation - that one never leaves the server).
    // NetMulticast_BroadcastServerMessage is the same mechanism WITHOUT a sender parameter, and is
    // the primary broadcast path: SendChat null-derefs its sender (lane L3d / L6b crash).
    FN("AVeinGameStateBase::NetMulticast_BroadcastServerMessage",
       "AVeinGameStateBase::NetMulticast_BroadcastServerMessage(FString const&)", false),
    FN("AVeinGameStateBase::NetMulticast_SendChat",
       "AVeinGameStateBase::NetMulticast_SendChat(AVeinPlayerState*, FString const&, EChatSegment, "
       "TSubclassOf<UChatCommand>, FVector_NetQuantize)", false),
    FN("AVeinPlayerController::Client_SendNotification",
       "AVeinPlayerController::Client_SendNotification(FText const&, ENotificationType, float)", false),

    // moderation: Vein's own string-keyed ban list on the game state (works offline, persists)
    FN("AVeinGameStateBase::BanID", "AVeinGameStateBase::BanID(FString, FString)", false),
    FN("AVeinGameStateBase::UnbanID", "AVeinGameStateBase::UnbanID(FString)", false),
    FN("AVeinGameStateBase::ReloadBans", "AVeinGameStateBase::ReloadBans()", false),

    // items / entities / locations
    FN("UItem::StaticClass", nullptr, false),
    FN("UItem::GetDefaultNameInvariant", "UItem::GetDefaultNameInvariant() const", false),
    FN("UBaseInventoryComponent::StaticClass", nullptr, false),
    FN("UBaseInventoryComponent::GetItemIDs", "UBaseInventoryComponent::GetItemIDs() const", false),
    FN("AVeinZombieCharacter::StaticClass", nullptr, false),
    FN("AVeinAnimalCharacter::StaticClass", nullptr, false),
    FN("AVeinPlayerCharacter::StaticClass", nullptr, false),
    FN("ALocationMarker::StaticClass", nullptr, false),

    // debug-only kill-nearest (proves entity-killed without a human swinging a weapon)
    FN("UGameplayStatics::ApplyDamage",
       "UGameplayStatics::ApplyDamage(AActor*, float, AController*, AActor*, TSubclassOf<UDamageType>)", false),

    // --- lane L2: event sources -------------------------------------------------------------------
    // Appended, never edited (L1's handover: add with FN(), pin the signature when the base name has
    // overloads). Every address, with the demangled .sym line it came from, is in docs/events-design.md.
    // Names lane L3 already declares above are deliberately not repeated.
    //
    // The live game mode is AVeinGameModeBase and it *overrides* PostLogin and Logout, so its vtable
    // holds the override; hooking only the engine base would bind vtables that never fire.
    FN("AVeinGameModeBase::PostLogin", "AVeinGameModeBase::PostLogin(APlayerController*)", false),
    FN("AVeinGameModeBase::Logout", "AVeinGameModeBase::Logout(AController*)", false),
    FN("AVeinGameModeBase::StaticClass", nullptr, false),
    // Disconnect backstops: the Vein player controller overrides neither, so the engine addresses are
    // what the live BP_VeinPlayerController_C vtable holds.
    FN("AController::Destroyed", "AController::Destroyed()", false),
    FN("APlayerController::Destroyed", "APlayerController::Destroyed()", false),
    // Identity fallback for when AVeinPlayerState::PlayerID is not readable yet.
    FN("FUniqueNetIdSteam::ToString", "FUniqueNetIdSteam::ToString() const", false),
    // One universal death event lives on the health component (players, zombies, animals alike).
    FN("UHealthComponent::StaticClass", nullptr, false),
    FN("AVeinBaseCharacter::StaticClass", nullptr, false),
    FN("AVeinAIController::StaticClass", nullptr, false),
    FN("AVeinBasePlayerController::StaticClass", nullptr, false),

    // --- lane L3b: admin grant --------------------------------------------------------------------
    // Appended, never edited. The grant itself goes through UObject::ProcessEvent on the live game
    // session (FindFunction("SetAdmin")), so none of these is on the critical path; they are declared
    // so that /health.diagnostics.resolve proves the functions exist on the deployed build and so a
    // direct call stays available as a fallback. There is deliberately no SetSuperAdmin entry: no
    // spelling of it exists anywhere in the depot .sym for build f0653d6e815bb2c3.
    FN("AVeinGameSession::SetAdmin", "AVeinGameSession::SetAdmin(FString, bool)", false),
    FN("AVeinGameSession::execSetAdmin", "AVeinGameSession::execSetAdmin(UObject*, FFrame&, void*)", false),
    FN("AVeinPlayerState::SetAdmin", "AVeinPlayerState::SetAdmin(bool)", false),
    FN("AVeinPlayerState::SetAdminPermissionsFromID", "AVeinPlayerState::SetAdminPermissionsFromID()", false),
    FN("AVeinPlayerState::IsAdmin", "AVeinPlayerState::IsAdmin() const", false),
    FN("UAdminComponent::IsAdmin", "UAdminComponent::IsAdmin() const", false),
    FN("UAdminComponent::IsSuperAdmin", "UAdminComponent::IsSuperAdmin() const", false),
};
#undef FN
#undef DATA

const size_t kWantedCount = sizeof(kWanted) / sizeof(kWanted[0]);

// Which L1-owned capability a wanted name backs. Everything else is diagnostic only. The names are
// namespaced under `symbols.` so lanes L2/L3 can own the connector-facing capability names.
struct CapMap {
    const char* want;
    const char* capability;
};
const CapMap kCapabilities[] = {
    {"UObject::ProcessEvent", "symbols.objectModel"},
    {"UObject::FindFunction", "symbols.objectModel"},
    {"UStruct::FindPropertyByName", "symbols.objectModel"},
    {"FName::FName", "symbols.objectModel"},
    {"FName::ToStringInto", "symbols.objectModel"},
    {"GetObjectsOfClass", "symbols.enumeration"},
    {"GetObjectsWithOuter", "symbols.enumeration"},
    {"StaticFindObjectPath", "symbols.enumeration"},
    {"UObjectBaseUtility::GetPathName", "symbols.objectPaths"},
    {"UEngine::Exec", "symbols.console"},
    {"FOutputDeviceRedirector::Get", "symbols.console"},
    {"RequestEngineExit", "symbols.lifecycle"},
};
const size_t kCapabilityCount = sizeof(kCapabilities) / sizeof(kCapabilities[0]);

std::vector<SymEntry> g_entries;
std::unordered_map<std::string, size_t> g_index;
bool g_ready = false;
bool g_cacheHit = false;
uint64_t g_scanMs = 0;
std::string g_symPath, g_debugPath;
size_t g_processEventSlot = SIZE_MAX;
std::string g_processEventSlotHow = "unresolved";
size_t g_resolved = 0, g_requiredWanted = 0, g_requiredResolved = 0;

// Per-strategy bookkeeping for /health.
struct StrategyStat {
    const char* name;
    bool available = false;
    std::string detail;
    size_t resolved = 0;
    uint64_t ms = 0;
};
StrategyStat g_strategies[] = {
    {"symtab", false, "not run", 0, 0},
    {"depotsym", false, "not run", 0, 0},
    // The depot also ships a separate, unstripped `.debug` companion (pointed at by
    // .gnu_debuglink). Its .symtab is a superset of both the `.sym` database and .dynsym - it
    // carries the LOCAL symbols the others drop. It is large, so it is only opened for the names
    // the cheaper sources could not supply.
    {"debugsym", false, "not run", 0, 0},
    {"dynsym", false, "not run", 0, 0},
    {"signature", false, "not run", 0, 0},
};
const size_t kStrategyCount = sizeof(g_strategies) / sizeof(g_strategies[0]);

}  // namespace

// ---------------------------------------------------------------------------------------------
// ELF

namespace {
struct MappedFile {
    const uint8_t* p = nullptr;
    size_t len = 0;
    int fd = -1;
    bool open(const std::string& path) {
        fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) return false;
        struct stat st;
        if (fstat(fd, &st) != 0 || st.st_size <= 0) { ::close(fd); fd = -1; return false; }
        len = (size_t)st.st_size;
        void* m = mmap(nullptr, len, PROT_READ, MAP_PRIVATE, fd, 0);
        if (m == MAP_FAILED) { ::close(fd); fd = -1; return false; }
        p = (const uint8_t*)m;
        return true;
    }
    void close() {
        if (p) munmap((void*)p, len);
        if (fd >= 0) ::close(fd);
        p = nullptr; fd = -1; len = 0;
    }
    ~MappedFile() { close(); }
};

// The runtime slide of the main executable. 0 for a non-PIE image (ET_EXEC).
uint64_t MainImageSlide() {
    // dl_iterate_phdr's first entry is always the main program.
    struct Ctx { uint64_t base = 0; bool got = false; } ctx;
    dl_iterate_phdr(
        [](struct dl_phdr_info* info, size_t, void* data) -> int {
            auto* c = (Ctx*)data;
            c->base = info->dlpi_addr;
            c->got = true;
            return 1;  // stop after the first (the main executable)
        },
        &ctx);
    return ctx.got ? ctx.base : 0;
}
}  // namespace

bool ResolveCore::ParseElfBuffer(const uint8_t* p, size_t len, ElfInfo& out) {
    if (len < sizeof(Elf64_Ehdr) || memcmp(p, ELFMAG, SELFMAG) != 0) { out.error = "not an ELF"; return false; }
    auto* eh = (const Elf64_Ehdr*)p;
    if (eh->e_ident[EI_CLASS] != ELFCLASS64) { out.error = "not ELF64"; return false; }
    out.pie = eh->e_type == ET_DYN;
    if (eh->e_phoff + (size_t)eh->e_phnum * eh->e_phentsize > len) { out.error = "bad phdr table"; return false; }
    bool haveBase = false;
    for (int i = 0; i < eh->e_phnum; i++) {
        auto* ph = (const Elf64_Phdr*)(p + eh->e_phoff + (size_t)i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD) continue;
        if (!haveBase) { out.loadBase = ph->p_vaddr; haveBase = true; }
        if (ph->p_flags & PF_X) out.execRanges.push_back({ph->p_vaddr, ph->p_vaddr + ph->p_memsz});
    }
    if (eh->e_shoff == 0 || eh->e_shoff + (size_t)eh->e_shnum * eh->e_shentsize > len) {
        out.error = "no section headers";
        return haveBase;  // still usable, just without the cross-check anchors
    }
    auto sec = [&](int i) { return (const Elf64_Shdr*)(p + eh->e_shoff + (size_t)i * eh->e_shentsize); };
    if (eh->e_shstrndx >= eh->e_shnum) { out.error = "bad shstrndx"; return haveBase; }
    const Elf64_Shdr* shstr = sec(eh->e_shstrndx);
    if (shstr->sh_offset + shstr->sh_size > len) { out.error = "bad shstrtab"; return haveBase; }
    const char* names = (const char*)(p + shstr->sh_offset);
    for (int i = 0; i < eh->e_shnum; i++) {
        const Elf64_Shdr* s = sec(i);
        if (s->sh_name >= shstr->sh_size) continue;
        const char* n = names + s->sh_name;
        if (!strcmp(n, ".text")) { out.textAddr = s->sh_addr; out.textSize = s->sh_size; out.textOff = s->sh_offset; }
        else if (!strcmp(n, ".init")) out.initAddr = s->sh_addr;
        else if (!strcmp(n, ".fini")) out.finiAddr = s->sh_addr;
        else if (!strcmp(n, ".rodata")) { out.rodataAddr = s->sh_addr; out.rodataSize = s->sh_size; out.rodataOff = s->sh_offset; }
        else if (!strcmp(n, ".symtab")) { out.hasSymtab = true; out.symtabCount = s->sh_entsize ? s->sh_size / s->sh_entsize : 0; }
        else if (!strcmp(n, ".dynsym")) { out.hasDynsym = true; out.dynsymCount = s->sh_entsize ? s->sh_size / s->sh_entsize : 0; }
        else if (!strcmp(n, ".note.gnu.build-id") && s->sh_offset + s->sh_size <= len && s->sh_size >= sizeof(Elf64_Nhdr)) {
            auto* nh = (const Elf64_Nhdr*)(p + s->sh_offset);
            const uint8_t* desc = (const uint8_t*)(nh + 1) + ((nh->n_namesz + 3) & ~3u);
            static const char* hex = "0123456789abcdef";
            out.buildId.clear();
            for (uint32_t k = 0; k < nh->n_descsz && k < 64; k++) {
                out.buildId += hex[desc[k] >> 4];
                out.buildId += hex[desc[k] & 0xf];
            }
        }
    }
    out.ok = haveBase && out.textAddr != 0;
    if (!out.ok && out.error.empty()) out.error = "missing PT_LOAD or .text";
    return out.ok;
}

size_t ResolveCore::ForEachSymbol(const uint8_t* p, size_t len, bool dynamic,
                                  const std::function<void(const SymbolRec&)>& fn) {
    if (len < sizeof(Elf64_Ehdr) || memcmp(p, ELFMAG, SELFMAG) != 0) return 0;
    auto* eh = (const Elf64_Ehdr*)p;
    if (eh->e_shoff == 0 || eh->e_shoff + (size_t)eh->e_shnum * eh->e_shentsize > len) return 0;
    auto sec = [&](int i) { return (const Elf64_Shdr*)(p + eh->e_shoff + (size_t)i * eh->e_shentsize); };
    if (eh->e_shstrndx >= eh->e_shnum) return 0;
    const Elf64_Shdr* shstr = sec(eh->e_shstrndx);
    if (shstr->sh_offset + shstr->sh_size > len) return 0;
    const char* names = (const char*)(p + shstr->sh_offset);
    const char* symName = dynamic ? ".dynsym" : ".symtab";
    const char* strName = dynamic ? ".dynstr" : ".strtab";
    const Elf64_Shdr *symtab = nullptr, *strtab = nullptr;
    for (int i = 0; i < eh->e_shnum; i++) {
        const Elf64_Shdr* s = sec(i);
        if (s->sh_name >= shstr->sh_size) continue;
        const char* n = names + s->sh_name;
        if (!strcmp(n, symName)) symtab = s;
        else if (!strcmp(n, strName)) strtab = s;
    }
    if (!symtab || !strtab || !symtab->sh_entsize) return 0;
    if (symtab->sh_offset + symtab->sh_size > len || strtab->sh_offset + strtab->sh_size > len) return 0;
    const char* strs = (const char*)(p + strtab->sh_offset);
    auto* syms = (const Elf64_Sym*)(p + symtab->sh_offset);
    size_t n = symtab->sh_size / sizeof(Elf64_Sym);
    for (size_t i = 0; i < n; i++) {
        if (syms[i].st_name >= strtab->sh_size) continue;
        SymbolRec r;
        r.name = strs + syms[i].st_name;
        r.value = syms[i].st_value;
        r.size = syms[i].st_size;
        r.info = syms[i].st_info;
        fn(r);
    }
    return n;
}

const ElfInfo& Elf() {
    static const ElfInfo info = [] {
        ElfInfo e;
        MappedFile f;
        if (!f.open(ExePath())) {
            e.error = "cannot mmap /proc/self/exe";
            return e;
        }
        ResolveCore::ParseElfBuffer(f.p, f.len, e);
        if (e.pie) e.slide = MainImageSlide();
        return e;
    }();
    return info;
}

bool IsExecutableAddr(uint64_t addr) {
    const ElfInfo& e = Elf();
    for (auto& r : e.execRanges)
        if (addr >= r.first + e.slide && addr < r.second + e.slide) return true;
    return false;
}

// Enumerates .dynsym once and keeps every `_ZTV*` entry (runtime addresses).
const std::vector<std::pair<std::string, std::pair<uint64_t, uint64_t>>>& VTableSymbols() {
    static const std::vector<std::pair<std::string, std::pair<uint64_t, uint64_t>>> tables = [] {
        std::vector<std::pair<std::string, std::pair<uint64_t, uint64_t>>> out;
        MappedFile f;
        if (!f.open(ExePath())) return out;
        uint64_t slide = Elf().slide;
        ResolveCore::ForEachSymbol(f.p, f.len, true, [&](const ResolveCore::SymbolRec& s) {
            if (!s.value || s.size < 16) return;
            if (strncmp(s.name, "_ZTV", 4) != 0) return;
            out.push_back({s.name, {s.value + slide, s.size}});
        });
        PluginLog("resolve: %zu exported vtables", out.size());
        return out;
    }();
    return tables;
}

uint64_t DynSymAddr(const char* name) {
    if (void* p = dlsym(RTLD_DEFAULT, name)) return (uint64_t)(uintptr_t)p;
    MappedFile f;
    if (!f.open(ExePath())) return 0;
    uint64_t found = 0, slide = Elf().slide;
    ResolveCore::ForEachSymbol(f.p, f.len, true, [&](const ResolveCore::SymbolRec& s) {
        if (!found && s.value && !strcmp(s.name, name)) found = s.value + slide;
    });
    return found;
}

// ---------------------------------------------------------------------------------------------
// name matching / demangling

std::string ResolveCore::Demangle(const char* mangled) {
    if (!mangled || mangled[0] != '_' || mangled[1] != 'Z') return "";
    int status = 0;
    char* d = abi::__cxa_demangle(mangled, nullptr, nullptr, &status);
    if (status != 0 || !d) {
        if (d) free(d);
        return "";
    }
    std::string out(d);
    free(d);
    return out;
}

std::string ResolveCore::BaseName(const std::string& key) {
    size_t c = key.rfind("::");
    return c == std::string::npos ? key : key.substr(c + 2);
}

bool ResolveCore::NameMatches(const std::string& line, const char* key, const char* sig) {
    if (sig && *sig) return line == sig;
    size_t par = line.find('(');
    std::string base = par == std::string::npos ? line : line.substr(0, par);
    // Strip a trailing " const" that a parameterless const method would not have, and any
    // return type the demangler prefixed (it does not for functions, only for some templates).
    return base == key;
}

// ---------------------------------------------------------------------------------------------
// signatures

bool ResolveCore::ParseSignature(const std::string& pattern, Signature& out, std::string& err) {
    out.bytes.clear();
    size_t i = 0;
    while (i < pattern.size()) {
        char c = pattern[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { i++; continue; }
        if (c == '?') {
            out.bytes.push_back(-1);
            i++;
            if (i < pattern.size() && pattern[i] == '?') i++;
            continue;
        }
        auto hex = [](char h, int& v) {
            if (h >= '0' && h <= '9') { v = h - '0'; return true; }
            if (h >= 'a' && h <= 'f') { v = h - 'a' + 10; return true; }
            if (h >= 'A' && h <= 'F') { v = h - 'A' + 10; return true; }
            return false;
        };
        int hi = 0, lo = 0;
        if (i + 1 >= pattern.size() || !hex(pattern[i], hi) || !hex(pattern[i + 1], lo)) {
            err = "not a hex byte pair at offset " + std::to_string(i);
            out.bytes.clear();
            return false;
        }
        out.bytes.push_back((hi << 4) | lo);
        i += 2;
    }
    if (out.bytes.empty()) { err = "empty pattern"; return false; }
    // A pattern that starts or ends with a wildcard hides how specific it really is.
    if (out.bytes.front() < 0) { err = "pattern must not start with a wildcard"; out.bytes.clear(); return false; }
    return true;
}

size_t ResolveCore::ScanSignature(const uint8_t* hay, size_t len, const Signature& sig, size_t& firstOff,
                                  size_t maxHits) {
    firstOff = 0;
    if (sig.bytes.empty() || len < sig.bytes.size()) return 0;
    size_t hits = 0;
    const size_t n = sig.bytes.size();
    const uint8_t first = (uint8_t)sig.bytes[0];
    for (size_t i = 0; i + n <= len; i++) {
        if (hay[i] != first) continue;
        size_t k = 1;
        for (; k < n; k++) {
            int want = sig.bytes[k];
            if (want >= 0 && hay[i + k] != (uint8_t)want) break;
        }
        if (k != n) continue;
        if (!hits) firstOff = i;
        if (++hits >= maxHits) break;
    }
    return hits;
}

// ---------------------------------------------------------------------------------------------
// depot .sym scan (Dragonwilds format)

namespace {
#pragma pack(push, 1)
struct SymRecord {
    uint64_t rva;
    uint32_t line;
    uint32_t fileOff;
    uint32_t nameOff;
};
#pragma pack(pop)
static_assert(sizeof(SymRecord) == 20, "sym record must be 20 bytes");

std::string SymPath() {
    std::string p = ConfigValue("TAKARO_SYM_PATH", "");
    if (!p.empty()) return p;
    return ExePath() + ".sym";
}

std::string CachePath() { return PluginDataDir() + "/symcache.json"; }

// The unstripped companion the depot ships next to the binary (.gnu_debuglink points at it).
// TAKARO_DEBUG_SYM_PATH overrides it, exactly like TAKARO_SYM_PATH does for the .sym database.
std::string DebugPath() {
    std::string p = ConfigValue("TAKARO_DEBUG_SYM_PATH", "");
    if (!p.empty()) return p;
    return ExePath() + ".debug";
}

bool Unresolved(const SymEntry& e) { return e.addr == 0; }

// ---- strategy 1: ELF .symtab ------------------------------------------------------------------
// Names are mangled. The Itanium encoding embeds every identifier literally (`12ProcessEvent`), so
// a strstr on the base name is a sound and very cheap pre-filter before __cxa_demangle.
size_t ScanElfSymbols(const std::string& path, bool dynamic, const char* how, std::string& detail) {
    MappedFile f;
    if (!f.open(path)) { detail = "cannot mmap " + path; return 0; }

    // base identifier -> wanted indices still missing
    std::unordered_map<std::string, std::vector<size_t>> byBase;
    for (size_t i = 0; i < g_entries.size(); i++) {
        if (!Unresolved(g_entries[i])) continue;
        byBase[ResolveCore::BaseName(g_entries[i].name)].push_back(i);
    }
    if (byBase.empty()) { detail = "nothing left to resolve"; return 0; }

    size_t visited = 0, demangled = 0, hits = 0;
    uint64_t slide = Elf().slide;
    visited = ResolveCore::ForEachSymbol(f.p, f.len, dynamic, [&](const ResolveCore::SymbolRec& s) {
        if (!s.value || !s.name || !s.name[0]) return;
        unsigned type = s.info & 0xf;
        if (type != STT_FUNC && type != STT_OBJECT && type != STT_NOTYPE) return;
        // Plain C names (RequestEngineExit is C++ but _init/_fini/_start are not).
        for (auto& kv : byBase) {
            if (strcmp(s.name, kv.first.c_str()) != 0) continue;
            for (size_t idx : kv.second) {
                SymEntry& e = g_entries[idx];
                if (!Unresolved(e) || e.name != kv.first) continue;
                e.rva = s.value;
                e.addr = s.value + slide;
                e.signature = s.name;
                e.how = how;
                e.records = 1;
                hits++;
            }
        }
        if (s.name[0] != '_' || s.name[1] != 'Z') return;
        bool interesting = false;
        for (auto& kv : byBase) {
            if (strstr(s.name, kv.first.c_str())) { interesting = true; break; }
        }
        if (!interesting) return;
        std::string line = ResolveCore::Demangle(s.name);
        if (line.empty()) return;
        demangled++;
        for (auto& kv : byBase) {
            for (size_t idx : kv.second) {
                SymEntry& e = g_entries[idx];
                if (!Unresolved(e)) continue;
                if (!ResolveCore::NameMatches(line, e.name.c_str(), kWanted[idx].sig)) continue;
                e.rva = s.value;
                e.addr = s.value + slide;
                e.signature = line;
                e.how = how;
                e.records = 1;
                hits++;
            }
        }
    });
    detail = std::to_string(visited) + " symbols in " + path + ", " + std::to_string(demangled) + " demangled";
    return hits;
}

// ---- strategy 2: depot .sym --------------------------------------------------------------------
size_t ScanSymFile(std::string& detail) {
    MappedFile f;
    if (!f.open(g_symPath)) { detail = "no file at " + g_symPath; return 0; }
    if (f.len < 8) { detail = ".sym too small"; return 0; }
    uint32_t n;
    memcpy(&n, f.p, 4);
    size_t recBytes = (size_t)n * sizeof(SymRecord);
    if (4 + recBytes > f.len) { detail = ".sym record count does not fit the file"; return 0; }
    const uint8_t* namesBase = f.p + 4 + recBytes;
    size_t namesLen = f.len - 4 - recBytes;

    std::unordered_map<std::string, size_t> byExact, byBase;
    for (size_t i = 0; i < g_entries.size(); i++) {
        if (!Unresolved(g_entries[i])) continue;
        if (kWanted[i].sig) byExact[kWanted[i].sig] = i;
        else byBase[kWanted[i].key] = i;
    }
    if (byExact.empty() && byBase.empty()) { detail = "nothing left to resolve"; return 0; }

    // pass 1: the name-table offsets we care about
    std::unordered_map<uint32_t, std::vector<size_t>> offToIdx;
    size_t pos = 0;
    while (pos < namesLen) {
        const uint8_t* nl = (const uint8_t*)memchr(namesBase + pos, '\n', namesLen - pos);
        size_t end = nl ? (size_t)(nl - namesBase) : namesLen;
        size_t len = end - pos;
        if (len && len < 2048) {
            const char* s = (const char*)namesBase + pos;
            auto it = byExact.find(std::string(s, len));
            if (it != byExact.end()) offToIdx[(uint32_t)pos].push_back(it->second);
            if (!byBase.empty()) {
                const void* par = memchr(s, '(', len);
                size_t blen = par ? (size_t)((const char*)par - s) : len;
                auto bt = byBase.find(std::string(s, blen));
                if (bt != byBase.end()) offToIdx[(uint32_t)pos].push_back(bt->second);
            }
        }
        if (!nl) break;
        pos = end + 1;
    }
    // pass 2: lowest rva per wanted name, plus the record run
    std::vector<uint64_t> firstIdx(g_entries.size(), UINT64_MAX), lastIdx(g_entries.size(), 0);
    auto* recs = (const SymRecord*)(f.p + 4);
    for (uint32_t i = 0; i < n; i++) {
        auto it = offToIdx.find(recs[i].nameOff);
        if (it == offToIdx.end()) continue;
        for (size_t k : it->second) {
            SymEntry& e = g_entries[k];
            if (!e.rva || recs[i].rva < e.rva) {
                e.rva = recs[i].rva;
                size_t off = recs[i].nameOff;
                const uint8_t* nl = (const uint8_t*)memchr(namesBase + off, '\n', namesLen - off);
                e.signature.assign((const char*)namesBase + off, nl ? (size_t)(nl - namesBase - off) : 0);
            }
            e.records++;
            if (firstIdx[k] == UINT64_MAX) firstIdx[k] = i;
            lastIdx[k] = i;
        }
    }
    size_t hits = 0;
    const ElfInfo& elf = Elf();
    for (size_t k = 0; k < g_entries.size(); k++) {
        SymEntry& e = g_entries[k];
        if (!e.rva || !e.how.empty()) continue;
        e.addr = e.rva + elf.loadBase + elf.slide;
        e.contiguous = firstIdx[k] != UINT64_MAX && (lastIdx[k] - firstIdx[k] + 1) == e.records;
        e.how = "depotsym";
        hits++;
    }
    detail = std::to_string(n) + " records in " + g_symPath;
    return hits;
}

// ---- strategy 4: byte signatures ---------------------------------------------------------------
// A signature is only ever accepted when it matches **exactly once** inside .text. When an `xref`
// anchor is given, the scan is additionally required to land in a function that references that
// literal string, which is what keeps a short pattern honest.
size_t ScanSignatures(std::string& detail) {
    size_t wanted = 0;
    for (size_t i = 0; i < g_entries.size(); i++)
        if (Unresolved(g_entries[i]) && kWanted[i].pattern) wanted++;
    if (!wanted) { detail = "no signatures declared for the unresolved names"; return 0; }

    const ElfInfo& elf = Elf();
    MappedFile f;
    if (!f.open(ExePath()) || !elf.textSize || elf.textOff + elf.textSize > f.len) {
        detail = "cannot map .text";
        return 0;
    }
    const uint8_t* text = f.p + elf.textOff;
    size_t hits = 0, rejected = 0;
    for (size_t i = 0; i < g_entries.size(); i++) {
        if (!Unresolved(g_entries[i]) || !kWanted[i].pattern) continue;
        ResolveCore::Signature sig;
        std::string err;
        if (!ResolveCore::ParseSignature(kWanted[i].pattern, sig, err)) {
            PluginLog("resolve: signature for %s is malformed: %s", kWanted[i].key, err.c_str());
            continue;
        }
        size_t off = 0;
        size_t count = ResolveCore::ScanSignature(text, elf.textSize, sig, off, 2);
        if (count != 1) {
            rejected++;
            PluginLog("resolve: signature for %s matched %zu times (exactly one required) - rejected",
                      kWanted[i].key, count);
            continue;
        }
        SymEntry& e = g_entries[i];
        e.rva = elf.textAddr + off + (uint64_t)(int64_t)kWanted[i].adjust;
        e.addr = e.rva + elf.slide;
        e.how = "signature";
        e.signature = kWanted[i].pattern;
        e.records = 1;
        hits++;
    }
    detail = std::to_string(wanted) + " declared, " + std::to_string(rejected) + " rejected (not unique)";
    return hits;
}

// ---- symcache ----------------------------------------------------------------------------------
bool LoadCache() {
    std::string text;
    if (!ReadFile(CachePath(), text)) return false;
    JsonValue root;
    if (!JsonParse(text, root)) return false;
    auto* bid = root.get("buildId");
    if (!bid || !bid->isStr() || Elf().buildId.empty() || bid->str != Elf().buildId) return false;
    auto* names = root.get("names");
    if (!names || names->type != JsonValue::Object) return false;
    size_t hits = 0;
    const ElfInfo& elf = Elf();
    for (auto& e : g_entries) {
        auto* v = names->get(e.name);
        if (!v || v->type != JsonValue::Object) continue;
        auto* r = v->get("rva");
        if (!r || !r->isNum()) continue;
        uint64_t rva = (uint64_t)strtoull(r->str.c_str(), nullptr, 10);
        auto* h = v->get("how");
        // An entry the cache remembers as unresolved still counts as covered.
        if (!rva) {
            hits++;
            continue;
        }
        // `how` is stored so the cache never hides which strategy actually found the name; the
        // depot-sym case is the only one whose rva is relative to loadBase.
        std::string how = h && h->isStr() ? h->str : "";
        e.rva = rva;
        e.addr = rva + (how == "depotsym" ? elf.loadBase : 0) + elf.slide;
        auto* s = v->get("sig");
        e.signature = s && s->isStr() ? s->str : "";
        auto* c = v->get("records");
        e.records = c && c->isNum() ? (uint32_t)c->num : 0;
        auto* ct = v->get("contiguous");
        e.contiguous = !ct || ct->type != JsonValue::Bool || ct->b;
        e.how = how.empty() ? "symcache" : ("symcache:" + how);
        hits++;
    }
    // The cache must cover *every* wanted name: a partial hit means either a half-written file or a
    // plugin build that wants names the cache predates, and both must force a rescan.
    return hits == g_entries.size();
}

void SaveCache() {
    std::string o = "{\"buildId\":" + JsonStr(Elf().buildId) + ",\"loadBase\":" + std::to_string(Elf().loadBase) +
                    ",\"pluginVersion\":\"" TAKARO_PLUGIN_VERSION "\"" +
                    ",\"writtenAt\":" + JsonStr(IsoNowUtc()) + ",\"names\":{";
    bool first = true;
    for (auto& e : g_entries) {
        if (!first) o += ",";
        first = false;
        std::string how = e.how;
        if (how.rfind("symcache:", 0) == 0) how = how.substr(9);
        o += JsonStr(e.name) + ":{\"rva\":" + std::to_string(e.rva) + ",\"how\":" + JsonStr(how) +
             ",\"sig\":" + JsonStr(e.signature) + ",\"records\":" + std::to_string(e.records) +
             ",\"contiguous\":" + (e.contiguous ? "true" : "false") + "}";
    }
    o += "}}";
    if (!WriteFileAtomic(CachePath(), o)) PluginLog("resolve: could not write %s", CachePath().c_str());
}

// An address must sit in an executable segment and not start with 0x00 / 0xCC.
bool PlausibleCode(uint64_t addr) {
    if (!addr) return false;
    const ElfInfo& e = Elf();
    bool inText = e.textSize && addr >= e.textAddr + e.slide && addr < e.textAddr + e.textSize + e.slide;
    if (!inText && !IsExecutableAddr(addr)) return false;
    if (!MemReadable((const void*)(uintptr_t)addr, 1)) return false;
    uint8_t b = *(const uint8_t*)(uintptr_t)addr;
    return b != 0x00 && b != 0xCC;
}

void MeasureProcessEventSlot() {
    uint64_t vt = DynSymAddr("_ZTV7UObject");
    uint64_t pe = Resolve::Addr("UObject::ProcessEvent");
    if (!vt || !pe) {
        g_processEventSlotHow = vt ? "UObject::ProcessEvent unresolved" : "_ZTV7UObject not exported";
        return;
    }
    auto* words = (const uint64_t*)(uintptr_t)vt;
    size_t hits = 0, slot = SIZE_MAX;
    if (!MemReadable(words, 8 * 256)) {
        g_processEventSlotHow = "_ZTV7UObject is not readable";
        return;
    }
    // The _ZTV symbol points at the two header words (offset-to-top, typeinfo); slot 0 follows.
    for (size_t i = 2; i < 256; i++) {
        if (words[i] == pe) { slot = i - 2; hits++; }
    }
    if (hits != 1) {
        PluginLog("resolve: ProcessEvent appears %zu times in _ZTV7UObject (expected exactly 1)", hits);
        g_processEventSlotHow = "ProcessEvent appears " + std::to_string(hits) + " times in _ZTV7UObject";
        return;
    }
    g_processEventSlot = slot;
    g_processEventSlotHow = "_ZTV7UObject";
    PluginLog("resolve: ProcessEvent vtable slot = %zu (_ZTV7UObject)", slot);
}

}  // namespace

// ---------------------------------------------------------------------------------------------

void Resolve::Init() {
    if (g_ready) return;
    g_entries.clear();
    g_entries.reserve(kWantedCount);
    for (size_t i = 0; i < kWantedCount; i++) {
        SymEntry e;
        e.name = kWanted[i].key;
        e.required = kWanted[i].required;
        e.data = kWanted[i].data;
        g_entries.push_back(e);
        g_index[e.name] = i;
        if (e.required) g_requiredWanted++;
    }
    const ElfInfo& elf = Elf();
    PluginLog("resolve: exe=%s type=%s loadBase=0x%llx slide=0x%llx .text=0x%llx+0x%llx buildId=%s "
              "symtab=%zu dynsym=%zu",
              ExePath().c_str(), elf.pie ? "PIE" : "EXEC", (unsigned long long)elf.loadBase,
              (unsigned long long)elf.slide, (unsigned long long)elf.textAddr, (unsigned long long)elf.textSize,
              elf.buildId.c_str(), elf.symtabCount, elf.dynsymCount);
    if (!elf.ok) {
        PluginLog("resolve: ELF parse failed: %s", elf.error.c_str());
        g_ready = true;
        ApplyCapabilities();
        return;
    }
    g_symPath = SymPath();
    uint64_t t0 = NowMs();

    if (LoadCache()) {
        g_cacheHit = true;
        PluginLog("resolve: symcache hit (%s)", CachePath().c_str());
    } else {
        struct stat st;
        g_debugPath = DebugPath();
        g_strategies[0].available = elf.hasSymtab && elf.symtabCount > 0;
        g_strategies[1].available = ::stat(g_symPath.c_str(), &st) == 0 && st.st_size > 8;
        g_strategies[2].available = ::stat(g_debugPath.c_str(), &st) == 0 && st.st_size > 64;
        g_strategies[3].available = elf.hasDynsym && elf.dynsymCount > 0;
        g_strategies[4].available = elf.textSize > 0;

        for (size_t s = 0; s < kStrategyCount; s++) {
            uint64_t ts = NowMs();
            std::string detail = "skipped";
            size_t got = 0;
            if (g_strategies[s].available) {
                try {
                    if (s == 0) got = ScanElfSymbols(ExePath(), false, "symtab", detail);
                    else if (s == 1) got = ScanSymFile(detail);
                    else if (s == 2) got = ScanElfSymbols(g_debugPath, false, "debugsym", detail);
                    else if (s == 3) got = ScanElfSymbols(ExePath(), true, "dynsym", detail);
                    else got = ScanSignatures(detail);
                } catch (...) {
                    detail = "strategy threw";
                }
            } else if (s == 1) {
                detail = "no depot .sym at " + g_symPath;
            } else if (s == 2) {
                detail = "no depot .debug at " + g_debugPath;
            } else {
                detail = "section absent";
            }
            g_strategies[s].resolved = got;
            g_strategies[s].detail = detail;
            g_strategies[s].ms = NowMs() - ts;
            PluginLog("resolve: strategy %-9s -> %zu names in %llums (%s)", g_strategies[s].name, got,
                      (unsigned long long)g_strategies[s].ms, detail.c_str());
        }
        SaveCache();
    }
    g_scanMs = NowMs() - t0;

#ifdef TAKARO_DEBUG_CORRUPT_SIG
    // Degrade proof: the named symbol is forcibly discarded, so exactly the capability it backs goes
    // `degraded` while the server keeps running. Never defined in a release build.
    {
        auto it = g_index.find(TAKARO_DEBUG_CORRUPT_SIG);
        if (it != g_index.end()) {
            g_entries[it->second].addr = 0;
            g_entries[it->second].rva = 0;
            g_entries[it->second].how = "debug-corrupted";
            PluginLog("resolve: DEBUG_CORRUPT_SIG discarded %s", TAKARO_DEBUG_CORRUPT_SIG);
        } else {
            PluginLog("resolve: DEBUG_CORRUPT_SIG names '%s' which is not a wanted symbol",
                      TAKARO_DEBUG_CORRUPT_SIG);
        }
    }
#endif

    // Drop anything implausible rather than handing a bad pointer to a caller.
    g_resolved = 0;
    g_requiredResolved = 0;
    for (auto& e : g_entries) {
        if (!e.addr) continue;
        if (!e.data && !PlausibleCode(e.addr)) {
            PluginLog("resolve: REJECT %s addr=0x%llx (outside an executable segment, or padding)", e.name.c_str(),
                      (unsigned long long)e.addr);
            e.addr = 0;
            e.how = "rejected";
            continue;
        }
        if (!e.contiguous)
            PluginLog("resolve: %s record run is not contiguous (%u records) - address still used", e.name.c_str(),
                      e.records);
        g_resolved++;
        if (e.required) g_requiredResolved++;
    }

    MeasureProcessEventSlot();
    PluginLog("resolve: %zu/%zu names (%zu/%zu required) in %llums (%s)", g_resolved, g_entries.size(),
              g_requiredResolved, g_requiredWanted, (unsigned long long)g_scanMs, g_cacheHit ? "cache" : "scan");
    g_ready = true;
    ApplyCapabilities();
}

void Resolve::ApplyCapabilities() {
    std::unordered_map<std::string, std::vector<std::string>> missing;
    std::unordered_set<std::string> all;
    for (size_t i = 0; i < kCapabilityCount; i++) {
        all.insert(kCapabilities[i].capability);
        if (!Resolve::Addr(kCapabilities[i].want)) missing[kCapabilities[i].capability].push_back(kCapabilities[i].want);
    }
    auto& st = PluginState::Get();
    for (auto& cap : all) {
        auto it = missing.find(cap);
        if (it == missing.end()) {
            st.SetCapability(cap, "ok", "");
            continue;
        }
        std::string names;
        for (auto& n : it->second) names += (names.empty() ? "" : ", ") + n;
        st.SetCapability(cap, "degraded", "unresolved: " + names);
    }
    st.SetCapability("symbolResolution",
                     g_requiredResolved == g_requiredWanted && g_requiredWanted > 0 ? "ok" : "degraded",
                     std::to_string(g_requiredResolved) + "/" + std::to_string(g_requiredWanted) +
                         " required symbols resolved");
}

bool Resolve::Ready() { return g_ready; }

uint64_t Resolve::Addr(const char* name) {
    auto it = g_index.find(name);
    if (it == g_index.end()) return 0;
    return g_entries[it->second].addr;
}

const SymEntry* Resolve::Entry(const char* name) {
    auto it = g_index.find(name);
    if (it == g_index.end()) return nullptr;
    return &g_entries[it->second];
}

const std::vector<SymEntry>& Resolve::All() { return g_entries; }

size_t Resolve::ProcessEventSlot() { return g_processEventSlot; }

void Resolve::SetProcessEventSlot(size_t slot, const char* how) {
    if (slot == SIZE_MAX) return;
    g_processEventSlot = slot;
    g_processEventSlotHow = how ? how : "live vtable";
    PluginLog("resolve: ProcessEvent vtable slot = %zu (%s)", slot, g_processEventSlotHow.c_str());
}

std::string Resolve::StatsJson() {
    std::string strat = "[";
    for (size_t i = 0; i < kStrategyCount; i++) {
        if (i) strat += ",";
        strat += "{\"name\":\"" + std::string(g_strategies[i].name) + "\",\"available\":" +
                 (g_strategies[i].available ? "true" : "false") +
                 ",\"resolved\":" + std::to_string(g_strategies[i].resolved) +
                 ",\"ms\":" + std::to_string(g_strategies[i].ms) + ",\"detail\":" + JsonStr(g_strategies[i].detail) +
                 "}";
    }
    strat += "]";
    return "{\"resolved\":" + std::to_string(g_resolved) + ",\"wanted\":" + std::to_string(g_entries.size()) +
           ",\"required\":" + std::to_string(g_requiredWanted) +
           ",\"requiredResolved\":" + std::to_string(g_requiredResolved) + ",\"processEventSlot\":" +
           (g_processEventSlot == SIZE_MAX ? "null" : std::to_string(g_processEventSlot)) +
           ",\"processEventSlotHow\":" + JsonStr(g_processEventSlotHow) + ",\"strategies\":" + strat + "}";
}

std::string Resolve::CacheJson() {
    return "{\"path\":" + JsonStr(CachePath()) + ",\"symPath\":" + JsonStr(g_symPath) +
           ",\"debugSymPath\":" + JsonStr(g_debugPath) +
           ",\"buildId\":" + JsonStr(Elf().buildId) + ",\"hit\":" + (g_cacheHit ? "true" : "false") +
           ",\"loadMs\":" + std::to_string(g_scanMs) + "}";
}

std::vector<std::pair<std::string, bool>> Resolve::SelfChecks() {
    std::vector<std::pair<std::string, bool>> out;
    auto add = [&](const std::string& s, bool ok) { out.push_back({s, ok}); };
    const ElfInfo& e = Elf();
    char b[320];

    snprintf(b, sizeof b, "elf: %s loadBase=0x%llx slide=0x%llx text=0x%llx+0x%llx buildId=%s",
             e.pie ? "PIE" : "EXEC", (unsigned long long)e.loadBase, (unsigned long long)e.slide,
             (unsigned long long)e.textAddr, (unsigned long long)e.textSize, e.buildId.c_str());
    add(b, e.ok);

    // _init / _fini vs the section headers: proves the address arithmetic (slide + loadBase).
    uint64_t initA = Resolve::Addr("_init"), finiA = Resolve::Addr("_fini");
    bool ok = initA && e.initAddr && initA == e.initAddr + e.slide;
    snprintf(b, sizeof b, "_init: sym=0x%llx elfSection+slide=0x%llx", (unsigned long long)initA,
             (unsigned long long)(e.initAddr + e.slide));
    add(b, ok);
    ok = finiA && e.finiAddr && finiA == e.finiAddr + e.slide;
    snprintf(b, sizeof b, "_fini: sym=0x%llx elfSection+slide=0x%llx", (unsigned long long)finiA,
             (unsigned long long)(e.finiAddr + e.slide));
    add(b, ok);

    ok = g_processEventSlot != SIZE_MAX;
    snprintf(b, sizeof b, "processEventSlot: %s (%s; exactly one match required)",
             ok ? std::to_string(g_processEventSlot).c_str() : "unresolved", g_processEventSlotHow.c_str());
    add(b, ok);

    ok = g_requiredWanted > 0 && g_requiredResolved == g_requiredWanted;
    snprintf(b, sizeof b, "requiredSymbols: %zu of %zu required names resolved", g_requiredResolved, g_requiredWanted);
    add(b, ok);

    std::string strategies;
    for (size_t i = 0; i < kStrategyCount; i++)
        if (g_strategies[i].resolved)
            strategies += (strategies.empty() ? "" : ", ") + std::string(g_strategies[i].name) + "=" +
                          std::to_string(g_strategies[i].resolved);
    if (g_cacheHit) strategies = "symcache (build id " + e.buildId + ")";
    snprintf(b, sizeof b, "strategy: %zu of %zu names resolved via %s", g_resolved, g_entries.size(),
             strategies.empty() ? "nothing" : strategies.c_str());
    add(b, g_resolved > 0);
    return out;
}

// ---------------------------------------------------------------------------------------------
// /proc/self/maps guard

namespace {
struct MapRange {
    uintptr_t lo, hi;
    bool r, w;
    std::string path;
};
Mutex g_mapsLock;
std::vector<MapRange> g_maps;
uint64_t g_mapsAt = 0;

// LANE L9 (performance): the hot paths - our ProcessEvent detour and every reflected read behind it
// - call MemReadable several times per engine RPC. Taking a mutex and re-reading /proc/self/maps
// there cost 2.9 us per ProcessEvent call on the live rig. The authoritative table below is still
// rebuilt under the mutex, but each rebuild *publishes an immutable copy* that readers consult
// without any lock. A snapshot is never freed (a few KB, rebuilt only when the map layout changes),
// so a reader can hold the pointer for as long as it likes.
struct MapSnapshot {
    std::vector<MapRange> ranges;
};
std::atomic<const MapSnapshot*> g_snap{nullptr};

void PublishSnapshotLocked() {
    auto* snap = new MapSnapshot();
    snap->ranges = g_maps;
    g_snap.store(snap, std::memory_order_release);
}

// Lock-free lookup in the published snapshot. `false` only means "not in this snapshot"; the caller
// decides whether that is worth a locked refresh.
bool SnapshotHas(uintptr_t a, size_t len, bool needWrite) {
    const MapSnapshot* snap = g_snap.load(std::memory_order_acquire);
    if (!snap) return false;
    const std::vector<MapRange>& v = snap->ranges;
    auto it = std::upper_bound(v.begin(), v.end(), a,
                               [](uintptr_t x, const MapRange& m) { return x < m.lo; });
    if (it == v.begin()) return false;
    --it;
    return a >= it->lo && a + len <= it->hi && it->r && (!needWrite || it->w);
}

void ReloadMapsLocked() {
    std::string text;
    if (!ReadFile("/proc/self/maps", text)) return;
    std::vector<MapRange> out;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t e = text.find('\n', pos);
        std::string line = text.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
        pos = e == std::string::npos ? text.size() : e + 1;
        unsigned long long lo = 0, hi = 0;
        char perms[8] = {0};
        char pathbuf[1024] = {0};
        int got = sscanf(line.c_str(), "%llx-%llx %7s %*s %*s %*s %1023[^\n]", &lo, &hi, perms, pathbuf);
        if (got < 3) continue;
        out.push_back({(uintptr_t)lo, (uintptr_t)hi, perms[0] == 'r', perms[1] == 'w', pathbuf});
    }
    std::sort(out.begin(), out.end(), [](const MapRange& a, const MapRange& b) { return a.lo < b.lo; });
    g_maps.swap(out);
    g_mapsAt = NowMs();
    PublishSnapshotLocked();
}

bool Check(const void* addr, size_t len, bool needWrite) {
    if (!addr || !len) return false;
    uintptr_t a = (uintptr_t)addr;
    if (a + len < a) return false;
    if (SnapshotHas(a, len, needWrite)) return true;  // lock-free fast path (the overwhelming case)
    Guard g(g_mapsLock);
    for (int attempt = 0; attempt < 2; attempt++) {
        if (g_maps.empty() || (attempt == 1)) ReloadMapsLocked();
        auto it = std::upper_bound(g_maps.begin(), g_maps.end(), a,
                                   [](uintptr_t v, const MapRange& m) { return v < m.lo; });
        if (it != g_maps.begin()) {
            --it;
            if (a >= it->lo && a + len <= it->hi && it->r && (!needWrite || it->w)) return true;
        }
        // A miss may just mean a stale cache; refresh once (rate-limited) and retry.
        if (attempt == 0 && NowMs() - g_mapsAt < 250) return false;
    }
    return false;
}
}  // namespace

bool MemReadable(const void* addr, size_t len) { return Check(addr, len, false); }
bool MemWritable(const void* addr, size_t len) { return Check(addr, len, true); }
void MemMapsRefresh() {
    Guard g(g_mapsLock);
    ReloadMapsLocked();
}

std::string MemMapsSelfSoLine() {
    Dl_info info{};
    if (!dladdr((void*)&MemMapsSelfSoLine, &info) || !info.dli_fname) return "";
    std::string text, out;
    if (!ReadFile("/proc/self/maps", text)) return "";
    size_t pos = 0;
    while (pos < text.size()) {
        size_t e = text.find('\n', pos);
        std::string line = text.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
        pos = e == std::string::npos ? text.size() : e + 1;
        if (line.find(info.dli_fname) != std::string::npos) out += line + "\n";
    }
    return out;
}
