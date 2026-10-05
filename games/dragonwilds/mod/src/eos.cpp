// EOS Connect lookups of the platform accounts behind a ProductUserId. See eos.h.
#include "eos.h"

#include "common.h"

#include <dlfcn.h>

#include <atomic>
#include <cstring>

// ---- the EOS SDK C ABI this file uses (eos_connect_types.h, eos_common.h; 8-byte packing) ----

namespace {

using EOS_HPlatform = void*;
using EOS_HConnect = void*;
using EOS_ProductUserId = void*;
using EOS_EResult = int32_t;
const EOS_EResult EOS_Success = 0;

const int32_t EOS_EAT_STEAM = 1;
const int32_t EOS_EAT_XBL = 3;

struct QueryProductUserIdMappingsOptions {
    int32_t ApiVersion;  // EOS_CONNECT_QUERYPRODUCTUSERIDMAPPINGS_API_LATEST
    EOS_ProductUserId LocalUserId;  // NULL: the dedicated-server form
    int32_t AccountIdType_DEPRECATED;
    EOS_ProductUserId* ProductUserIds;
    uint32_t ProductUserIdCount;
};
const int32_t kQueryMappingsApi = 2;

struct QueryProductUserIdMappingsCallbackInfo {
    EOS_EResult ResultCode;
    void* ClientData;
    EOS_ProductUserId LocalUserId;
};

struct CopyByAccountTypeOptions {
    int32_t ApiVersion;  // EOS_CONNECT_COPYPRODUCTUSEREXTERNALACCOUNTBYACCOUNTTYPE_API_LATEST
    EOS_ProductUserId TargetUserId;
    int32_t AccountIdType;
};
const int32_t kCopyByTypeApi = 1;

struct ExternalAccountInfo {
    int32_t ApiVersion;
    EOS_ProductUserId ProductUserId;
    const char* DisplayName;
    const char* AccountId;
    int32_t AccountIdType;
    int64_t LastLoginTime;
};

using FnTick = void (*)(EOS_HPlatform);
using FnGetConnect = EOS_HConnect (*)(EOS_HPlatform);
using FnPuidFromString = EOS_ProductUserId (*)(const char*);
using FnQueryCallback = void (*)(const QueryProductUserIdMappingsCallbackInfo*);
using FnQueryMappings = void (*)(EOS_HConnect, const QueryProductUserIdMappingsOptions*, void*, FnQueryCallback);
using FnCopyByType = EOS_EResult (*)(EOS_HConnect, const CopyByAccountTypeOptions*, ExternalAccountInfo**);
using FnInfoRelease = void (*)(ExternalAccountInfo*);

std::atomic<EOS_HPlatform> g_platform{nullptr};
std::atomic<uint64_t> g_ticks{0};

struct Api {
    FnGetConnect getConnect = nullptr;
    FnPuidFromString puidFromString = nullptr;
    FnQueryMappings queryMappings = nullptr;
    FnCopyByType copyByType = nullptr;
    FnInfoRelease infoRelease = nullptr;
    bool ok() const { return getConnect && puidFromString && queryMappings && copyByType && infoRelease; }
};

const Api& Sdk() {
    static Api api = [] {
        Api a;
        a.getConnect = (FnGetConnect)dlsym(RTLD_DEFAULT, "EOS_Platform_GetConnectInterface");
        a.puidFromString = (FnPuidFromString)dlsym(RTLD_DEFAULT, "EOS_ProductUserId_FromString");
        a.queryMappings = (FnQueryMappings)dlsym(RTLD_DEFAULT, "EOS_Connect_QueryProductUserIdMappings");
        a.copyByType = (FnCopyByType)dlsym(RTLD_DEFAULT, "EOS_Connect_CopyProductUserExternalAccountByAccountType");
        a.infoRelease = (FnInfoRelease)dlsym(RTLD_DEFAULT, "EOS_Connect_ExternalAccountInfo_Release");
        return a;
    }();
    return api;
}

// Retries cover a transient EOS backend error; a player with no linked Steam account still ends in
// Done after the first successful answer.
const int kMaxAttempts = 3;
const uint64_t kRetryMs = 10000;
const uint64_t kQueryTimeoutMs = 15000;

struct Entry {
    Eos::Lookup state = Eos::Lookup::Pending;
    Eos::Linked ids;
    int attempts = 0;
    bool inFlight = false;
    uint64_t lastAttemptMs = 0;
    EOS_EResult lastResult = EOS_Success;
};

Mutex g_lock;
std::map<std::string, Entry> g_entries;
std::atomic<uint64_t> g_queries{0}, g_answers{0}, g_failures{0};

bool AllDigits(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s)
        if (c < '0' || c > '9') return false;
    return true;
}

// A SteamID64 of an individual public-universe account: 0x0110000100000000 | accountId.
bool IsSteamId64(const std::string& s) {
    if (s.size() != 17 || !AllDigits(s)) return false;
    unsigned long long v = strtoull(s.c_str(), nullptr, 10);
    return (v >> 32) == 0x01100001ull && (uint32_t)v != 0;
}

std::string CopyAccount(EOS_HConnect connect, EOS_ProductUserId puid, int32_t type) {
    const Api& api = Sdk();
    CopyByAccountTypeOptions o{};
    o.ApiVersion = kCopyByTypeApi;
    o.TargetUserId = puid;
    o.AccountIdType = type;
    ExternalAccountInfo* info = nullptr;
    if (api.copyByType(connect, &o, &info) != EOS_Success || !info) return "";
    std::string id = info->AccountId ? info->AccountId : "";
    api.infoRelease(info);
    return id;
}

// Reads the SDK's mapping cache for one PUID. Game thread (inside EOS_Platform_Tick or the pump).
void FillFromCache(EOS_HConnect connect, EOS_ProductUserId handle, Eos::Linked& out) {
    std::string steam = CopyAccount(connect, handle, EOS_EAT_STEAM);
    if (IsSteamId64(steam)) out.steamId = steam;
    else if (!steam.empty()) PluginLog("eos: ignoring a Steam account id that is not a SteamID64 (%zu chars)", steam.size());
    std::string xbox = CopyAccount(connect, handle, EOS_EAT_XBL);
    if (AllDigits(xbox)) out.xboxLiveId = xbox;
}

void OnQueryDone(const QueryProductUserIdMappingsCallbackInfo* info) {
    std::string* puid = info ? (std::string*)info->ClientData : nullptr;
    if (!puid) return;
    EOS_EResult rc = info->ResultCode;
    Eos::Linked ids;
    EOS_HPlatform platform = g_platform.load(std::memory_order_relaxed);
    const Api& api = Sdk();
    if (rc == EOS_Success && platform) {
        EOS_HConnect connect = api.getConnect(platform);
        EOS_ProductUserId handle = connect ? api.puidFromString(puid->c_str()) : nullptr;
        if (handle) FillFromCache(connect, handle, ids);
    }
    {
        Guard g(g_lock);
        Entry& e = g_entries[*puid];
        e.inFlight = false;
        e.lastResult = rc;
        if (rc == EOS_Success) {
            e.state = Eos::Lookup::Done;
            e.ids = ids;
        } else if (e.attempts >= kMaxAttempts) {
            e.state = Eos::Lookup::Done;
        }
    }
    if (rc == EOS_Success) {
        g_answers++;
        PluginLog("eos: linked accounts of %s: steam=%s xbox=%s", puid->c_str(), ids.steamId.empty() ? "none" : "yes",
                  ids.xboxLiveId.empty() ? "none" : "yes");
    } else {
        g_failures++;
        PluginLog("eos: mapping query for %s failed (EOS_EResult %d)", puid->c_str(), (int)rc);
    }
    delete puid;
}

}  // namespace

// The server binary calls EOS_Platform_Tick through its PLT every frame; this definition wins
// because the plugin is preloaded. It only records the handle and forwards.
extern "C" __attribute__((visibility("default"))) void EOS_Platform_Tick(EOS_HPlatform handle) {
    static FnTick real = (FnTick)dlsym(RTLD_NEXT, "EOS_Platform_Tick");
    if (handle) {
        g_platform.store(handle, std::memory_order_relaxed);
        g_ticks.fetch_add(1, std::memory_order_relaxed);
    }
    if (real) real(handle);
}

namespace Eos {

Lookup LinkedAccounts(const std::string& puid, Linked& out) {
    if (puid.size() != 32) return Lookup::Unavailable;
    EOS_HPlatform platform = g_platform.load(std::memory_order_relaxed);
    const Api& api = Sdk();
    if (!platform || !api.ok()) return Lookup::Unavailable;
    uint64_t now = NowMs();
    {
        Guard g(g_lock);
        Entry& e = g_entries[puid];
        if (e.state == Lookup::Done) {
            out = e.ids;
            return Lookup::Done;
        }
        if (e.inFlight && now - e.lastAttemptMs < kQueryTimeoutMs) return Lookup::Pending;
        if (e.attempts > 0 && now - e.lastAttemptMs < kRetryMs) return Lookup::Pending;
        if (e.attempts >= kMaxAttempts) {
            e.state = Lookup::Done;  // the EOS backend never answered; report what is known (nothing)
            out = e.ids;
            return Lookup::Done;
        }
        e.attempts++;
        e.inFlight = true;
        e.lastAttemptMs = now;
    }
    EOS_HConnect connect = api.getConnect(platform);
    EOS_ProductUserId handle = connect ? api.puidFromString(puid.c_str()) : nullptr;
    if (!handle) {
        Guard g(g_lock);
        g_entries[puid].inFlight = false;
        return Lookup::Pending;
    }
    QueryProductUserIdMappingsOptions o{};
    o.ApiVersion = kQueryMappingsApi;
    o.LocalUserId = nullptr;
    o.ProductUserIds = &handle;
    o.ProductUserIdCount = 1;
    g_queries++;
    api.queryMappings(connect, &o, new std::string(puid), &OnQueryDone);
    return Lookup::Pending;
}

std::string DiagnosticsJson() {
    size_t done = 0, steam = 0;
    {
        Guard g(g_lock);
        for (auto& kv : g_entries) {
            if (kv.second.state == Lookup::Done) done++;
            if (!kv.second.ids.steamId.empty()) steam++;
        }
    }
    return std::string("{\"platformSeen\":") + (g_platform.load() ? "true" : "false") +
           ",\"sdkFunctions\":" + (Sdk().ok() ? "true" : "false") + ",\"ticks\":" + std::to_string(g_ticks.load()) +
           ",\"queries\":" + std::to_string(g_queries.load()) + ",\"answers\":" + std::to_string(g_answers.load()) +
           ",\"failures\":" + std::to_string(g_failures.load()) + ",\"resolved\":" + std::to_string(done) +
           ",\"withSteam\":" + std::to_string(steam) + "}";
}

}  // namespace Eos
