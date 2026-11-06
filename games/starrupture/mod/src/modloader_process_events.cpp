#include "modloader_process_events.h"

#include <algorithm>

bool TakaroProcessEventBus::Subscribe(
    TakaroRawProcessEventCallback callback,
    TakaroProcessEventRegistrar registerWithHost)
{
    if (!callback || !registerWithHost) return false;
    for (const auto& slot : callbacks_)
        if (slot.load() == callback) return true;
    const bool first = SubscriberCount() == 0;
    for (auto& slot : callbacks_)
    {
        TakaroRawProcessEventCallback empty = nullptr;
        if (!slot.compare_exchange_strong(empty, callback)) continue;
        if (first) registerWithHost(&TakaroProcessEventBusDispatch);
        return true;
    }
    return false;
}

void TakaroProcessEventBus::Unsubscribe(
    TakaroRawProcessEventCallback callback,
    TakaroProcessEventRegistrar unregisterFromHost)
{
    for (auto& slot : callbacks_)
    {
        TakaroRawProcessEventCallback expected = callback;
        if (!slot.compare_exchange_strong(expected, nullptr)) continue;
        if (SubscriberCount() == 0 && unregisterFromHost)
            unregisterFromHost(&TakaroProcessEventBusDispatch);
        return;
    }
}

void TakaroProcessEventBus::Dispatch(void* object, void* function, void* parameters) const
{
    for (const auto& slot : callbacks_)
        if (const auto callback = slot.load()) callback(object, function, parameters);
}

size_t TakaroProcessEventBus::SubscriberCount() const
{
    return static_cast<size_t>(std::count_if(callbacks_.begin(), callbacks_.end(),
        [](const auto& slot) { return slot.load() != nullptr; }));
}

namespace
{
TakaroProcessEventBus g_processEvents;
#if defined(TAKARO_STARRUPTURE_WITH_GAME_SDK)
TakaroProcessEventRegistrar g_registerWithModLoader = nullptr;
TakaroProcessEventRegistrar g_unregisterFromModLoader = nullptr;
#endif
}

void TakaroProcessEventBusDispatch(void* object, void* function, void* parameters)
{
    g_processEvents.Dispatch(object, function, parameters);
}

#if defined(TAKARO_STARRUPTURE_WITH_GAME_SDK)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <DbgHelp.h>

#include <array>
#include <filesystem>
#include <string>

#include "plugin_helpers.h"

namespace
{
constexpr wchar_t ModLoaderModuleName[] = L"StarRupture-ModLoader-Core.dll";
constexpr char RegisterDecorated[] =
    "?RegisterProcessEventCallback@GameInstanceInit@Hooks@@YAXP6AXPEAX00@Z@Z";
constexpr char UnregisterDecorated[] =
    "?UnregisterProcessEventCallback@GameInstanceInit@Hooks@@YAXP6AXPEAX00@Z@Z";
constexpr char RegisterUndecorated[] = "Hooks::GameInstanceInit::RegisterProcessEventCallback";
constexpr char UnregisterUndecorated[] = "Hooks::GameInstanceInit::UnregisterProcessEventCallback";

// The engine owns the DbgHelp session of GetCurrentProcess() for crash reporting.
// DbgHelp accepts any unique handle when it is not asked to invade the process,
// so this lookup gets a session of its own and cannot collide with that one.
const HANDLE SymbolSession = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(0x54414B41));

TakaroProcessEventRegistrar ResolveRegistrar(const char* decorated, const char* undecorated)
{
    struct SymbolStorage
    {
        SYMBOL_INFO symbol{};
        std::array<char, MAX_SYM_NAME> name{};
    } storage;
    for (const char* candidate : {decorated, undecorated})
    {
        storage.symbol = SYMBOL_INFO{};
        storage.symbol.SizeOfStruct = sizeof(SYMBOL_INFO);
        storage.symbol.MaxNameLen = MAX_SYM_NAME;
        const std::string qualified = "StarRupture-ModLoader-Core!" + std::string(candidate);
        if (SymFromName(SymbolSession, candidate, &storage.symbol) ||
            SymFromName(SymbolSession, qualified.c_str(), &storage.symbol))
            return reinterpret_cast<TakaroProcessEventRegistrar>(storage.symbol.Address);
    }
    TAKARO_LOG_WARN("ModLoader symbol %s was not found (error %lu).", undecorated, GetLastError());
    return nullptr;
}

bool ResolveModLoaderProcessEventHost()
{
    if (g_registerWithModLoader && g_unregisterFromModLoader) return true;
    const auto module = GetModuleHandleW(ModLoaderModuleName);
    if (!module)
    {
        TAKARO_LOG_WARN("ModLoader core module is not loaded under its expected name.");
        return false;
    }

    std::array<wchar_t, 32768> modulePath{};
    const auto pathLength = GetModuleFileNameW(
        module, modulePath.data(), static_cast<DWORD>(modulePath.size()));
    if (pathLength == 0 || pathLength >= modulePath.size()) return false;
    const auto symbolDirectory = std::filesystem::path(modulePath.data()).parent_path().wstring();

    // Symbol options are process-wide, so the engine's are put back afterwards.
    const auto previousOptions = SymGetOptions();
    SymSetOptions((previousOptions & ~static_cast<DWORD>(SYMOPT_UNDNAME)) |
        SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS);
    if (!SymInitializeW(SymbolSession, symbolDirectory.c_str(), FALSE))
    {
        TAKARO_LOG_WARN("DbgHelp session could not start (error %lu).", GetLastError());
        SymSetOptions(previousOptions);
        return false;
    }
    const auto moduleBase = reinterpret_cast<DWORD64>(module);
    SetLastError(ERROR_SUCCESS);
    if (SymLoadModuleExW(
            SymbolSession, nullptr, modulePath.data(), nullptr, moduleBase, 0, nullptr, 0) == 0 &&
        GetLastError() != ERROR_SUCCESS)
        TAKARO_LOG_WARN("ModLoader symbols could not be loaded (error %lu).", GetLastError());

    g_registerWithModLoader = ResolveRegistrar(RegisterDecorated, RegisterUndecorated);
    g_unregisterFromModLoader = ResolveRegistrar(UnregisterDecorated, UnregisterUndecorated);
    SymCleanup(SymbolSession);
    SymSetOptions(previousOptions);
    if (!g_registerWithModLoader || !g_unregisterFromModLoader)
    {
        g_registerWithModLoader = nullptr;
        g_unregisterFromModLoader = nullptr;
        return false;
    }
    TAKARO_LOG_INFO("Resolved pinned ModLoader ProcessEvent observer host from matching symbols.");
    return true;
}
}
#endif

bool SubscribeTakaroProcessEvent(TakaroRawProcessEventCallback callback)
{
#if defined(TAKARO_STARRUPTURE_WITH_GAME_SDK)
    if (!ResolveModLoaderProcessEventHost()) return false;
    return g_processEvents.Subscribe(callback, g_registerWithModLoader);
#else
    (void)callback;
    return false;
#endif
}

void UnsubscribeTakaroProcessEvent(TakaroRawProcessEventCallback callback)
{
#if defined(TAKARO_STARRUPTURE_WITH_GAME_SDK)
    g_processEvents.Unsubscribe(callback, g_unregisterFromModLoader);
#else
    (void)callback;
#endif
}
