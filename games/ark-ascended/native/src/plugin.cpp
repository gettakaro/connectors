// AsaApi plugin: the game half of the connector. Everything here that touches a game object runs on the game
// thread, from the tick callback or a hook. The network and JSON work stays in takaro::Client's worker.
#include "API/ARK/Ark.h"

#include "log.h"
#include "logic.h"
#include "takaro_client.h"
#include "transport_winhttp.h"
#include "version.h"

#include <bcrypt.h>
#include <wincrypt.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <thread>
#include <vector>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "crypt32.lib")

namespace fs = std::filesystem;

// FOutputDevice declares this virtual but its body is in the game binary, out of the link's reach. Nothing here calls it.
void FOutputDevice::SerializeRecord(const UE::FLogRecord&) {}

namespace {

constexpr wchar_t kCallbackId[] = L"TakaroAsa";
constexpr auto kTickBudget = std::chrono::milliseconds(2);
constexpr auto kBanSweep = std::chrono::seconds(30);
constexpr auto kSurvivorSweep = std::chrono::seconds(2);
constexpr auto kExitDelay = std::chrono::seconds(4);
constexpr size_t kMaxConsoleOutput = 32768;
constexpr char kGameName[] = "ARK: Survival Ascended";
constexpr char kCalibratedMap[] = "TheIsland_WP";
constexpr double kIslandHalfWidth = 342900;  // measured: world 0 is 50/100 on the in-game map, 100000 is 64.58

struct PluginConfig {
    std::string url = "wss://connect.takaro.io/";
    std::string registrationToken, identityToken, serverName, serverChatName = "Server", caFile;
    bool wireDebug = false;
};

struct Roster {
    std::string name, ip;
    int ping = -1;
    double x = 0, y = 0, z = 0;
    bool hasPosition = false, online = false;
    // The last readings stay while the player has no body, so a death shows up at respawn and not as an error.
    std::optional<takaro::ProgressReading> progress;
    std::optional<tj::Value> inventory;
    std::optional<takaro::Survivor> survivor;
};

struct Icon {
    std::string base64, revision;
    size_t size = 0;
};

std::unique_ptr<native::WinHttpTransport> g_transport;
std::unique_ptr<takaro::Client> g_client;
PluginConfig g_config;
std::atomic<bool> g_gameReady{false};
std::map<std::string, Roster> g_players;  // game thread only
std::unique_ptr<takaro::BanStore> g_bans;
std::unique_ptr<takaro::KnownPlayers> g_known;
std::string g_gameVersion;
std::string g_map;
fs::path g_banListFile;
std::unique_ptr<Icon> g_icon;

// Snapshots taken once when the world is ready, so answering costs the game thread nothing.
std::vector<takaro::CatalogRow> g_items, g_entities;
tj::Value g_itemsJson, g_entitiesJson, g_locationsJson;

std::chrono::steady_clock::time_point g_nextBanSweep{}, g_nextSurvivorSweep{}, g_exitAt{};
bool g_exitPending = false;

std::thread g_tail;
std::atomic<bool> g_tailStop{false};

// Collects what a console command prints.
class ConsoleCapture final : public FOutputDevice {
public:
    void Serialize(const TCHAR* v, ELogVerbosity::Type, const FName&) override { Append(v); }
    void Serialize(const TCHAR* v, ELogVerbosity::Type, const FName&, const double) override { Append(v); }
    void SerializeRecord(const UE::FLogRecord&) override {}
    void Append(const TCHAR* v) {
        if (v == nullptr || text.size() >= kMaxConsoleOutput) return;
        text += v;
        text += L'\n';
    }
    std::wstring text;
};

// Cheat commands answer through the RCON reply, not the output device. Set only while Console runs, on the game thread.
ConsoleCapture* g_capture = nullptr;

std::string Utf8(const FString& s) {
    const wchar_t* w = *s;
    if (w == nullptr || *w == 0) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n > 0 ? n - 1 : 0), '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring Wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring out(static_cast<size_t>(n > 0 ? n - 1 : 0), L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), n);
    return out;
}

std::string RandomHex(size_t bytes) {
    std::string raw(bytes, '\0');
    BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(raw.data()), static_cast<ULONG>(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    static const char* hex = "0123456789abcdef";
    std::string out;
    for (unsigned char c : raw) { out += hex[c >> 4]; out += hex[c & 15]; }
    return out;
}

fs::path PluginDir() {
    wchar_t buffer[MAX_PATH];
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&PluginDir), &self);
    GetModuleFileNameW(self, buffer, MAX_PATH);
    return fs::path(buffer).parent_path();
}

std::string Trim(std::string s) {
    const char* space = " \t\r\n";
    const auto a = s.find_first_not_of(space);
    if (a == std::string::npos) return {};
    return s.substr(a, s.find_last_not_of(space) - a + 1);
}

PluginConfig LoadConfig(const fs::path& dir) {
    PluginConfig c;
    std::ifstream in(dir / "TakaroConfig.txt");
    std::string line;
    while (std::getline(in, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == '#') continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = Trim(line.substr(0, eq)), value = Trim(line.substr(eq + 1));
        if (key == "TAKARO_WS_URL" && !value.empty()) c.url = value;
        else if (key == "TAKARO_REGISTRATION_TOKEN") c.registrationToken = value;
        else if (key == "TAKARO_IDENTITY_TOKEN") c.identityToken = value;
        else if (key == "TAKARO_SERVER_NAME") c.serverName = value;
        else if (key == "TAKARO_SERVER_CHAT_NAME" && !value.empty()) c.serverChatName = value;
        else if (key == "TAKARO_CA_FILE") c.caFile = value;
        else if (key == "TAKARO_WIRE_DEBUG") c.wireDebug = value == "1" || value == "true";
    }
    return c;
}

// The identity token names this server to Takaro and must survive restarts.
std::string LoadIdentity(const fs::path& dir, const std::string& configured) {
    if (!configured.empty()) return configured;
    const fs::path file = dir / "state" / "identity-token";
    std::ifstream in(file);
    std::string token;
    std::getline(in, token);
    token = Trim(token);
    if (!token.empty()) return token;
    token = "ark-ascended-" + RandomHex(16);
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::trunc) << token << "\n";
    return token;
}

std::string EosId(AShooterPlayerController* pc) { return Utf8(AsaApi::IApiUtils::GetEOSIDFromController(pc)); }

// The account name, which stays the same from login on; the character name does not exist until the player spawns.
std::string DisplayName(AShooterPlayerController* pc) {
    std::string name = Utf8(AsaApi::IApiUtils::GetSteamName(pc));
    if (name.empty()) name = Utf8(AsaApi::IApiUtils::GetCharacterName(pc));
    return name;
}

AShooterPlayerController* FindOnline(const std::string& eos) {
    if (eos.empty()) return nullptr;
    const FString id(Wide(eos).c_str());
    return AsaApi::GetApiUtils().FindPlayerFromEOSID(id);
}

tj::Value PositionJson(double x, double y, double z) {
    tj::Value p = tj::obj({{"x", tj::num(x)}, {"y", tj::num(y)}, {"z", tj::num(z)}});
    if (!g_map.empty()) p.set("dimension", tj::str(g_map));
    return p;
}

// Steam ids. The game knows players only by their EOS id, which is different in every EOS product, so it can never
// match a player across games. EOS keeps the Steam account a Steam player logged in with; the server asks for it at
// login and holds player-connected back for at most kSteamWait, so the first event already carries the Steam id.
namespace steamlink {
using EosPuid = void*;
using EosConnect = void*;
struct QueryOptions { int32_t ApiVersion; EosPuid LocalUserId; int32_t AccountIdTypeDeprecated; const EosPuid* ProductUserIds; uint32_t ProductUserIdCount; };
struct MappingOptions { int32_t ApiVersion; EosPuid LocalUserId; int32_t AccountIdType; EosPuid TargetProductUserId; };
struct QueryInfo { int32_t ResultCode; void* ClientData; EosPuid LocalUserId; };
using QueryCallback = void(__cdecl*)(const QueryInfo*);
using FromStringFn = EosPuid(__cdecl*)(const char*);
using QueryFn = void(__cdecl*)(EosConnect, const QueryOptions*, void*, QueryCallback);
using MappingFn = int32_t(__cdecl*)(EosConnect, const MappingOptions*, char*, int32_t*);
struct CopyOptions { int32_t ApiVersion; EosPuid TargetUserId; int32_t AccountIdType; };
struct ExternalAccount { int32_t ApiVersion; EosPuid ProductUserId; const char* DisplayName; const char* AccountId; int32_t AccountIdType; int64_t LastLoginTime; };
using CopyFn = int32_t(__cdecl*)(EosConnect, const CopyOptions*, ExternalAccount**);
using ReleaseFn = void(__cdecl*)(ExternalAccount*);
struct CountOptions { int32_t ApiVersion; EosPuid TargetUserId; };
using CountFn = uint32_t(__cdecl*)(EosConnect, const CountOptions*);
constexpr int32_t kSteam = 1;  // EOS_EAT_STEAM
constexpr auto kSteamWait = std::chrono::seconds(5);

struct Pending { std::string eos; std::chrono::steady_clock::time_point deadline; bool asked = false, done = false; };
std::unordered_map<std::string, std::string> g_ids;  // EOS id -> SteamID64
std::vector<Pending> g_pending;
FromStringFn g_fromString = nullptr;
QueryFn g_query = nullptr;
MappingFn g_mapping = nullptr;
CopyFn g_copy = nullptr;
ReleaseFn g_release = nullptr;
CountFn g_count = nullptr;
bool g_loaded = false, g_warned = false;

bool Load() {
    if (g_loaded) return g_query != nullptr;
    g_loaded = true;
    HMODULE sdk = GetModuleHandleW(L"EOSSDK-Win64-Shipping.dll");
    if (sdk == nullptr) { LogLine("steam: EOS SDK not loaded, players carry the EOS id only"); return false; }
    g_fromString = reinterpret_cast<FromStringFn>(GetProcAddress(sdk, "EOS_ProductUserId_FromString"));
    g_query = reinterpret_cast<QueryFn>(GetProcAddress(sdk, "EOS_Connect_QueryProductUserIdMappings"));
    g_mapping = reinterpret_cast<MappingFn>(GetProcAddress(sdk, "EOS_Connect_GetProductUserIdMapping"));
    g_copy = reinterpret_cast<CopyFn>(GetProcAddress(sdk, "EOS_Connect_CopyProductUserExternalAccountByAccountType"));
    g_release = reinterpret_cast<ReleaseFn>(GetProcAddress(sdk, "EOS_Connect_ExternalAccountInfo_Release"));
    g_count = reinterpret_cast<CountFn>(GetProcAddress(sdk, "EOS_Connect_GetProductUserExternalAccountCount"));
    if (g_fromString == nullptr || g_query == nullptr || g_mapping == nullptr) {
        g_query = nullptr;
        LogLine("steam: EOS Connect functions missing, players carry the EOS id only");
        return false;
    }
    return true;
}

// Reads one pointer, false instead of a crash when the address is not readable.
bool SafeRead(const void* address, void** out) {
    __try {
        *out = *static_cast<void* const*>(address);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The server's EOS Connect handle, read from the game's EOS online subsystem: Subsystem -> IdentityImpl -> EOSConnect.
// The identity points back at its subsystem, which confirms the layout before the handle is used. Found once, then
// cached; every read is guarded, so a layout change costs the Steam id, never the server.
EosConnect g_connect = nullptr;
int g_tries = 0;
EosConnect Connect() {
    if (g_connect != nullptr || g_tries >= 3) return g_connect;
    ++g_tries;
    const auto identityAt = static_cast<ptrdiff_t>(GetAddress(nullptr, "FOnlineSubsystemEOS.IdentityImpl"));
    const auto connectAt = static_cast<ptrdiff_t>(GetAddress(nullptr, "FOnlineIdentityInterfaceEOS.EOSConnect"));
    const auto ownerAt = static_cast<ptrdiff_t>(GetAddress(nullptr, "FOnlineIdentityInterfaceEOS.SubsystemEOS"));
    if (identityAt <= 0 || connectAt <= 0 || ownerAt <= 0) { LogLine("steam: EOS subsystem fields unknown"); return nullptr; }
    auto hex = [](const void* v) { char b[24]; snprintf(b, sizeof b, "%p", v); return std::string(b); };
    for (const char* service : {"RedpointEOS", "EOS"}) {
        FName name(service, FNAME_Find);
        if (name.IsNone()) continue;
        auto* subsystem = static_cast<char*>(NativeCall<void*, FName&>(nullptr, "IOnlineSubsystem.Get(FName&)", name));
        LogLine(std::string("steam: subsystem ") + service + " = " + hex(subsystem));
        if (subsystem == nullptr) continue;
        for (ptrdiff_t back = 0; back <= 0x40; back += 8) {
            char* base = subsystem - back;
            void *identity = nullptr, *owner = nullptr, *connect = nullptr;
            if (!SafeRead(base + identityAt, &identity) || identity == nullptr) continue;
            const bool ro = SafeRead(static_cast<char*>(identity) + ownerAt, &owner);
            const bool rc = SafeRead(static_cast<char*>(identity) + connectAt, &connect);
            LogLine("steam: back=" + std::to_string(back) + " identity=" + hex(identity) + " owner=" + (ro ? hex(owner) : "?") +
                    " connect=" + (rc ? hex(connect) : "?"));
            if (!ro || owner != base || !rc || connect == nullptr) continue;
            g_connect = connect;
            LogLine(std::string("steam: EOS Connect found through ") + service);
            return g_connect;
        }
    }
    if (g_tries >= 3) LogLine("steam: EOS Connect not found, players carry the EOS id only");
    return nullptr;
}

bool IsSteamId64(const std::string& id) {
    return id.size() == 17 && id.rfind("7656119", 0) == 0 && id.find_first_not_of("0123456789") == std::string::npos;
}

void __cdecl OnQueried(const QueryInfo* info) {
    std::unique_ptr<std::string> eos(static_cast<std::string*>(info->ClientData));
    for (auto& p : g_pending) if (p.eos == *eos) p.done = true;
    if (info->ResultCode != 0) { LogLine("steam: lookup failed for " + *eos + " (EOS result " + std::to_string(info->ResultCode) + ")"); return; }
    EosConnect connect = Connect();
    if (connect == nullptr) return;
    EosPuid target = g_fromString(eos->c_str());
    std::string id;
    int32_t result = -1;
    // A game server has no logged-in user, so read the account the query cached; the mapping call needs one.
    if (g_copy != nullptr && g_release != nullptr) {
        CopyOptions copy{1, target, kSteam};
        ExternalAccount* account = nullptr;
        result = g_copy(connect, &copy, &account);
        if (result == 0 && account != nullptr) {
            if (account->AccountId != nullptr) id = account->AccountId;
            g_release(account);
        }
    }
    if (id.empty()) {
        MappingOptions options{1, nullptr, kSteam, target};
        char buffer[129] = {};
        int32_t length = sizeof(buffer);
        const int32_t mapped = g_mapping(connect, &options, buffer, &length);
        if (mapped == 0) id = buffer;
        else LogLine("steam: copy result " + std::to_string(result) + ", mapping result " + std::to_string(mapped));
        result = mapped;
    }
    if (g_count != nullptr) {
        CountOptions count{1, target};
        LogLine("steam: " + *eos + " has " + std::to_string(g_count(connect, &count)) + " linked accounts, Steam id '" + id + "'");
    }
    if (IsSteamId64(id)) {
        g_ids[*eos] = id;
        LogLine("steam: " + *eos + " is Steam " + id);
    } else {
        LogLine("steam: no Steam account for " + *eos + " (EOS result " + std::to_string(result) + ")");
    }
}

void Ask(Pending& p) {
    EosConnect connect = Connect();
    if (connect == nullptr) return;
    EosPuid id = g_fromString(p.eos.c_str());
    QueryOptions options{2, nullptr, 0, &id, 1};  // a game server queries without a local user
    p.asked = true;
    g_query(connect, &options, new std::string(p.eos), OnQueried);
}
}  // namespace steamlink

void FlushConnects(const std::string& only = std::string());

tj::Value PlayerJson(const std::string& eos, const Roster& r) {
    tj::Value p = tj::Value::object();
    p.set("gameId", tj::str(eos));
    p.set("name", tj::str(r.name.empty() ? eos : r.name));
    // An EOS id is issued per EOS product and Takaro keeps one per player, so for a Steam player it would fight the
    // EOS id of every other EOS game. The Steam id names the account everywhere; the EOS id stays the gameId.
    if (const auto steam = steamlink::g_ids.find(eos); steam != steamlink::g_ids.end()) {
        p.set("steamId", tj::str(steam->second));
        p.set("platformId", tj::str("steam:" + steam->second));
    } else {
        p.set("epicOnlineServicesId", tj::str(eos));
    }
    if (!r.ip.empty()) p.set("ip", tj::str(r.ip));
    if (r.ping >= 0) p.set("ping", tj::num(r.ping));
    if (r.survivor) p.set("character", takaro::SurvivorJson(*r.survivor));
    return p;
}

// Null while the player has no player data. MyData is inline, so it needs no check of its own.
FPrimalPlayerDataStruct* PlayerData(AShooterPlayerController* pc) {
    auto* state = static_cast<AShooterPlayerState*>(pc->PlayerStateField().Get());
    if (state == nullptr || state->MyPlayerDataField() == nullptr) return nullptr;
    return state->MyPlayerDataField()->MyDataField();
}

// False from the moment the game swaps the survivor until the sweep has told Takaro. Until then the old Character is
// active, so a reading of the new survivor must not reach Takaro.
bool SurvivorAnnounced(AShooterPlayerController* pc, const Roster& r) {
    FPrimalPlayerDataStruct* data = PlayerData(pc);
    return !r.survivor || data == nullptr || r.survivor->id == std::to_string(data->PlayerDataIDField());
}

// Works without a body, so also on the respawn screen. Only the login and the sweep call this: events go out after
// action responses, so a switch found while answering getPlayers would reach Takaro behind the new survivor's data.
void ReadSurvivor(const std::string& eos, AShooterPlayerController* pc, Roster& r) {
    FPrimalPlayerDataStruct* data = PlayerData(pc);
    if (data == nullptr) return;
    auto* config = GetNativePointerField<void*>(data, "FPrimalPlayerDataStruct.MyPlayerCharacterConfig");
    const std::string name = Trim(Utf8(*GetNativePointerField<FString*>(config, "FPrimalPlayerCharacterConfigStruct.PlayerCharacterName")));
    const auto seen = takaro::MakeSurvivor(data->PlayerDataIDField(), name, r.name);
    if (!seen) return;
    if (!takaro::IsSurvivorSwitch(r.survivor, *seen)) {
        r.survivor = seen;
        return;
    }
    // The game swaps a survivor without a relog, but Takaro only changes a player's active Character on a connect.
    LogLine("new survivor " + seen->id + " eos=" + eos);
    if (r.online) g_client->Emit("player-disconnected", tj::obj({{"player", PlayerJson(eos, r)}}));
    r.survivor = seen;
    r.progress.reset();
    r.inventory.reset();
    if (r.online) g_client->Emit("player-connected", tj::obj({{"player", PlayerJson(eos, r)}}));
}

// Refresh runs on every getPlayers, so getPlayerProgression answers from here without touching the game.
void ReadProgress(const std::string& eos, AShooterCharacter* character, Roster& r) {
    UPrimalCharacterStatusComponent* status = character->MyCharacterStatusComponentField();
    if (status == nullptr) return;
    const auto reading = takaro::MakeProgressReading(status->GetCharacterLevel(), status->ExperiencePointsField(),
                                                     status->GetExperienceRequiredForPreviousLevelUp(),
                                                     status->GetExperienceRequiredForNextLevelUp());
    if (!reading) return;
    if (!r.progress || r.progress->level != reading->level) LogLine("level " + std::to_string(reading->level) + " eos=" + eos);
    r.progress = reading;
}

Roster& Refresh(const std::string& eos, AShooterPlayerController* pc) {
    Roster& r = g_players[eos];
    const std::string name = DisplayName(pc);
    if (!name.empty()) r.name = name;
    const std::string ip = Utf8(AsaApi::IApiUtils::GetIPAddress(pc));
    if (!ip.empty()) r.ip = ip;
    if (auto* state = pc->PlayerStateField().Get()) r.ping = static_cast<int>(std::lround(state->ExactPingField()));
    if (g_known) g_known->Remember(eos, r.name);
    if (auto* character = pc->GetPlayerCharacter(); character != nullptr) {
        const FVector v = AsaApi::IApiUtils::GetPosition(pc);
        if (takaro::IsRealPosition(v.X, v.Y, v.Z)) {
            r.x = v.X; r.y = v.Y; r.z = v.Z;
            r.hasPosition = true;
        }
        if (SurvivorAnnounced(pc, r)) ReadProgress(eos, character, r);
    }
    return r;
}

takaro::Outcome GetPlayers() {
    tj::Value out = tj::Value::array();
    auto& controllers = AsaApi::GetApiUtils().GetWorld()->PlayerControllerListField();
    for (auto& weak : controllers) {
        auto* pc = static_cast<AShooterPlayerController*>(weak.Get());
        if (pc == nullptr) continue;
        const std::string eos = EosId(pc);
        if (eos.empty()) continue;
        out.push(PlayerJson(eos, Refresh(eos, pc)));
    }
    return takaro::Outcome::Ok(std::move(out));
}

takaro::Outcome GetPlayer(const tj::Value& args) {
    const std::string id = takaro::PlayerIdOf(args);
    if (id.empty()) return takaro::Outcome::Fail("invalid_args", "gameId is required");
    if (auto* pc = FindOnline(id)) return takaro::Outcome::Ok(PlayerJson(id, Refresh(id, pc)));
    const auto it = g_players.find(id);
    if (it != g_players.end()) return takaro::Outcome::Ok(PlayerJson(id, it->second));
    Roster offline;
    if (g_known) offline.name = g_known->Name(id);
    return takaro::Outcome::Ok(PlayerJson(id, offline));
}

takaro::Outcome GetProgression(const tj::Value& args) {
    const std::string id = takaro::PlayerIdOf(args);
    const auto it = g_players.find(id);
    if (it != g_players.end()) return takaro::Outcome::Ok(takaro::ProgressionJson(true, it->second.progress));
    return takaro::Outcome::Ok(takaro::ProgressionJson(g_known && !id.empty() && !g_known->Name(id).empty(), std::nullopt));
}

takaro::Outcome GetLocation(const tj::Value& args) {
    const std::string id = takaro::PlayerIdOf(args);
    if (id.empty()) return takaro::Outcome::Fail("invalid_args", "gameId is required");
    if (auto* pc = FindOnline(id)) Refresh(id, pc);
    const auto it = g_players.find(id);
    if (it == g_players.end() || !it->second.hasPosition) return takaro::Outcome::Fail("not_found", "no known position");
    return takaro::Outcome::Ok(PositionJson(it->second.x, it->second.y, it->second.z));
}

takaro::Outcome SendMessage(const tj::Value& args) {
    const tj::Value* message = args.get("message");
    if (message == nullptr || !message->is_string() || message->as_string().empty())
        return takaro::Outcome::Fail("invalid_args", "message is required");
    static const tj::Value none = tj::Value::object();
    const tj::Value* optsPtr = args.get("opts");
    const tj::Value& opts = optsPtr != nullptr && optsPtr->is_object() ? *optsPtr : none;
    std::string sender = g_config.serverChatName;
    if (const tj::Value* o = opts.get("senderNameOverride"); o != nullptr && o->is_string() && !o->as_string().empty())
        sender = o->as_string();
    const FString senderName(Wide(sender).c_str());
    const std::wstring text = Wide(message->as_string());
    std::string recipient;
    if (const tj::Value* r = opts.get("recipient"); r != nullptr) {
        recipient = takaro::PlayerIdOf(*r);
        if (recipient.empty()) return takaro::Outcome::Fail("invalid_args", "recipient has no gameId");
    }
    if (recipient.empty()) {
        AsaApi::GetApiUtils().SendChatMessageToAll(senderName, L"{}", text.c_str());
    } else {
        // A directed message never falls back to a broadcast.
        auto* pc = FindOnline(recipient);
        if (pc == nullptr) return takaro::Outcome::Fail("not_found", "recipient is not online");
        AsaApi::GetApiUtils().SendChatMessage(pc, senderName, L"{}", text.c_str());
    }
    return takaro::Outcome::Ok(tj::Value::object());
}

// The game declares these parameters as FString by value, which on x64 means a pointer to memory the callee
// destroys. Handing it a heap copy that is never destroyed here avoids a double free whichever way it behaves.
FString* CalleeOwned(const std::string& text) { return new FString(Wide(text).c_str()); }

bool NumberOf(const tj::Value* v, double limit, double& out) {
    if (v == nullptr) return false;
    if (v->is_number()) out = v->as_number();
    else if (v->is_string()) {
        char* end = nullptr;
        out = std::strtod(v->as_string().c_str(), &end);
        if (end == v->as_string().c_str() || *end != 0) return false;
    } else return false;
    return std::isfinite(out) && std::fabs(out) <= limit;
}

takaro::Outcome Console(const tj::Value& args) {
    const tj::Value* raw = args.get("command");
    if (raw == nullptr || !raw->is_string()) return takaro::Outcome::Fail("invalid_args", "command is required");
    const auto reply = [](bool ok, const std::string& output, const std::string& error) {
        tj::Value out = tj::obj({{"success", tj::Value::boolean(ok)}, {"rawResult", tj::str(output)}});
        out.set("errorMessage", error.empty() ? tj::Value::null() : tj::str(error));
        return takaro::Outcome::Ok(std::move(out));
    };
    const std::string refusal = takaro::ConsoleRefusal(raw->as_string());
    if (!refusal.empty()) return reply(false, "", refusal);

    std::string text = Trim(raw->as_string());
    for (const char* prefix : {"admincheat ", "cheat "}) {
        if (_strnicmp(text.c_str(), prefix, strlen(prefix)) == 0) { text = Trim(text.substr(strlen(prefix))); break; }
    }
    const std::wstring command = Wide(text);
    // UEngine::Exec is not an option here: it writes through a null pointer on a dedicated server.
    auto* cheats = AsaApi::GetApiUtils().GetCheatManager();
    if (cheats == nullptr) return reply(false, "", "Console commands need RCONEnabled=True in GameUserSettings.ini");
    ConsoleCapture capture;
    g_capture = &capture;
    const bool handled = cheats->ProcessConsoleExec(command.c_str(), &capture, nullptr);
    g_capture = nullptr;
    const FString output(capture.text.c_str());
    return reply(handled, Utf8(output), handled ? "" : "The game did not recognise this command");
}

takaro::Outcome Kick(const tj::Value& args) {
    const std::string id = takaro::PlayerIdOf(args);
    if (id.empty()) return takaro::Outcome::Fail("invalid_args", "gameId is required");
    auto* pc = FindOnline(id);
    if (pc == nullptr) return takaro::Outcome::Fail("not_found", "player is not online");
    std::string reason = "Kicked by an admin";
    if (const tj::Value* r = args.get("reason"); r != nullptr && r->is_string() && !Trim(r->as_string()).empty()) reason = r->as_string();
    const FString message(Wide(reason).c_str());
    AsaApi::GetApiUtils().GetShooterGameMode()->KickPlayerController(pc, message);
    return takaro::Outcome::Ok(tj::Value::object());
}

takaro::Outcome BanPlayer(const tj::Value& args) {
    const std::string id = takaro::PlayerIdOf(args);
    if (id.empty()) return takaro::Outcome::Fail("invalid_args", "gameId is required");
    takaro::Ban ban;
    if (const tj::Value* r = args.get("reason"); r != nullptr && r->is_string()) ban.reason = r->as_string();
    if (const tj::Value* e = args.get("expiresAt"); e != nullptr) {
        if (!e->is_string() || !takaro::ParseIso8601(e->as_string(), ban.expiresEpoch))
            return takaro::Outcome::Fail("invalid_args", "expiresAt must be an ISO 8601 time");
        ban.expiresAt = e->as_string();
    }
    auto* mode = AsaApi::GetApiUtils().GetShooterGameMode();
    // A timed ban is lifted by the sweep below, so the game holds it as permanent and one clock decides.
    // The record comes first: without it a timed ban would never end.
    if (!g_bans->Set(id, std::move(ban))) return takaro::Outcome::Fail("internal", "the ban could not be saved");
    if (!mode->BanPlayer(CalleeOwned(id), 0)) {
        g_bans->Erase(id);
        return takaro::Outcome::Fail("game_error", "the game refused the ban");
    }
    mode->SaveBannedList();
    if (auto* pc = FindOnline(id)) mode->KickPlayerController(pc, FString(L"Banned"));
    return takaro::Outcome::Ok(tj::Value::object());
}

std::vector<takaro::NativeBan> NativeBans() {
    std::ifstream in(g_banListFile, std::ios::binary);
    return takaro::ParseBanList(std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()));
}

// True once the game no longer holds the ban. Its own return value says false for a player it never had banned,
// so the saved list decides. The record stays until then, so a timed ban is tried again on the next sweep.
bool LiftBan(const std::string& id) {
    auto* mode = AsaApi::GetApiUtils().GetShooterGameMode();
    mode->UnbanPlayer(CalleeOwned(id));
    mode->SaveBannedList();
    for (const auto& ban : NativeBans())
        if (ban.id == id) return false;
    g_bans->Erase(id);
    return true;
}

takaro::Outcome UnbanPlayer(const tj::Value& args) {
    const std::string id = takaro::PlayerIdOf(args);
    if (id.empty()) return takaro::Outcome::Fail("invalid_args", "gameId is required");
    if (!LiftBan(id)) return takaro::Outcome::Fail("game_error", "the game still lists this ban");
    return takaro::Outcome::Ok(tj::Value::object());
}

bool IsBanned(const std::string& id) {
    if (g_bans->Rows().count(id) > 0) return true;
    for (const auto& ban : NativeBans())
        if (ban.id == id) return true;
    return false;
}

takaro::Outcome ListBans() {
    tj::Value out = tj::Value::array();
    const auto rows = g_bans->Rows();
    // Bans an admin made in the game are reported too; only their reason and end are unknown.
    for (const auto& native : NativeBans()) {
        if (rows.count(native.id) > 0) continue;
        Roster who;
        who.name = native.name;
        tj::Value row = tj::obj({{"player", PlayerJson(native.id, who)}, {"reason", tj::str("")}});
        row.set("expiresAt", tj::Value::null());
        out.push(std::move(row));
    }
    for (const auto& [id, ban] : rows) {
        Roster who;
        who.name = g_known->Name(id);
        tj::Value row = tj::obj({{"player", PlayerJson(id, who)}, {"reason", tj::str(ban.reason)}});
        row.set("expiresAt", ban.expiresAt.empty() ? tj::Value::null() : tj::str(ban.expiresAt));
        out.push(std::move(row));
    }
    return takaro::Outcome::Ok(std::move(out));
}

takaro::Outcome Teleport(const tj::Value& args) {
    const std::string id = takaro::PlayerIdOf(args);
    if (id.empty()) return takaro::Outcome::Fail("invalid_args", "gameId is required");
    double x, y, z;
    if (!NumberOf(args.get("x"), 1e8, x) || !NumberOf(args.get("y"), 1e8, y) || !NumberOf(args.get("z"), 1e8, z))
        return takaro::Outcome::Fail("invalid_args", "x, y and z must be numbers");
    if (const tj::Value* d = args.get("dimension"); d != nullptr && !(d->is_string() && d->as_string() == g_map))
        return takaro::Outcome::Fail("invalid_args", "unknown dimension");
    auto* pc = FindOnline(id);
    if (pc == nullptr) return takaro::Outcome::Fail("not_found", "player is not online");
    if (!AsaApi::IApiUtils::TeleportToPos(pc, FVector(x, y, z)))
        return takaro::Outcome::Fail("game_error", "the player cannot be moved right now");
    const FVector now = AsaApi::IApiUtils::GetPosition(pc);
    if (std::hypot(now.X - x, now.Y - y) > 500.0) return takaro::Outcome::Fail("game_error", "the teleport did not take effect");
    return takaro::Outcome::Ok(tj::Value::object());
}

takaro::Outcome Inventory(const tj::Value& args) {
    const std::string id = takaro::PlayerIdOf(args);
    if (id.empty()) return takaro::Outcome::Fail("invalid_args", "gameId is required");
    auto* pc = FindOnline(id);
    if (pc == nullptr) return takaro::Outcome::Fail("not_found", "player is not online");
    auto* character = pc->GetPlayerCharacter();
    UPrimalInventoryComponent* inventory = character != nullptr ? character->MyInventoryComponentField() : nullptr;
    const auto it = g_players.find(id);
    if (inventory == nullptr || (it != g_players.end() && !SurvivorAnnounced(pc, it->second))) {
        if (it != g_players.end() && it->second.inventory) return takaro::Outcome::Ok(*it->second.inventory);
        return takaro::Outcome::Fail("game_error", "the player has no inventory right now");
    }
    tj::Value out = tj::Value::array();
    std::set<UPrimalItem*> seen;
    const auto add = [&](UPrimalItem* item) {
        if (item == nullptr || !seen.insert(item).second) return;
        // Skins sit in the same list but the game shows them on the Cosmetics tab, not as things the player carries.
        const BitFieldValue<bool, unsigned __int32> cosmetic{item, "UPrimalItem.bIsCharacterSkinOrCosmetic"};
        if (item->bIsEngram()() || item->bHideFromInventoryDisplay()() || item->bIsItemSkin()() || cosmetic()) return;
        const std::string code = Utf8(AsaApi::IApiUtils::GetItemBlueprint(item));
        if (takaro::IsCosmeticCode(code)) return;
        std::string name = Utf8(item->DescriptiveNameBaseField());
        if (name.empty()) {
            FString full;
            item->GetItemName(&full, false, false, nullptr);
            name = Utf8(full);
        }
        const int amount = item->GetItemQuantity();
        if (code.empty() || name.empty() || amount < 1) return;
        out.push(tj::obj({{"code", tj::str(code)}, {"name", tj::str(name)}, {"amount", tj::num(amount)}}));
    };
    for (UPrimalItem* item : inventory->InventoryItemsField()) add(item);
    for (UPrimalItem* item : inventory->EquippedItemsField()) add(item);
    g_players[id].inventory = out;
    return takaro::Outcome::Ok(std::move(out));
}

takaro::Outcome GiveItem(const tj::Value& args) {
    const std::string id = takaro::PlayerIdOf(args);
    if (id.empty()) return takaro::Outcome::Fail("invalid_args", "gameId is required");
    std::string code = takaro::ItemCodeOf(args);
    if (code.empty()) {
        size_t matches = 0;
        code = takaro::ItemCodeByName(args, g_items, matches);
        if (matches > 1) return takaro::Outcome::Fail("invalid_args", "more than one item has this name; give the item code from listItems");
        if (code.empty()) return takaro::Outcome::Fail("invalid_args", "item must be an item code or name from listItems");
    }
    double amount = 1, quality = 0;
    const tj::Value* amountRaw = args.get("amount");
    if (amountRaw == nullptr) amountRaw = args.get("quantity");
    if (amountRaw != nullptr && (!NumberOf(amountRaw, 10000, amount) || amount < 1 || amount != std::floor(amount)))
        return takaro::Outcome::Fail("invalid_args", "amount must be a whole number from 1 to 10000");
    if (const tj::Value* q = args.get("quality"); q != nullptr && (!NumberOf(q, 100000, quality) || quality < 0))
        return takaro::Outcome::Fail("invalid_args", "quality must be a nonnegative number");
    bool blueprint = false;
    if (const tj::Value* b = args.get("blueprint"); b != nullptr) {
        if (!b->is_bool()) return takaro::Outcome::Fail("invalid_args", "blueprint must be true or false");
        blueprint = b->as_bool();
    }
    auto* pc = FindOnline(id);
    if (pc == nullptr) return takaro::Outcome::Fail("not_found", "player is not online");
    if (pc->GetPlayerCharacter() == nullptr) return takaro::Outcome::Fail("game_error", "the player has no character right now");
    const FString path(Wide(code).c_str());
    TArray<UPrimalItem*, TSizedDefaultAllocator<32>> given;
    const bool ok = pc->GiveItem(&given, &path, static_cast<int>(amount), static_cast<float>(quality), blueprint, false, 0.0f);
    if (!ok || given.Num() == 0) return takaro::Outcome::Fail("game_error", "the game did not grant this item");
    return takaro::Outcome::Ok(tj::Value::object());
}

takaro::Outcome Catalog(const tj::Value& args, const std::vector<takaro::CatalogRow>& rows, const tj::Value& all) {
    if (rows.empty()) return takaro::Outcome::Fail("game_error", "the catalogue is not loaded yet");
    if (const tj::Value* s = args.get("search"); s != nullptr && s->is_string() && !Trim(s->as_string()).empty())
        return takaro::Outcome::Ok(takaro::CatalogSearch(rows, s->as_string()));
    return takaro::Outcome::Ok(all);
}

takaro::Outcome Shutdown() {
    if (AsaApi::GetApiUtils().GetCheatManager() == nullptr)
        return takaro::Outcome::Fail("game_error", "shutdown needs RCONEnabled=True in GameUserSettings.ini");
    // No save here: DoExit saves by itself, and a second save just before it left a corrupt world file.
    // The game runs no Logout for players still online when it exits.
    for (auto& [id, r] : g_players) {
        if (!r.online) continue;
        r.online = false;
        g_client->Emit("player-disconnected", tj::obj({{"player", PlayerJson(id, r)}}));
    }
    g_exitAt = std::chrono::steady_clock::now() + kExitDelay;
    g_exitPending = true;
    return takaro::Outcome::Ok(tj::Value::object());
}

takaro::Outcome Run(const takaro::GameCall& call) {
    if (call.action == "getPlayers") return GetPlayers();
    if (call.action == "getPlayer") return GetPlayer(call.args);
    if (call.action == "getPlayerLocation") return GetLocation(call.args);
    if (call.action == "getPlayerProgression") return GetProgression(call.args);
    if (call.action == "sendMessage") return SendMessage(call.args);
    if (call.action == "executeConsoleCommand") return Console(call.args);
    if (call.action == "kickPlayer") return Kick(call.args);
    if (call.action == "banPlayer") return BanPlayer(call.args);
    if (call.action == "unbanPlayer") return UnbanPlayer(call.args);
    if (call.action == "listBans") return ListBans();
    if (call.action == "teleportPlayer") return Teleport(call.args);
    if (call.action == "getPlayerInventory") return Inventory(call.args);
    if (call.action == "giveItem") return GiveItem(call.args);
    if (call.action == "listItems") return Catalog(call.args, g_items, g_itemsJson);
    if (call.action == "listEntities") return Catalog(call.args, g_entities, g_entitiesJson);
    if (call.action == "listLocations") return takaro::Outcome::Ok(g_locationsJson);
    if (call.action == "shutdown") return Shutdown();
    return takaro::Outcome::Fail("unsupported", "action '" + call.action.substr(0, 64) + "' is unavailable on this build");
}

void BuildItems() {
    using ItemClasses = TArray<TSubclassOf<UPrimalItem>, TSizedDefaultAllocator<32>>;
    auto* data = AsaApi::GetApiUtils().GetGameData();
    auto& classes = *GetNativePointerField<ItemClasses*>(data, "UPrimalGameData.MasterItemList");
    std::set<std::string> seen;
    for (auto& item : classes) {
        UClass* cls = item.uClass;
        if (cls == nullptr) continue;
        auto* defaults = static_cast<UPrimalItem*>(cls->GetDefaultObject(true));
        if (defaults == nullptr) continue;
        const std::string code = Utf8(AsaApi::IApiUtils::GetClassBlueprint(cls));
        const std::string name = Trim(Utf8(defaults->DescriptiveNameBaseField()));
        if (!code.empty() && !name.empty() && seen.insert(code).second) g_items.push_back({code, name});
    }
    g_itemsJson = takaro::CatalogJson(g_items);
}

void BuildEntities() {
    auto* data = AsaApi::GetApiUtils().GetGameData();
    std::set<std::string> seen;
    for (auto* entry : data->DinoEntriesObjectsField()) {
        if (entry == nullptr) continue;
        const FName& tag = *GetNativePointerField<FName*>(entry, "UPrimalDinoEntry.DinoNameTag");
        const std::string code = Utf8(tag.ToString());
        const std::string name = Trim(Utf8(*GetNativePointerField<FString*>(entry, "UPrimalDinoEntry.DinoDescriptiveName")));
        if (!code.empty() && code != "None" && !name.empty() && seen.insert(code).second) g_entities.push_back({code, name});
    }
    g_entitiesJson = takaro::CatalogJson(g_entities);
}

void BuildLocations() {
    TArray<AActor*, TSizedDefaultAllocator<32>> starts;
    UVictoryCore::GetAllPlayerStarts(&starts, AsaApi::GetApiUtils().GetWorld());
    tj::Value out = tj::Value::array();
    int index = 0;
    for (AActor* start : starts) {
        if (start == nullptr) continue;
        const FVector at = start->GetLocation();
        ++index;
        out.push(tj::obj({{"code", tj::str("spawn-" + std::to_string(index))}, {"name", tj::str("Spawn point " + std::to_string(index))},
                          {"position", PositionJson(at.X, at.Y, at.Z)},
                          {"sizeX", tj::num(1000)}, {"sizeY", tj::num(1000)}, {"sizeZ", tj::num(1000)}}));
    }
    g_locationsJson = std::move(out);
}

// Runs once, before anyone can be on the server, so this is the one place a long game-thread pass is free.
void OnWorldReady() {
    const auto started = std::chrono::steady_clock::now();
    try {
        BuildItems();
        BuildEntities();
        BuildLocations();
    } catch (const std::exception& e) {
        LogLine(std::string("snapshot failed: ") + e.what());
    }
    const auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    LogLine("snapshots: items=" + std::to_string(g_items.size()) + " entities=" + std::to_string(g_entities.size()) +
            " locations=" + std::to_string(g_locationsJson.as_array().size()) + " in " + std::to_string(spent) + " ms");
    LogLine(std::string("console: rcon cheat manager ") + (AsaApi::GetApiUtils().GetCheatManager() != nullptr ? "present" : "absent"));
}

void SweepBans() {
    const auto now = std::chrono::steady_clock::now();
    if (now < g_nextBanSweep) return;
    g_nextBanSweep = now + kBanSweep;
    for (const std::string& id : g_bans->Expired(std::time(nullptr))) {
        LogLine(std::string(LiftBan(id) ? "timed ban expired: " : "timed ban could not be lifted yet: ") + id);
    }
}

// Measures what the connector costs the game thread. Logged only with TAKARO_WIRE_DEBUG.
struct TickCost {
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    ~TickCost() {
        static long long total = 0, worst = 0, ticks = 0;
        static auto reported = start;
        const auto now = std::chrono::steady_clock::now();
        const long long spent = std::chrono::duration_cast<std::chrono::microseconds>(now - start).count();
        total += spent; ++ticks;
        if (spent > worst) worst = spent;
        if (g_config.wireDebug && now - reported >= std::chrono::minutes(1)) {
            LogLine("tick cost: avg " + std::to_string(total / ticks) + " us, worst " + std::to_string(worst) + " us over " + std::to_string(ticks) + " ticks");
            total = worst = ticks = 0;
            reported = now;
        }
    }
};

void SweepSurvivors() {
    const auto now = std::chrono::steady_clock::now();
    if (now < g_nextSurvivorSweep) return;
    g_nextSurvivorSweep = now + kSurvivorSweep;
    for (auto& weak : AsaApi::GetApiUtils().GetWorld()->PlayerControllerListField()) {
        auto* pc = static_cast<AShooterPlayerController*>(weak.Get());
        if (pc == nullptr) continue;
        const auto it = g_players.find(EosId(pc));
        if (it != g_players.end() && it->second.online) ReadSurvivor(it->first, pc, it->second);
    }
}

void OnTick(float) {
    if (!g_client) return;
    TickCost cost;
    if (!g_gameReady.load() && AsaApi::GetApiUtils().GetStatus() == AsaApi::ServerStatus::Ready) {
        OnWorldReady();
        g_gameReady = true;
        LogLine("game ready");
        // Takaro reads the catalogues only in the first seconds after a server registers, so connecting while the
        // world loads would leave them empty for up to an hour.
        if (!g_client->Start()) LogLine("transport failed to start");
    }
    if (!g_gameReady.load()) return;
    if (g_exitPending && std::chrono::steady_clock::now() >= g_exitAt) {
        g_exitPending = false;
        LogLine("exiting on request");
        AsaApi::GetApiUtils().GetCheatManager()->DoExit();
        return;
    }
    if (!steamlink::g_pending.empty()) FlushConnects();
    SweepBans();
    SweepSurvivors();
    const auto stop = std::chrono::steady_clock::now() + kTickBudget;
    takaro::GameCall call;
    while (std::chrono::steady_clock::now() < stop && g_client->NextCall(call)) {
        takaro::Outcome outcome;
        try {
            outcome = Run(call);
        } catch (const std::exception& e) {
            LogLine(std::string("action threw: ") + e.what());
            outcome = takaro::Outcome::Fail("internal", "internal connector error");
        }
        g_client->Complete(call, std::move(outcome));
    }
}

bool OnChat(AShooterPlayerController* pc, FString* message, int mode, int, bool spam, bool command) {
    if (!g_client || pc == nullptr || message == nullptr || spam || command) return false;
    const std::string eos = EosId(pc);
    if (eos.empty()) return false;
    const Roster& r = Refresh(eos, pc);
    const char* channel = mode == 1 ? "team" : "global";
    g_client->Emit("chat-message", tj::obj({{"player", PlayerJson(eos, r)}, {"channel", tj::str(channel)}, {"msg", tj::str(Utf8(*message))}}));
    return false;
}

// Sends the held-back player-connected once the Steam lookup answered or timed out; `force` sends it now (logout).
void FlushConnects(const std::string& only) {
    const auto now = std::chrono::steady_clock::now();
    for (auto it = steamlink::g_pending.begin(); it != steamlink::g_pending.end();) {
        const bool forced = !only.empty() && it->eos == only;
        if (!forced && !it->done && now < it->deadline) {
            if (!it->asked) steamlink::Ask(*it);
            ++it;
            continue;
        }
        if (!it->done) LogLine("steam: no answer for " + it->eos + ", connecting without a Steam id");
        if (const auto r = g_players.find(it->eos); r != g_players.end() && r->second.online)
            g_client->Emit("player-connected", tj::obj({{"player", PlayerJson(it->eos, r->second)}}));
        it = steamlink::g_pending.erase(it);
    }
}


DECLARE_HOOK(AShooterGameMode_HandleNewPlayer_Implementation, bool, AShooterGameMode*, AShooterPlayerController*, UPrimalPlayerData*, AShooterCharacter*, bool);
bool Hook_HandleNewPlayer(AShooterGameMode* mode, AShooterPlayerController* pc, UPrimalPlayerData* data, AShooterCharacter* character, bool fromLogin)
{
    const bool result = AShooterGameMode_HandleNewPlayer_Implementation_original(mode, pc, data, character, fromLogin);
    if (g_client && pc != nullptr) {
        const std::string eos = EosId(pc);
        LogLine("login eos=" + eos + " fromLogin=" + std::to_string(fromLogin));
        // A banned player reaches this hook before the game turns them away; they never really joined.
        if (!eos.empty() && !IsBanned(eos)) {
            Roster& r = Refresh(eos, pc);
            ReadSurvivor(eos, pc, r);
            if (!r.online) {
                r.online = true;
                if (steamlink::g_ids.count(eos) != 0 || !steamlink::Load()) {
                    g_client->Emit("player-connected", tj::obj({{"player", PlayerJson(eos, r)}}));
                } else {
                    steamlink::g_pending.push_back({eos, std::chrono::steady_clock::now() + steamlink::kSteamWait});
                }
            }
        }
    }
    return result;
}

AShooterPlayerController* AsPlayer(AController* controller) {
    if (controller == nullptr || !controller->IsA(AShooterPlayerController::GetPrivateStaticClass())) return nullptr;
    return static_cast<AShooterPlayerController*>(controller);
}

DECLARE_HOOK(UShooterCheatManager_PrintMessageOut, void, UShooterCheatManager*, const FString*);
void Hook_PrintMessageOut(UShooterCheatManager* cheats, const FString* message)
{
    if (g_capture != nullptr && message != nullptr) g_capture->Append(**message);
    UShooterCheatManager_PrintMessageOut_original(cheats, message);
}

// A static function: the message is the only argument.
DECLARE_HOOK(URCONServer_SetClientMessage, void, FString*);
void Hook_SetClientMessage(FString* message)
{
    if (g_capture != nullptr && message != nullptr) g_capture->Append(**message);
    URCONServer_SetClientMessage_original(message);
}

// Admin chat: ServerChat from an admin's console or over RCON. It has no player; player chat goes through OnChat. The
// connector's own sendMessage uses a different call, so this never echoes Takaro's messages back.
DECLARE_HOOK(UShooterCheatManager_ServerChat, void, UShooterCheatManager*, FString*);
void Hook_ServerChat(UShooterCheatManager* cheats, FString* message)
{
    const std::string text = message != nullptr ? Utf8(*message) : std::string();
    UShooterCheatManager_ServerChat_original(cheats, message);
    if (g_client && !Trim(text).empty()) g_client->Emit("chat-message", tj::obj({{"channel", tj::str("global")}, {"msg", tj::str(text)}}));
}

DECLARE_HOOK(AShooterGameState_NotifyPlayerDied, void, AShooterGameState*, AShooterCharacter*, AShooterPlayerController*, APawn*, AActor*);
void Hook_NotifyPlayerDied(AShooterGameState* state, AShooterCharacter* character, AShooterPlayerController* pc, APawn* instigator, AActor* causer)
{
    if (g_client && pc != nullptr && character != nullptr) {
        const std::string eos = EosId(pc);
        if (!eos.empty()) {
            Roster& r = g_players[eos];
            const FVector at = character->GetLocation();
            r.x = at.X; r.y = at.Y; r.z = at.Z;
            r.hasPosition = true;
            tj::Value data = tj::obj({{"player", PlayerJson(eos, r)}, {"position", PositionJson(r.x, r.y, r.z)}});
            if (instigator != nullptr) {
                if (auto* attacker = AsPlayer(instigator->ControllerField().Get()); attacker != nullptr && attacker != pc) {
                    const std::string attackerId = EosId(attacker);
                    if (!attackerId.empty()) data.set("attacker", PlayerJson(attackerId, Refresh(attackerId, attacker)));
                }
            }
            g_client->Emit("player-death", data);
        }
    }
    AShooterGameState_NotifyPlayerDied_original(state, character, pc, instigator, causer);
}

DECLARE_HOOK(APrimalDinoCharacter_Die, bool, APrimalDinoCharacter*, float, const FDamageEvent*, AController*, AActor*);
bool Hook_DinoDie(APrimalDinoCharacter* dino, float damage, const FDamageEvent* event, AController* killer, AActor* causer)
{
    tj::Value data;
    if (g_client && dino != nullptr) {
        if (auto* pc = AsPlayer(killer)) {
            const std::string eos = EosId(pc);
            const std::string entity = Utf8(dino->DinoNameTagField().ToString());
            if (!eos.empty() && !entity.empty()) {
                std::string weapon;
                if (auto* character = pc->GetPlayerCharacter()) {
                    if (auto* held = character->GetCurrentWeapon()) {
                        if (auto* item = held->AssociatedPrimalItemField()) weapon = Utf8(item->DescriptiveNameBaseField());
                    }
                }
                data = tj::obj({{"player", PlayerJson(eos, Refresh(eos, pc))}, {"entity", tj::str(entity)}, {"weapon", tj::str(weapon)}});
            }
        }
    }
    const bool died = APrimalDinoCharacter_Die_original(dino, damage, event, killer, causer);
    if (died && data.is_object()) g_client->Emit("entity-killed", data);
    return died;
}

DECLARE_HOOK(AShooterGameMode_Logout, void, AShooterGameMode*, AController*);
void Hook_Logout(AShooterGameMode* mode, AController* controller)
{
    if (g_client && controller != nullptr) {
        auto* pc = static_cast<AShooterPlayerController*>(controller);
        const std::string eos = EosId(pc);
        if (!eos.empty()) {
            FlushConnects(eos);
            Roster& r = Refresh(eos, pc);
            if (r.online) {
                r.online = false;
                g_client->Emit("player-disconnected", tj::obj({{"player", PlayerJson(eos, r)}}));
            }
        }
    }
    AShooterGameMode_Logout_original(mode, controller);
}

std::vector<tj::Value> Manifest() {
    std::vector<tj::Value> out;
    for (const char* name : {"players.list", "players.get", "players.location", "players.inventory", "players.teleport",
                             "items.list", "items.give", "entities.list", "locations.list", "chat.send", "console.execute",
                             "moderation.kick", "moderation.ban", "moderation.unban", "moderation.listBans", "server.shutdown",
                             "events.player-connected", "events.player-disconnected", "events.chat-message",
                             "events.player-death", "events.entity-killed", "events.log"})
        out.push_back(tj::obj({{"name", tj::str(name)}}));
    out.push_back(tj::obj({{"name", tj::str("players.progression")}, {"parameters", tj::obj({{"source", tj::str("server")}})}}));
    // Survivors live in the server's save. A death does not end one, so `ends` stays out.
    out.push_back(tj::obj({{"name", tj::str("players.characters")}, {"parameters", tj::obj({{"held", tj::str("server")}})}}));
    tj::Value profile = tj::obj({{"name", tj::str(kGameName)}});
    if (!g_gameVersion.empty()) profile.set("version", tj::str(g_gameVersion));
    if (g_icon) profile.set("icon", tj::obj({{"revision", tj::str(g_icon->revision)}, {"size", tj::num(static_cast<double>(g_icon->size))}}));
    out.push_back(tj::obj({{"name", tj::str("game.profile")}, {"parameters", std::move(profile)}}));
    if (g_map == kCalibratedMap) {
        const auto range = [] { return tj::obj({{"min", tj::num(-kIslandHalfWidth)}, {"max", tj::num(kIslandHalfWidth)}}); };
        tj::Value dimension = tj::obj({
            {"id", tj::str(g_map)}, {"name", tj::str("The Island")},
            {"bounds", tj::obj({{"horizontal", range()}, {"vertical", range()}})},
            {"axes", tj::obj({{"horizontal", tj::obj({{"field", tj::str("x")}, {"direction", tj::str("right")}})},
                              {"vertical", tj::obj({{"field", tj::str("y")}, {"direction", tj::str("down")}})}})},
            {"source", tj::obj({{"type", tj::str("none")}})}});
        tj::Value dimensions = tj::Value::array();
        dimensions.push(std::move(dimension));
        out.push_back(tj::obj({{"name", tj::str("map")}, {"parameters", tj::obj({{"dimensions", std::move(dimensions)}})}}));
    }
    return out;
}

tj::Value Reachability() {
    tj::Value out = tj::Value::object();
    const bool ready = g_gameReady.load();
    out.set("connectable", tj::Value::boolean(ready));
    if (ready) out.set("reason", tj::Value::null());
    else out.set("reason", tj::str("The game server is still starting"));
    return out;
}

bool LocalAction(const std::string& action, takaro::Outcome& out) {
    if (action != "getGameIcon") return false;
    if (g_icon) out = takaro::Outcome::Ok(tj::obj({{"revision", tj::str(g_icon->revision)}, {"data", tj::str(g_icon->base64)}}));
    else out = takaro::Outcome::Fail("unsupported", "no icon is shipped with this build");
    return true;
}

std::unique_ptr<Icon> LoadIcon(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    const std::string png((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (png.empty() || png.size() > 128 * 1024) return nullptr;
    unsigned char digest[32];
    if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char*>(png.data())),
                   static_cast<ULONG>(png.size()), digest, sizeof digest) != 0) return nullptr;
    static const char* hex = "0123456789abcdef";
    auto icon = std::make_unique<Icon>();
    for (int i = 0; i < 6; ++i) { icon->revision += hex[digest[i] >> 4]; icon->revision += hex[digest[i] & 15]; }
    DWORD length = 0;
    const DWORD flags = CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF;
    CryptBinaryToStringA(reinterpret_cast<const BYTE*>(png.data()), static_cast<DWORD>(png.size()), flags, nullptr, &length);
    icon->base64.resize(length);
    CryptBinaryToStringA(reinterpret_cast<const BYTE*>(png.data()), static_cast<DWORD>(png.size()), flags, icon->base64.data(), &length);
    icon->base64.resize(length);
    icon->size = png.size();
    return icon;
}

// The log prints the human version early in boot; a server that has not printed it yet still has its Steam build.
std::string ReadGameVersion(const fs::path& gameRoot) {
    std::ifstream log(gameRoot / "ShooterGame" / "Saved" / "Logs" / "ShooterGame.log");
    std::string line;
    for (int i = 0; i < 2000 && std::getline(log, line); ++i) {
        const auto at = line.find("ARK Version: ");
        if (at != std::string::npos) return Trim(line.substr(at + 13));
    }
    std::ifstream manifest(gameRoot / "steamapps" / "appmanifest_2430930.acf");
    while (std::getline(manifest, line)) {
        const auto at = line.find("\"buildid\"");
        if (at == std::string::npos) continue;
        const auto end = line.rfind('"'), start = line.rfind('"', end - 1);
        return line.substr(start + 1, end - start - 1);
    }
    return {};
}

// The map is the first launch argument, as in TheIsland_WP?listen?Port=7777.
std::string ReadMapName() {
    std::wistringstream words(GetCommandLineW());
    std::wstring word;
    while (words >> word) {
        const auto mark = word.find(L'?');
        if (mark != std::wstring::npos && mark > 0) {
            const std::wstring map = word.substr(0, mark);
            return std::string(map.begin(), map.end());
        }
    }
    return {};
}

// The game's own event lines; the engine's categorised noise (LogNet: and so on) and housekeeping stay out.
bool IsGameLine(const std::string& text) {
    for (const char* noise : {"Attempted GC", "StructureHibernation", "Added Explorer Note", "ServerPerformanceTunables"})
        if (text.rfind(noise, 0) == 0) return false;
    if (text.rfind("Log", 0) != 0) return true;
    const auto colon = text.find(':');
    return colon == std::string::npos || text.find(' ') < colon;
}

void TailLog(fs::path file) {
    std::streamoff offset = 0;
    {
        std::ifstream in(file, std::ios::binary | std::ios::ate);
        if (in) offset = in.tellg();
    }
    std::string partial;
    while (!g_tailStop.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        std::ifstream in(file, std::ios::binary);
        if (!in) continue;
        in.seekg(0, std::ios::end);
        if (in.tellg() < offset) {  // a new file took its place
            offset = 0;
            partial.clear();
        }
        in.seekg(offset);
        std::string chunk(65536, '\0');
        in.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        const std::streamsize got = in.gcount();
        if (got <= 0) continue;
        offset += got;
        partial.append(chunk.data(), static_cast<size_t>(got));
        size_t start = 0, end, sent = 0;
        while ((end = partial.find('\n', start)) != std::string::npos) {
            std::string line = Trim(partial.substr(start, end - start));
            start = end + 1;
            // Lines start with "[YYYY.MM.DD-hh.mm.ss:mmm][  0]".
            if (const auto bracket = line.find("]["); bracket != std::string::npos && !line.empty() && line[0] == '[') {
                const auto close = line.find(']', bracket + 2);
                if (close != std::string::npos) line = Trim(line.substr(close + 1));
            }
            if (sent < 32 && IsGameLine(line) && takaro::LogLineAllowed(line) && g_client) {
                g_client->Emit("log", tj::obj({{"msg", tj::str(line)}}));
                ++sent;
            }
        }
        partial.erase(0, start);
    }
}

constexpr char kHookNewPlayer[] = "AShooterGameMode.HandleNewPlayer_Implementation(AShooterPlayerController*,UPrimalPlayerData*,AShooterCharacter*,bool)";
constexpr char kHookLogout[] = "AShooterGameMode.Logout(AController*)";
constexpr char kHookPrintMessage[] = "UShooterCheatManager.PrintMessageOut(FString&)";
constexpr char kHookRconReply[] = "URCONServer.SetClientMessage(FString)";
constexpr char kHookServerChat[] = "UShooterCheatManager.ServerChat(FString&)";
constexpr char kHookPlayerDied[] = "AShooterGameState.NotifyPlayerDied(AShooterCharacter*,AShooterPlayerController*,APawn*,AActor*)";
constexpr char kHookDinoDie[] = "APrimalDinoCharacter.Die(float,FDamageEvent&,AController*,AActor*)";

}  // namespace

extern "C" __declspec(dllexport) void Plugin_Init()
{
    Log::Get().Init("TakaroAsa");
    const fs::path dir = PluginDir();
    LogSetFile((dir / "takaro.log").string());
    g_config = LoadConfig(dir);
    LogLine("TakaroAsa " TAKARO_PLUGIN_VERSION " starting");
    if (g_config.registrationToken.empty() && g_config.identityToken.empty()) {
        LogLine("no TAKARO_REGISTRATION_TOKEN in TakaroConfig.txt, not connecting");
        return;
    }

    fs::path gameRoot = dir;  // <root>\ShooterGame\Binaries\Win64\ArkApi\Plugins\TakaroAsa
    for (int i = 0; i < 6; ++i) gameRoot = gameRoot.parent_path();
    g_bans = std::make_unique<takaro::BanStore>(dir / "state" / "bans.json");
    g_known = std::make_unique<takaro::KnownPlayers>(dir / "state" / "known-players.json");
    g_gameVersion = ReadGameVersion(gameRoot);
    g_map = ReadMapName();
    g_banListFile = gameRoot / "ShooterGame" / "Binaries" / "Win64" / "BanList.txt";
    g_icon = LoadIcon(dir / "icon-256.png");
    LogLine("game version " + (g_gameVersion.empty() ? std::string("unknown") : g_gameVersion) + ", map " + (g_map.empty() ? std::string("unknown") : g_map));

    native::WinHttpTransport::Options options;
    options.url = g_config.url;
    options.caFile = g_config.caFile;
    g_transport = std::make_unique<native::WinHttpTransport>(options);

    takaro::Config cfg;
    cfg.registrationToken = g_config.registrationToken;
    cfg.identityToken = LoadIdentity(dir, g_config.identityToken);
    cfg.serverName = g_config.serverName;
    cfg.wireDebug = g_config.wireDebug;
    g_client = std::make_unique<takaro::Client>(cfg, g_transport.get(), RandomHex(16), [](const std::string& l) { LogLine(l); });
    g_client->SetReachability(Reachability);
    g_client->SetManifest(Manifest);
    g_client->SetLocalAction(LocalAction);

    auto& hooks = AsaApi::GetHooks();
    hooks.SetHook(kHookNewPlayer, Hook_HandleNewPlayer, &AShooterGameMode_HandleNewPlayer_Implementation_original);
    hooks.SetHook(kHookLogout, Hook_Logout, &AShooterGameMode_Logout_original);
    hooks.SetHook(kHookPrintMessage, Hook_PrintMessageOut, &UShooterCheatManager_PrintMessageOut_original);
    hooks.SetHook(kHookRconReply, Hook_SetClientMessage, &URCONServer_SetClientMessage_original);
    hooks.SetHook(kHookPlayerDied, Hook_NotifyPlayerDied, &AShooterGameState_NotifyPlayerDied_original);
    hooks.SetHook(kHookServerChat, Hook_ServerChat, &UShooterCheatManager_ServerChat_original);
    hooks.SetHook(kHookDinoDie, Hook_DinoDie, &APrimalDinoCharacter_Die_original);
    auto& commands = AsaApi::GetCommands();
    commands.AddOnTickCallback(kCallbackId, OnTick);
    commands.AddOnChatMessageCallback(kCallbackId, OnChat);
    g_tail = std::thread(TailLog, gameRoot / "ShooterGame" / "Saved" / "Logs" / "ShooterGame.log");
}

extern "C" __declspec(dllexport) void Plugin_Unload()
{
    auto& commands = AsaApi::GetCommands();
    commands.RemoveOnTickCallback(kCallbackId);
    commands.RemoveOnChatMessageCallback(kCallbackId);
    auto& hooks = AsaApi::GetHooks();
    hooks.DisableHook(kHookNewPlayer, Hook_HandleNewPlayer);
    hooks.DisableHook(kHookLogout, Hook_Logout);
    hooks.DisableHook(kHookPrintMessage, Hook_PrintMessageOut);
    hooks.DisableHook(kHookRconReply, Hook_SetClientMessage);
    hooks.DisableHook(kHookPlayerDied, Hook_NotifyPlayerDied);
    hooks.DisableHook(kHookServerChat, Hook_ServerChat);
    hooks.DisableHook(kHookDinoDie, Hook_DinoDie);
    g_tailStop = true;
    if (g_tail.joinable()) g_tail.join();
    if (g_client) g_client->Stop();
    g_client.reset();
    g_transport.reset();
}
