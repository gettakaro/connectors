#pragma once

#include "plugin_state.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

struct TakaroModerationResult
{
    bool ok = false;
    std::string error;
};

struct TakaroBanListResult : TakaroModerationResult
{
    std::vector<TakaroBanDto> bans;
};

std::optional<std::chrono::system_clock::time_point> ParseTakaroUtcTimestamp(const std::string& value);

class TakaroBanRegistry
{
public:
    using Clock = std::chrono::system_clock;
    using NowFunction = std::function<Clock::time_point()>;

    explicit TakaroBanRegistry(std::filesystem::path path, NowFunction now = [] { return Clock::now(); });
    TakaroModerationResult Initialize();
    bool Healthy() const;
    const std::string& Error() const;
    TakaroModerationResult Upsert(const TakaroBanDto& ban);
    TakaroModerationResult Remove(const std::string& gameId);
    TakaroBanListResult List();
    bool IsActive(const std::string& gameId);

private:
    TakaroModerationResult Persist(const std::map<std::string, TakaroBanDto>& replacement);
    TakaroModerationResult Fail(const std::string& error);

    std::filesystem::path path_;
    NowFunction now_;
    std::map<std::string, TakaroBanDto> bans_;
    bool initialized_ = false;
    bool healthy_ = false;
    std::string error_;
};

TakaroBanRegistry& TakaroModerationRegistry();
std::filesystem::path DefaultTakaroBanRegistryPath();
void ConfigureTakaroModerationRegistry(const std::filesystem::path& path);
TakaroOperationResult ExecuteTakaroModerationOperation(const TakaroOperationRequest& request);
void EnforceTakaroBans();
