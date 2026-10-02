#include "sym.h"

#include <atomic>

#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <link.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <unordered_map>

// ---------------------------------------------------------------------------------------------
// The names we want. `sig` pins one overload; when it is null the lowest-rva overload wins.
// Everything here was verified present in the shipped .sym for build ++dominion+staging-CL-240163.
namespace {
struct Want {
    const char* key;
    const char* sig;  // exact demangled line, or nullptr for "base name, lowest rva"
};
const Want kWanted[] = {
    // --- anchors used by the boot cross-checks ---
    {"_init", "_init"},
    {"_fini", "_fini"},
    {"_start", "_start"},
    // --- UE object model ---
    {"UObject::ProcessEvent", "UObject::ProcessEvent(UFunction*, void*)"},
    {"UObject::FindFunction", "UObject::FindFunction(FName) const"},
    {"UStruct::FindPropertyByName", "UStruct::FindPropertyByName(FName) const"},
    {"StaticFindObject", "StaticFindObject(UClass*, UObject*, char16_t const*, bool)"},
    {"GetObjectsOfClass", "GetObjectsOfClass(UClass const*, TArray<UObject*, TSizedDefaultAllocator<32> >&, bool, EObjectFlags, EInternalObjectFlags)"},
    {"GetObjectsWithOuter", "GetObjectsWithOuter(UObjectBase const*, TArray<UObject*, TSizedDefaultAllocator<32> >&, bool, EObjectFlags, EInternalObjectFlags)"},
    {"UObjectBaseUtility::GetPathName", "UObjectBaseUtility::GetPathName(UObject const*) const"},
    {"FName::ToString", "FName::ToString() const"},
    {"FName::ToStringInto", "FName::ToString(FString&) const"},
    {"FName::FName", "FName::FName(char16_t const*, EFindName)"},
    {"FName::GetPlainNameString", nullptr},
    // --- engine / world ---
    {"UGameEngine::Tick", "UGameEngine::Tick(float, bool)"},
    {"UJgxGameEngine::Tick", "UJgxGameEngine::Tick(float, bool)"},
    {"UGameplayStatics::GetGameMode", nullptr},
    {"UGameplayStatics::GetGameState", nullptr},
    {"UGameplayStatics::GetPlayerController", nullptr},
    {"AGameStateBase::GetServerWorldTimeSeconds", nullptr},
    {"UEngine::Exec", "UEngine::Exec(UWorld*, char16_t const*, FOutputDevice&)"},
    {"UKismetSystemLibrary::ExecuteConsoleCommand", nullptr},
    {"FOutputDeviceRedirector::Get", nullptr},
    {"FOutputDeviceRedirector::AddOutputDevice", nullptr},
    {"RequestEngineExit", "RequestEngineExit(char16_t const*)"},
    // --- game mode / session (events + moderation; wired by L2/L3) ---
    {"ADominionGameMode::PreLogin", "ADominionGameMode::PreLogin(FString const&, FString const&, FUniqueNetIdRepl const&, FString&)"},
    {"ADominionGameMode::PostLogin", nullptr},
    {"ADominionGameMode::PreLogout", nullptr},
    {"ADominionGameMode::RequestSaveGame", nullptr},
    {"ADominionGameMode::SaveGame", nullptr},
    {"ADominionGameMode::CanSave", nullptr},
    {"AGameModeBase::Logout", nullptr},
    // AGameMode (not ...Base) overrides Logout, and the live Dominion game mode derives from it -
    // hooking only AGameModeBase::Logout binds vtables that never fire (lane L2, proven live).
    {"AGameMode::Logout", "AGameMode::Logout(AController*)"},
    {"ADominionGameSession::KickPlayer", "ADominionGameSession::KickPlayer(APlayerController*, FText const&)"},
    {"ADominionGameSession::BanPlayer", "ADominionGameSession::BanPlayer(APlayerController*, FText const&)"},
    {"ADominionGameSession::RemoveBanPlayer", nullptr},
    {"ADominionGameSession::RemoveKickPlayer", nullptr},
    {"UDedicatedServerSettings::GetBannedUsers", nullptr},
    {"UDedicatedServerSettings::SetBannedUsers", nullptr},
    {"UDedicatedServerSettings::PerformConfigSave", nullptr},
    {"UDedicatedServerSettings::TryGetKnownPlayer", nullptr},
    // The login check itself: ADominionGameMode::PreLogin -> UDomMatchmakerSubsystem::IsOnlineUserBanned
    // -> FNetworkMatcherSession::IsOnlineUserBanned, a set built at start-up from the settings. An unban
    // must go through the session's own remove or it only takes effect after a restart.
    {"UDomMatchmakerSubsystem::StaticClass", nullptr},
    {"UDomMatchmakerSubsystem::IsOnlineUserBanned", "UDomMatchmakerSubsystem::IsOnlineUserBanned(FUniqueNetIdWrapper const&) const"},
    {"FNetworkMatcherSession::IsOnlineUserBanned", "FNetworkMatcherSession::IsOnlineUserBanned(FUniqueNetIdWrapper const&) const"},
    {"FNetworkMatcherSession::RequestRemoveBanPlayer", nullptr},
    // --- chat ---
    {"UPlayerChatComponent::Server_SendChatMessage", "UPlayerChatComponent::Server_SendChatMessage(FChatMessageData const&, FChatPlayerFilterData const&)"},
    {"UPlayerChatComponent::execServer_SendChatMessage", "UPlayerChatComponent::execServer_SendChatMessage(UObject*, FFrame&, void*)"},
    {"UPlayerChatComponent::Client_ReceiveChatMessage", "UPlayerChatComponent::Client_ReceiveChatMessage(FChatMessageData const&)"},
    {"UPlayerChatComponent::Client_ReceivePlayerEvent", "UPlayerChatComponent::Client_ReceivePlayerEvent(FChatPlayerEventData const&)"},
    // --- items / inventory / teleport ---
    {"UDominionRuntimeBlueprintLibrary::TryGiveItemToPlayer", "UDominionRuntimeBlueprintLibrary::TryGiveItemToPlayer(APlayerController const*, UItemData const*, int)"},
    {"UInventoryComponent::GetAllItems", nullptr},
    {"UInventoryComponent::GetNumItems", nullptr},
    {"UInventoryComponent::GetItemFromSlot", nullptr},
    {"AActor::TeleportTo", nullptr},
    {"UTeleportationSubsystem::GetTargetLocationNames", nullptr},
    {"UTeleportationSubsystem::GetTargetLocationTransform", nullptr},
    {"UTeleportationSubsystem::TeleportWithParams", nullptr},
    // --- deaths / kills ---
    {"UDominionAISubsystem::OnAIKilled", nullptr},
    {"UProgressComponent::OnAIKilled", nullptr},
    {"ADominionPlayerCharacter::Client_SendDeathEventTelemetry", nullptr},
    // --- identity / cheats ---
    {"FUniqueNetIdEOS::ToString", "FUniqueNetIdEOS::ToString() const"},
    {"APlayerController::EnableCheats", nullptr},
    {"APlayerController::ConsoleCommand", nullptr},
    // --- allocator (FString buffers we receive must be freed with the game's allocator) ---
    {"FMemory::Malloc", "FMemory::Malloc(unsigned long, unsigned int)"},
    {"FMemory::Free", "FMemory::Free(void*)"},
    // --- class objects (StaticClass() is emitted per class; avoids name-based object lookups) ---
    {"StaticFindObjectPath", "StaticFindObject(UClass*, FTopLevelAssetPath, bool)"},
    {"UObject::StaticClass", nullptr},
    {"UClass::StaticClass", nullptr},
    {"UWorld::StaticClass", nullptr},
    {"UPlayerChatComponent::StaticClass", nullptr},
    {"UItemData::StaticClass", nullptr},
    {"APlayerController::StaticClass", nullptr},
    {"ADominionGameMode::StaticClass", nullptr},
    {"ADominionPlayerState::StaticClass", nullptr},
    {"UTeleportationSubsystem::StaticClass", nullptr},
    // --- version strings for /health ---
    {"FApp::GetBuildVersion", nullptr},
    {"FEngineVersion::Current", nullptr},
    {"FEngineVersion::ToString", nullptr},
    {"UGameplayStatics::GetGameInstance", nullptr},
    // --- lane L3 (actions): chat injection, moderation, item names, console ---
    {"ADominionPlayerController::GetPlayerChatComponent", "ADominionPlayerController::GetPlayerChatComponent() const"},
    {"ADominionPlayerController::ConstructPlayerChatSenderData", "ADominionPlayerController::ConstructPlayerChatSenderData(FChatPlayerSenderData&) const"},
    {"FText::FromString", "FText::FromString(FString const&)"},
    {"FTextInspector::GetDisplayString", "FTextInspector::GetDisplayString(FText const&)"},
    {"FUniqueNetIdEOS::ParseFromString", "FUniqueNetIdEOS::ParseFromString(FString const&)"},
    {"UDedicatedServerSettings::SynchronizeBannedListToKnownPlayers", nullptr},
    {"FOutputDeviceFile::Serialize", "FOutputDeviceFile::Serialize(char16_t const*, ELogVerbosity::Type, FName const&, double)"},
    {"ADominionGameStateBase::StaticClass", nullptr},
    {"ADominionPlayerController::StaticClass", nullptr},
    {"AWorldLodestone::StaticClass", nullptr},
    {"ADominionAICharacter::StaticClass", nullptr},
    {"UDominionRuntimeBlueprintLibrary::StaticClass", nullptr},
    // --- lane L3b: live ban enforcement, the real logout path, names and ids ---
    // PreLogin is where the game itself refuses a banned login ("PreLogin failure: PLogBanned"),
    // and the only place a refusal can happen without a restart.
    {"AGameModeBase::PreLogin", "AGameModeBase::PreLogin(FString const&, FString const&, FUniqueNetIdRepl const&, FString&)"},
    // ADominionGameMode::PostLogin fires but neither Logout override ever does: the Dominion player
    // controller overrides OnNetCleanup and defers its own logout, so that is the real disconnect path.
    {"ADominionPlayerController::OnNetCleanup", "ADominionPlayerController::OnNetCleanup(UNetConnection*)"},
    {"APlayerController::OnNetCleanup", "APlayerController::OnNetCleanup(UNetConnection*)"},
    {"ADominionPlayerController::Destroyed", "ADominionPlayerController::Destroyed()"},
    {"AGameSession::NotifyLogout", "AGameSession::NotifyLogout(APlayerController const*)"},
    // Character display name: the UFUNCTION path through ProcessEvent returns nothing, the native
    // symbols do not.
    {"ADominionPlayerState::GetCharacterDisplayName", "ADominionPlayerState::GetCharacterDisplayName() const"},
    {"UDisplayNameComponent::GetCharacterDisplayName", "UDisplayNameComponent::GetCharacterDisplayName(FDomOwnerGuid const&, FString const&) const"},
    {"UDisplayNameComponent::IsCharacterNameReady", "UDisplayNameComponent::IsCharacterNameReady(FDomOwnerGuid const&) const"},
    // Asset registry: the full AI bestiary is not loaded, so /entities asks the cooked registry.
    {"UAssetRegistryImpl::GetAssetsByClass", "UAssetRegistryImpl::GetAssetsByClass(FTopLevelAssetPath, TArray<FAssetData, TSizedDefaultAllocator<32> >&, bool) const"},
    // --- lane L3b: POST /debug/kill-nearest, the no-human way to fire a real AI kill ---
    {"UGameplayStatics::ApplyDamage", "UGameplayStatics::ApplyDamage(AActor*, float, AController*, AActor*, TSubclassOf<UDamageType>)"},
    {"UHealthComponent::DecreaseHealth", "UHealthComponent::DecreaseHealth(float, FString const&)"},
    {"UHealthComponent::GetLocalHealth", "UHealthComponent::GetLocalHealth() const"},
    {"UHealthComponent::StaticClass", nullptr},
    {"ADominionAICharacter::StaticClass", nullptr},
    // --- lane L3c: which weapon made the kill ---
    // The equipped main-hand item. ELoadoutSlot is a plain UENUM, so its numeric values are read
    // out of the live UEnum by name instead of being hard-coded.
    {"ULoadoutComponent::GetEquipmentFromSlot", "ULoadoutComponent::GetEquipmentFromSlot(ELoadoutSlot) const"},
    {"UEnum::GetValueByName", "UEnum::GetValueByName(FName, EGetByNameFlags) const"},
    {"ULoadoutComponent::StaticClass", nullptr},
    {"UEquipment::StaticClass", nullptr},
};
const size_t kWantedCount = sizeof(kWanted) / sizeof(kWanted[0]);

std::vector<SymEntry> g_entries;
std::unordered_map<std::string, size_t> g_index;
bool g_ready = false;
bool g_cacheHit = false;
uint64_t g_scanMs = 0;
std::string g_symPath;
size_t g_processEventSlot = SIZE_MAX;
size_t g_resolved = 0;
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

bool ParseElf(ElfInfo& out) {
    MappedFile f;
    if (!f.open(ExePath())) { out.error = "cannot mmap /proc/self/exe"; return false; }
    if (f.len < sizeof(Elf64_Ehdr) || memcmp(f.p, ELFMAG, SELFMAG) != 0) { out.error = "not an ELF"; return false; }
    auto* eh = (const Elf64_Ehdr*)f.p;
    if (eh->e_phoff + (size_t)eh->e_phnum * eh->e_phentsize > f.len) { out.error = "bad phdr table"; return false; }
    for (int i = 0; i < eh->e_phnum; i++) {
        auto* ph = (const Elf64_Phdr*)(f.p + eh->e_phoff + (size_t)i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD) continue;
        if (!out.loadBase) out.loadBase = ph->p_vaddr;
        if (ph->p_flags & PF_X) out.execRanges.push_back({ph->p_vaddr, ph->p_vaddr + ph->p_memsz});
    }
    if (eh->e_shoff == 0 || eh->e_shoff + (size_t)eh->e_shnum * eh->e_shentsize > f.len) {
        out.error = "no section headers";
        return out.loadBase != 0;  // still usable, just without the cross-check anchors
    }
    auto sec = [&](int i) { return (const Elf64_Shdr*)(f.p + eh->e_shoff + (size_t)i * eh->e_shentsize); };
    const Elf64_Shdr* shstr = sec(eh->e_shstrndx);
    const char* names = (const char*)(f.p + shstr->sh_offset);
    for (int i = 0; i < eh->e_shnum; i++) {
        const Elf64_Shdr* s = sec(i);
        if (s->sh_name >= shstr->sh_size) continue;
        const char* n = names + s->sh_name;
        if (!strcmp(n, ".text")) { out.textAddr = s->sh_addr; out.textSize = s->sh_size; }
        else if (!strcmp(n, ".init")) out.initAddr = s->sh_addr;
        else if (!strcmp(n, ".fini")) out.finiAddr = s->sh_addr;
        else if (!strcmp(n, ".rodata")) { out.rodataAddr = s->sh_addr; out.rodataSize = s->sh_size; }
        else if (!strcmp(n, ".note.gnu.build-id") && s->sh_offset + s->sh_size <= f.len) {
            auto* nh = (const Elf64_Nhdr*)(f.p + s->sh_offset);
            const uint8_t* desc = (const uint8_t*)(nh + 1) + ((nh->n_namesz + 3) & ~3u);
            static const char* hex = "0123456789abcdef";
            for (uint32_t k = 0; k < nh->n_descsz && k < 64; k++) {
                out.buildId += hex[desc[k] >> 4];
                out.buildId += hex[desc[k] & 0xf];
            }
        }
    }
    out.ok = out.loadBase != 0 && out.textAddr != 0;
    if (!out.ok && out.error.empty()) out.error = "missing PT_LOAD or .text";
    return out.ok;
}
}  // namespace

const ElfInfo& Elf() {
    static const ElfInfo info = [] {
        ElfInfo e;
        ParseElf(e);
        return e;
    }();
    return info;
}

bool IsExecutableAddr(uint64_t addr) {
    for (auto& r : Elf().execRanges)
        if (addr >= r.first && addr < r.second) return true;
    return false;
}

// Enumerates .dynsym once and keeps every `_ZTV*` entry.
const std::vector<std::pair<std::string, std::pair<uint64_t, uint64_t>>>& VTableSymbols() {
    static const std::vector<std::pair<std::string, std::pair<uint64_t, uint64_t>>> tables = [] {
        std::vector<std::pair<std::string, std::pair<uint64_t, uint64_t>>> out;
        MappedFile f;
        if (!f.open(ExePath())) return out;
        auto* eh = (const Elf64_Ehdr*)f.p;
        if (f.len < sizeof(Elf64_Ehdr) || eh->e_shoff == 0) return out;
        auto sec = [&](int i) { return (const Elf64_Shdr*)(f.p + eh->e_shoff + (size_t)i * eh->e_shentsize); };
        const Elf64_Shdr* shstr = sec(eh->e_shstrndx);
        const char* names = (const char*)(f.p + shstr->sh_offset);
        const Elf64_Shdr *dynsym = nullptr, *dynstr = nullptr;
        for (int i = 0; i < eh->e_shnum; i++) {
            const char* n = names + sec(i)->sh_name;
            if (!strcmp(n, ".dynsym")) dynsym = sec(i);
            else if (!strcmp(n, ".dynstr")) dynstr = sec(i);
        }
        if (!dynsym || !dynstr) return out;
        if (dynsym->sh_offset + dynsym->sh_size > f.len || dynstr->sh_offset + dynstr->sh_size > f.len) return out;
        const char* strs = (const char*)(f.p + dynstr->sh_offset);
        auto* syms = (const Elf64_Sym*)(f.p + dynsym->sh_offset);
        size_t n = dynsym->sh_size / sizeof(Elf64_Sym);
        for (size_t i = 0; i < n; i++) {
            if (syms[i].st_name >= dynstr->sh_size || !syms[i].st_value || syms[i].st_size < 16) continue;
            const char* nm = strs + syms[i].st_name;
            if (strncmp(nm, "_ZTV", 4) != 0) continue;
            out.push_back({nm, {syms[i].st_value, syms[i].st_size}});
        }
        PluginLog("sym: %zu exported vtables", out.size());
        return out;
    }();
    return tables;
}

uint64_t DynSymAddr(const char* name) {
    if (void* p = dlsym(RTLD_DEFAULT, name)) return (uint64_t)(uintptr_t)p;
    // Fallback: read .dynsym out of the on-disk image (non-PIE, so st_value is the runtime vaddr).
    MappedFile f;
    if (!f.open(ExePath())) return 0;
    auto* eh = (const Elf64_Ehdr*)f.p;
    if (f.len < sizeof(Elf64_Ehdr) || eh->e_shoff == 0) return 0;
    auto sec = [&](int i) { return (const Elf64_Shdr*)(f.p + eh->e_shoff + (size_t)i * eh->e_shentsize); };
    const Elf64_Shdr* shstr = sec(eh->e_shstrndx);
    const char* names = (const char*)(f.p + shstr->sh_offset);
    const Elf64_Shdr *dynsym = nullptr, *dynstr = nullptr;
    for (int i = 0; i < eh->e_shnum; i++) {
        const Elf64_Shdr* s = sec(i);
        const char* n = names + s->sh_name;
        if (!strcmp(n, ".dynsym")) dynsym = s;
        else if (!strcmp(n, ".dynstr")) dynstr = s;
    }
    if (!dynsym || !dynstr) return 0;
    if (dynsym->sh_offset + dynsym->sh_size > f.len || dynstr->sh_offset + dynstr->sh_size > f.len) return 0;
    const char* strs = (const char*)(f.p + dynstr->sh_offset);
    size_t n = dynsym->sh_size / sizeof(Elf64_Sym);
    auto* syms = (const Elf64_Sym*)(f.p + dynsym->sh_offset);
    for (size_t i = 0; i < n; i++)
        if (syms[i].st_name < dynstr->sh_size && !strcmp(strs + syms[i].st_name, name)) return syms[i].st_value;
    return 0;
}

// ---------------------------------------------------------------------------------------------
// .sym scan

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
    std::string p = ConfigValue("TAKARO_SYM_PATH", "symPath", "");
    if (!p.empty()) return p;
    return ExePath() + ".sym";
}

std::string CachePath() { return PluginDataDir() + "/symcache.json"; }

bool LoadCache() {
    std::string text;
    if (!ReadFile(CachePath(), text)) return false;
    JsonValue root;
    if (!JsonParse(text, root)) return false;
    auto* bid = root.get("buildId");
    if (!bid || !bid->isStr() || bid->str != Elf().buildId || Elf().buildId.empty()) return false;
    auto* names = root.get("names");
    if (!names || names->type != JsonValue::Object) return false;
    size_t hits = 0;
    for (auto& e : g_entries) {
        auto* v = names->get(e.name);
        if (!v || v->type != JsonValue::Object) continue;
        auto* r = v->get("rva");
        auto* s = v->get("sig");
        if (!r || !r->isNum()) continue;
        e.rva = (uint64_t)strtoull(r->str.c_str(), nullptr, 10);
        if (!e.rva) continue;
        e.addr = e.rva + Elf().loadBase;
        e.signature = s && s->isStr() ? s->str : "";
        auto* c = v->get("records");
        e.records = c && c->isNum() ? (uint32_t)c->num : 0;
        auto* ct = v->get("contiguous");
        e.contiguous = !ct || ct->type != JsonValue::Bool || ct->b;
        e.how = "symcache";
        hits++;
    }
    // The cache must cover *every* wanted name: a partial hit means either a half-written file or
    // a build of the plugin that wants names the cache predates, and both must force a rescan.
    return hits == g_entries.size();
}

void SaveCache() {
    std::string o = "{\"buildId\":" + JsonStr(Elf().buildId) + ",\"loadBase\":" + std::to_string(Elf().loadBase) +
                    ",\"writtenAt\":" + JsonStr(IsoNowUtc()) + ",\"names\":{";
    bool first = true;
    for (auto& e : g_entries) {
        if (!e.rva) continue;
        if (!first) o += ",";
        first = false;
        o += JsonStr(e.name) + ":{\"rva\":" + std::to_string(e.rva) + ",\"sig\":" + JsonStr(e.signature) +
             ",\"records\":" + std::to_string(e.records) + ",\"contiguous\":" + (e.contiguous ? "true" : "false") + "}";
    }
    o += "}}";
    if (!WriteFileAtomic(CachePath(), o)) PluginLog("sym: could not write %s", CachePath().c_str());
}

// One pass over the name table, one pass over the records.
bool ScanSymFile(std::string& err) {
    MappedFile f;
    if (!f.open(g_symPath)) { err = "cannot open " + g_symPath; return false; }
    if (f.len < 8) { err = ".sym too small"; return false; }
    uint32_t n;
    memcpy(&n, f.p, 4);
    size_t recBytes = (size_t)n * sizeof(SymRecord);
    if (4 + recBytes > f.len) { err = ".sym record count does not fit the file"; return false; }
    const uint8_t* namesBase = f.p + 4 + recBytes;
    size_t namesLen = f.len - 4 - recBytes;

    // name/signature -> wanted index
    std::unordered_map<std::string, size_t> byExact, byBase;
    for (size_t i = 0; i < g_entries.size(); i++) {
        if (kWanted[i].sig) byExact[kWanted[i].sig] = i;
        else byBase[kWanted[i].key] = i;
    }
    // pass 1: name-table offsets we care about. One line can serve several wanted entries: a pinned
    // overload ("FName::ToString(FString&) const") and a base-name entry ("FName::ToString") both match.
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
    // pass 2: lowest rva per wanted name, and the record run
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
    for (size_t k = 0; k < g_entries.size(); k++) {
        SymEntry& e = g_entries[k];
        if (!e.rva) continue;
        e.addr = e.rva + Elf().loadBase;
        e.contiguous = firstIdx[k] != UINT64_MAX && (lastIdx[k] - firstIdx[k] + 1) == e.records;
        e.how = "sym-scan";
    }
    return true;
}

// The address must sit inside .text and not start with 0x00 / 0xCC.
bool PlausibleCode(uint64_t addr) {
    if (!addr) return false;
    const ElfInfo& e = Elf();
    bool inText = e.textSize && addr >= e.textAddr && addr < e.textAddr + e.textSize;
    // .init / .fini live in an executable segment but outside .text.
    if (!inText && !IsExecutableAddr(addr)) return false;
    if (!MemReadable((const void*)(uintptr_t)addr, 1)) return false;
    uint8_t b = *(const uint8_t*)(uintptr_t)addr;
    return b != 0x00 && b != 0xCC;
}
}  // namespace

void Sym::Init() {
    if (g_ready) return;
    g_entries.clear();
    g_entries.reserve(kWantedCount);
    for (size_t i = 0; i < kWantedCount; i++) {
        SymEntry e;
        e.name = kWanted[i].key;
        g_entries.push_back(e);
        g_index[e.name] = i;
    }
    const ElfInfo& elf = Elf();
    PluginLog("sym: exe=%s loadBase=0x%llx .text=0x%llx+0x%llx buildId=%s", ExePath().c_str(),
              (unsigned long long)elf.loadBase, (unsigned long long)elf.textAddr, (unsigned long long)elf.textSize,
              elf.buildId.c_str());
    if (!elf.ok) {
        PluginLog("sym: ELF parse failed: %s", elf.error.c_str());
        g_ready = true;
        return;
    }
    g_symPath = SymPath();
    uint64_t t0 = NowMs();
    if (LoadCache()) {
        g_cacheHit = true;
        PluginLog("sym: symcache hit (%s)", CachePath().c_str());
    } else {
        std::string err;
        if (!ScanSymFile(err)) PluginLog("sym: scan failed: %s", err.c_str());
        else SaveCache();
    }
    g_scanMs = NowMs() - t0;

    // Drop anything implausible rather than handing a bad pointer to a caller.
    g_resolved = 0;
    for (auto& e : g_entries) {
        if (!e.addr) continue;
        if (!PlausibleCode(e.addr)) {
            PluginLog("sym: REJECT %s addr=0x%llx (outside .text or padding)", e.name.c_str(),
                      (unsigned long long)e.addr);
            e.addr = 0;
            e.how = "rejected";
            continue;
        }
        if (!e.contiguous)
            PluginLog("sym: %s record run is not contiguous (%u records) - address still used",
                      e.name.c_str(), e.records);
        g_resolved++;
    }

    // ProcessEvent slot from the exported UObject vtable.
    uint64_t vt = DynSymAddr("_ZTV7UObject");
    uint64_t pe = Sym::Addr("UObject::ProcessEvent");
    if (vt && pe) {
        // vtable symbol points at the two header words (offset-to-top, typeinfo); slot 0 follows.
        auto* words = (const uint64_t*)(uintptr_t)vt;
        size_t hits = 0;
        if (MemReadable(words, 8 * 256)) {
            for (size_t i = 2; i < 256; i++) {
                if (words[i] == pe) { g_processEventSlot = i - 2; hits++; }
            }
        }
        if (hits != 1) {
            PluginLog("sym: ProcessEvent appears %zu times in _ZTV7UObject (expected exactly 1)", hits);
            if (hits != 1) g_processEventSlot = SIZE_MAX;
        } else {
            PluginLog("sym: ProcessEvent vtable slot = %zu", g_processEventSlot);
        }
    }
    PluginLog("sym: resolved %zu/%zu in %llums (%s)", g_resolved, g_entries.size(),
              (unsigned long long)g_scanMs, g_cacheHit ? "cache" : "scan");
    g_ready = true;
}

bool Sym::Ready() { return g_ready; }

#ifdef TAKARO_DEBUG_CORRUPT_SIG
// Degrade-proof build (DEVELOPMENT.md): this one symbol is treated as unresolved everywhere.
static bool CorruptedSymbol(const char* name) { return name && strcmp(name, TAKARO_DEBUG_CORRUPT_SIG) == 0; }
#else
static bool CorruptedSymbol(const char*) { return false; }
#endif

uint64_t Sym::Addr(const char* name) {
    if (CorruptedSymbol(name)) return 0;
    auto it = g_index.find(name);
    if (it == g_index.end()) return 0;
    return g_entries[it->second].addr;
}

const SymEntry* Sym::Entry(const char* name) {
    if (CorruptedSymbol(name)) return nullptr;
    auto it = g_index.find(name);
    if (it == g_index.end()) return nullptr;
    return &g_entries[it->second];
}

const std::vector<SymEntry>& Sym::All() { return g_entries; }

size_t Sym::ProcessEventSlot() { return g_processEventSlot; }

std::string Sym::StatsJson() {
    return "{\"resolved\":" + std::to_string(g_resolved) + ",\"wanted\":" + std::to_string(g_entries.size()) +
           ",\"processEventSlot\":" + (g_processEventSlot == SIZE_MAX ? "null" : std::to_string(g_processEventSlot)) +
           "}";
}

std::string Sym::CacheJson() {
    return "{\"path\":" + JsonStr(CachePath()) + ",\"symPath\":" + JsonStr(g_symPath) +
           ",\"buildId\":" + JsonStr(Elf().buildId) + ",\"hit\":" + (g_cacheHit ? "true" : "false") +
           ",\"loadMs\":" + std::to_string(g_scanMs) + "}";
}

std::vector<std::pair<std::string, bool>> Sym::SelfChecks() {
    std::vector<std::pair<std::string, bool>> out;
    auto add = [&](const std::string& s, bool ok) { out.push_back({s, ok}); };
    const ElfInfo& e = Elf();
    char b[256];

    bool ok = e.ok;
    snprintf(b, sizeof b, "elf: loadBase=0x%llx text=0x%llx+0x%llx buildId=%s", (unsigned long long)e.loadBase,
             (unsigned long long)e.textAddr, (unsigned long long)e.textSize, e.buildId.c_str());
    add(b, ok);

    uint64_t initA = Sym::Addr("_init"), finiA = Sym::Addr("_fini");
    ok = initA && e.initAddr && initA == e.initAddr;
    snprintf(b, sizeof b, "_init: sym=0x%llx elfSection=0x%llx", (unsigned long long)initA,
             (unsigned long long)e.initAddr);
    add(b, ok);
    ok = finiA && e.finiAddr && finiA == e.finiAddr;
    snprintf(b, sizeof b, "_fini: sym=0x%llx elfSection=0x%llx", (unsigned long long)finiA,
             (unsigned long long)e.finiAddr);
    add(b, ok);

    ok = g_processEventSlot != SIZE_MAX;
    snprintf(b, sizeof b, "processEventSlot: %s in _ZTV7UObject (exactly one match required)",
             ok ? std::to_string(g_processEventSlot).c_str() : "unresolved");
    add(b, ok);

    ok = g_resolved >= 40;
    snprintf(b, sizeof b, "resolvedCount: %zu of %zu wanted (>=40 required for M0)", g_resolved, g_entries.size());
    add(b, ok);
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

// The hot paths - the ProcessEvent detour and every reflected read behind it - call MemReadable
// several times per engine RPC. Taking a mutex and re-reading /proc/self/maps there cost ~3 us per
// ProcessEvent call on the VEIN rig (same code). The authoritative table below is still rebuilt
// under the mutex, but each rebuild *publishes an immutable copy* that readers consult without any
// lock. A snapshot is never freed (a few KB, rebuilt only when the map layout changes), so a reader
// can hold the pointer for as long as it likes.
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
