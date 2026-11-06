#pragma once

#include <chrono>
#include <map>
#include <string>
#include <utility>

struct IPluginHookUtils;

class TakaroChatDedupe
{
public:
    using Clock = std::chrono::steady_clock;

    explicit TakaroChatDedupe(std::chrono::milliseconds window = std::chrono::milliseconds(250));
    bool ShouldForward(const std::string& gameId, const std::string& message, Clock::time_point now = Clock::now());
    void Reset();

private:
    std::chrono::milliseconds window_;
    std::map<std::pair<std::string, std::string>, Clock::time_point> seen_;
};

bool ShouldCaptureTakaroAcceptedChat(bool hasPlayerState, const std::string& message);

bool InstallTakaroChatHook(IPluginHookUtils* hooks);
bool EnsureTakaroChatHookForController(IPluginHookUtils* hooks, void* playerController);
void RemoveTakaroChatHook(IPluginHookUtils* hooks);
