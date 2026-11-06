#include "starrupture_chat_hooks.h"

#include <optional>

TakaroChatDedupe::TakaroChatDedupe(std::chrono::milliseconds window) : window_(window) {}

bool TakaroChatDedupe::ShouldForward(
    const std::string& gameId, const std::string& message, Clock::time_point now)
{
    if (gameId.empty() || message.empty()) return false;
    for (auto item = seen_.begin(); item != seen_.end();)
    {
        if (now - item->second > window_) item = seen_.erase(item);
        else ++item;
    }
    const auto key = std::make_pair(gameId, message);
    const auto found = seen_.find(key);
    if (found != seen_.end() && now - found->second <= window_) return false;
    seen_.insert_or_assign(key, now);
    return true;
}

void TakaroChatDedupe::Reset() { seen_.clear(); }

bool ShouldCaptureTakaroAcceptedChat(bool hasPlayerState, const std::string& message)
{
    return hasPlayerState && !message.empty();
}

#if defined(TAKARO_STARRUPTURE_WITH_GAME_SDK)
#include "SDK.hpp"
#include "SDK/Chimera_parameters.hpp"
#include "modloader_process_events.h"
#include "plugin_interface.h"
#include "plugin_helpers.h"
#include "plugin_state.h"
#include "starrupture_sdk_bridge.h"

namespace
{
SDK::UFunction* g_chatFunction = nullptr;
bool g_chatSubscribed = false;
TakaroChatDedupe g_chatDedupe;

void TakaroAcceptedChatProcessEvent(void* rawObject, void* rawFunction, void* rawParameters)
{
    auto* object = static_cast<SDK::UObject*>(rawObject);
    auto* function = static_cast<SDK::UFunction*>(rawFunction);
    const void* parameters = rawParameters;
    std::optional<TakaroPlayerSnapshot> player;
    std::string message;
    if (function == g_chatFunction && object && parameters &&
        object->IsA(SDK::ACrGameStateBase::StaticClass()))
    {
        const auto* chat =
            static_cast<const SDK::Params::CrGameStateBase_MulticastClientChatCommit*>(parameters);
        message = chat->ChatText.ToString();
        if (ShouldCaptureTakaroAcceptedChat(chat->Player != nullptr, message))
            player = TakaroPlayerFromState(chat->Player);
    }
    if (player && g_chatDedupe.ShouldForward(player->gameId, message))
        TakaroState().AddChatMessage(*player, message);
}
}

bool InstallTakaroChatHook(IPluginHookUtils* hooks)
{
    (void)hooks;
    if (g_chatSubscribed) return true;
    auto* gameStateClass = SDK::ACrGameStateBase::StaticClass();
    g_chatFunction = gameStateClass
        ? gameStateClass->GetFunction("CrGameStateBase", "MulticastClientChatCommit")
        : nullptr;
    if (!g_chatFunction || !SubscribeTakaroProcessEvent(&TakaroAcceptedChatProcessEvent))
        return false;
    g_chatSubscribed = true;
    TAKARO_LOG_INFO("Accepted player-chat ProcessEvent observer installed.");
    return true;
}

bool EnsureTakaroChatHookForController(IPluginHookUtils* hooks, void* playerController)
{
    (void)playerController;
    return InstallTakaroChatHook(hooks);
}

void RemoveTakaroChatHook(IPluginHookUtils* hooks)
{
    (void)hooks;
    if (g_chatSubscribed) UnsubscribeTakaroProcessEvent(&TakaroAcceptedChatProcessEvent);
    g_chatSubscribed = false;
    g_chatFunction = nullptr;
    g_chatDedupe.Reset();
}
#else
bool InstallTakaroChatHook(IPluginHookUtils*) { return false; }
bool EnsureTakaroChatHookForController(IPluginHookUtils*, void*) { return false; }
void RemoveTakaroChatHook(IPluginHookUtils*) {}
#endif
