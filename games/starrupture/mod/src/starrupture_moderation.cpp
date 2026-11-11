#include "starrupture_moderation.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

#if defined(TAKARO_STARRUPTURE_WITH_GAME_SDK)
#include "SDK.hpp"
#endif

namespace
{
constexpr const char* kHeader = "takaro-starrupture-bans-v1";

std::string Trim(std::string value)
{
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](unsigned char ch) {
        return !std::isspace(ch);
    }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [](unsigned char ch) {
        return !std::isspace(ch);
    }).base(), value.end());
    return value;
}

int Decimal(const std::string& value, size_t start, size_t length)
{
    int result = 0;
    for (size_t index = start; index < start + length; ++index)
        result = result * 10 + (value[index] - '0');
    return result;
}

std::string HexEncode(const std::string& value)
{
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(value.size() * 2);
    for (const unsigned char byte : value)
    {
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 0x0f]);
    }
    return result;
}

std::optional<std::string> HexDecode(const std::string& value)
{
    if (value.size() % 2 != 0) return std::nullopt;
    const auto nibble = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
        return -1;
    };
    std::string result;
    result.reserve(value.size() / 2);
    for (size_t index = 0; index < value.size(); index += 2)
    {
        const int high = nibble(value[index]);
        const int low = nibble(value[index + 1]);
        if (high < 0 || low < 0) return std::nullopt;
        result.push_back(static_cast<char>((high << 4) | low));
    }
    return result;
}

std::vector<std::string> SplitTabs(const std::string& line)
{
    std::vector<std::string> fields;
    size_t start = 0;
    while (true)
    {
        const auto separator = line.find('\t', start);
        fields.push_back(line.substr(start, separator == std::string::npos ? separator : separator - start));
        if (separator == std::string::npos) return fields;
        start = separator + 1;
    }
}

std::string Serialize(const std::map<std::string, TakaroBanDto>& bans)
{
    std::string bytes = std::string(kHeader) + "\n";
    for (const auto& [_, ban] : bans)
    {
        bytes += HexEncode(ban.player.gameId) + "\t" + HexEncode(ban.player.name) + "\t" +
            HexEncode(ban.player.platformId) + "\t" + HexEncode(ban.player.steamId) + "\t" +
            HexEncode(ban.reason) + "\t" + HexEncode(ban.expiresAt.value_or("")) + "\n";
    }
    return bytes;
}

bool AtomicReplace(const std::filesystem::path& temporary, const std::filesystem::path& destination)
{
#if defined(_WIN32)
    return MoveFileExW(temporary.c_str(), destination.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    return std::rename(temporary.c_str(), destination.c_str()) == 0;
#endif
}

TakaroOperationFailure Failure(const TakaroOperationRequest& request, const std::string& reason)
{
    return TakaroOperationFailure{request.action + ": " + reason};
}

TakaroBanRegistry g_registry(std::filesystem::path("TakaroStarRupture.bans"));

#if defined(TAKARO_STARRUPTURE_WITH_GAME_SDK)
SDK::ACrPlayerControllerBase* FindController(const std::string& gameId)
{
    auto* world = SDK::UWorld::GetWorld();
    auto* gameState = world ? SDK::UGameplayStatics::GetGameState(world) : nullptr;
    if (!gameState) return nullptr;
    for (auto* basePlayerState : gameState->PlayerArray)
    {
        if (!basePlayerState || !basePlayerState->IsA(SDK::ACrPlayerStateBase::StaticClass())) continue;
        auto* playerState = static_cast<SDK::ACrPlayerStateBase*>(basePlayerState);
        if (playerState->GetUniqueNetIdAsString().ToString() != gameId) continue;
        auto* controller = playerState->GetPlayerController();
        if (!controller || !controller->IsA(SDK::ACrPlayerControllerBase::StaticClass())) return nullptr;
        return static_cast<SDK::ACrPlayerControllerBase*>(controller);
    }
    return nullptr;
}

void Disconnect(SDK::ACrPlayerControllerBase* controller, const std::string& reason)
{
    const auto wideReason = UtfN::Utf8StringToUtf16String<std::wstring>(reason);
    const SDK::FString reasonString(wideReason.c_str());
    controller->ClientReturnToMainMenuWithTextReason(SDK::UKismetTextLibrary::Conv_StringToText(reasonString));
}
#endif
}

std::optional<std::chrono::system_clock::time_point> ParseTakaroUtcTimestamp(const std::string& value)
{
    if (value.size() != 24 || value[4] != '-' || value[7] != '-' || value[10] != 'T' ||
        value[13] != ':' || value[16] != ':' || value[19] != '.' || value[23] != 'Z') return std::nullopt;
    for (const size_t index : {0u, 1u, 2u, 3u, 5u, 6u, 8u, 9u, 11u, 12u, 14u, 15u,
        17u, 18u, 20u, 21u, 22u})
        if (!std::isdigit(static_cast<unsigned char>(value[index]))) return std::nullopt;

    const int yearValue = Decimal(value, 0, 4);
    const int monthValue = Decimal(value, 5, 2);
    const int dayValue = Decimal(value, 8, 2);
    const int hourValue = Decimal(value, 11, 2);
    const int minuteValue = Decimal(value, 14, 2);
    const int secondValue = Decimal(value, 17, 2);
    const int millisValue = Decimal(value, 20, 3);
    if (hourValue > 23 || minuteValue > 59 || secondValue > 59) return std::nullopt;
    const std::chrono::year_month_day date{
        std::chrono::year{yearValue}, std::chrono::month{static_cast<unsigned int>(monthValue)},
        std::chrono::day{static_cast<unsigned int>(dayValue)}};
    if (!date.ok()) return std::nullopt;
    return std::chrono::sys_days{date} + std::chrono::hours{hourValue} +
        std::chrono::minutes{minuteValue} + std::chrono::seconds{secondValue} +
        std::chrono::milliseconds{millisValue};
}

TakaroBanRegistry::TakaroBanRegistry(std::filesystem::path path, NowFunction now)
    : path_(std::move(path)), now_(std::move(now)) {}

TakaroModerationResult TakaroBanRegistry::Fail(const std::string& error)
{
    error_ = error;
    return {false, error};
}

TakaroModerationResult TakaroBanRegistry::Initialize()
{
    bans_.clear();
    error_.clear();
    initialized_ = true;
    healthy_ = false;
    std::error_code statusError;
    if (!std::filesystem::exists(path_, statusError))
    {
        if (statusError) return Fail("ban registry path could not be inspected");
        healthy_ = true;
        return {true, {}};
    }

    std::ifstream input(path_, std::ios::binary);
    std::string header;
    if (!input || !std::getline(input, header) || header != kHeader)
        return Fail("ban registry is corrupt; original file was preserved");
    std::string line;
    while (std::getline(input, line))
    {
        if (line.empty()) continue;
        const auto fields = SplitTabs(line);
        if (fields.size() != 6) return Fail("ban registry is corrupt; original file was preserved");
        std::array<std::optional<std::string>, 6> decoded;
        for (size_t index = 0; index < fields.size(); ++index) decoded[index] = HexDecode(fields[index]);
        if (std::any_of(decoded.begin(), decoded.end(), [](const auto& field) { return !field.has_value(); }))
            return Fail("ban registry is corrupt; original file was preserved");
        TakaroBanDto ban;
        ban.player.gameId = *decoded[0];
        ban.player.name = *decoded[1];
        ban.player.platformId = *decoded[2];
        ban.player.steamId = *decoded[3];
        ban.reason = *decoded[4];
        if (!decoded[5]->empty()) ban.expiresAt = *decoded[5];
        if (Trim(ban.player.gameId).empty() || Trim(ban.player.name).empty() || Trim(ban.reason).empty() ||
            (ban.expiresAt && !ParseTakaroUtcTimestamp(*ban.expiresAt)) || bans_.contains(ban.player.gameId))
            return Fail("ban registry is corrupt; original file was preserved");
        bans_.emplace(ban.player.gameId, std::move(ban));
    }
    if (!input.eof()) return Fail("ban registry could not be read; original file was preserved");
    healthy_ = true;
    return {true, {}};
}

bool TakaroBanRegistry::Healthy() const { return initialized_ && healthy_; }
const std::string& TakaroBanRegistry::Error() const { return error_; }

TakaroModerationResult TakaroBanRegistry::Persist(const std::map<std::string, TakaroBanDto>& replacement)
{
    if (!Healthy()) return {false, error_.empty() ? "ban registry is not initialized" : error_};
    std::error_code directoryError;
    const auto parent = path_.parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, directoryError);
    if (directoryError) return {false, "ban registry directory could not be created"};
    auto temporary = path_;
    temporary += ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        const auto bytes = Serialize(replacement);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        output.flush();
        if (!output)
        {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            return {false, "ban registry temporary file could not be written"};
        }
    }
    if (!AtomicReplace(temporary, path_))
    {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return {false, "ban registry atomic replacement failed"};
    }
    bans_ = replacement;
    return {true, {}};
}

TakaroModerationResult TakaroBanRegistry::Upsert(const TakaroBanDto& ban)
{
    if (!Healthy()) return {false, error_.empty() ? "ban registry is not initialized" : error_};
    if (Trim(ban.player.gameId).empty() || Trim(ban.player.name).empty())
        return {false, "ban player requires stable gameId and name"};
    if (Trim(ban.reason).empty() || ban.reason.size() > 512)
        return {false, "ban reason must contain 1 through 512 bytes"};
    if (ban.expiresAt)
    {
        const auto expiry = ParseTakaroUtcTimestamp(*ban.expiresAt);
        if (!expiry) return {false, "ban expiry must be normalized ISO-8601 UTC"};
        if (*expiry <= now_()) return {false, "ban expiry must be in the future"};
    }
    auto replacement = bans_;
    replacement.insert_or_assign(ban.player.gameId, ban);
    return Persist(replacement);
}

TakaroModerationResult TakaroBanRegistry::Remove(const std::string& gameId)
{
    if (!Healthy()) return {false, error_.empty() ? "ban registry is not initialized" : error_};
    const auto normalized = Trim(gameId);
    if (normalized.empty()) return {false, "unban requires a stable gameId"};
    if (!bans_.contains(normalized)) return {true, {}};
    auto replacement = bans_;
    replacement.erase(normalized);
    return Persist(replacement);
}

TakaroBanListResult TakaroBanRegistry::List()
{
    if (!Healthy()) return {{false, error_.empty() ? "ban registry is not initialized" : error_}, {}};
    auto replacement = bans_;
    const auto now = now_();
    std::erase_if(replacement, [&](const auto& entry) {
        return entry.second.expiresAt && *ParseTakaroUtcTimestamp(*entry.second.expiresAt) <= now;
    });
    if (replacement.size() != bans_.size())
    {
        const auto persisted = Persist(replacement);
        if (!persisted.ok) return {persisted, {}};
    }
    TakaroBanListResult result;
    result.ok = true;
    for (const auto& [_, ban] : bans_) result.bans.push_back(ban);
    return result;
}

bool TakaroBanRegistry::IsActive(const std::string& gameId)
{
    const auto listed = List();
    if (!listed.ok) return false;
    return std::any_of(listed.bans.begin(), listed.bans.end(), [&](const auto& ban) {
        return ban.player.gameId == gameId;
    });
}

TakaroBanRegistry& TakaroModerationRegistry() { return g_registry; }

std::filesystem::path DefaultTakaroBanRegistryPath()
{
#if defined(_WIN32)
    std::wstring executable(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (length == 0 || length >= executable.size()) return {};
    executable.resize(length);
    return std::filesystem::path(executable).parent_path() / "ModLoader" / "Plugins" / "config" /
        "TakaroStarRupture.bans";
#else
    return std::filesystem::current_path() / "ModLoader" / "Plugins" / "config" / "TakaroStarRupture.bans";
#endif
}

void ConfigureTakaroModerationRegistry(const std::filesystem::path& path)
{
    g_registry = TakaroBanRegistry(path.empty() ? DefaultTakaroBanRegistryPath() : path);
    g_registry.Initialize();
}

TakaroOperationResult ExecuteTakaroModerationOperation(const TakaroOperationRequest& request)
{
#if defined(TAKARO_STARRUPTURE_WITH_GAME_SDK)
    auto& registry = TakaroModerationRegistry();
    if (!registry.Healthy()) return Failure(request, registry.Error());
    if (request.type == TakaroOperationType::ListBans)
    {
        const auto listed = registry.List();
        if (!listed.ok) return Failure(request, listed.error);
        return TakaroOperationSuccess{request.type, listed.bans};
    }
    if (request.type == TakaroOperationType::UnbanPlayer)
    {
        const auto playerJson = ExtractJsonObject(request.argsJson, "player");
        auto gameId = ExtractJsonString(request.argsJson, "gameId");
        if (!gameId && playerJson) gameId = ExtractJsonString(*playerJson, "gameId");
        if (!gameId) return Failure(request, "gameId is required");
        const auto removed = registry.Remove(*gameId);
        if (!removed.ok) return Failure(request, removed.error);
        return TakaroOperationSuccess{request.type, std::monostate{}};
    }
    if (request.type == TakaroOperationType::BanPlayer)
    {
        const auto playerJson = ExtractJsonObject(request.argsJson, "player");
        const auto gameId = playerJson ? ExtractJsonString(*playerJson, "gameId") : std::nullopt;
        // Takaro sends only the gameId, and a ban must work for a player who is offline.
        const auto name = playerJson ? ExtractJsonString(*playerJson, "name") : std::nullopt;
        const auto reason = ExtractJsonString(request.argsJson, "reason");
        if (!gameId) return Failure(request, "player gameId is required");
        TakaroBanDto ban;
        ban.player.gameId = *gameId;
        ban.player.name = name.value_or(*gameId);
        if (const auto value = ExtractJsonString(*playerJson, "platformId")) ban.player.platformId = *value;
        if (const auto value = ExtractJsonString(*playerJson, "steamId")) ban.player.steamId = *value;
        ban.reason = reason.value_or("Banned through Takaro");
        if (const auto value = ExtractJsonString(request.argsJson, "expiresAt")) ban.expiresAt = *value;
        const auto stored = registry.Upsert(ban);
        if (!stored.ok) return Failure(request, stored.error);
        if (auto* controller = FindController(ban.player.gameId)) Disconnect(controller, ban.reason);
        return TakaroOperationSuccess{request.type, std::monostate{}};
    }
    return Failure(request, "operation is not a moderation action");
#else
    return Failure(request, "operation requires the pinned StarRupture Game SDK");
#endif
}

void EnforceTakaroBans()
{
#if defined(TAKARO_STARRUPTURE_WITH_GAME_SDK)
    const auto listed = TakaroModerationRegistry().List();
    if (!listed.ok) return;
    for (const auto& ban : listed.bans)
        if (auto* controller = FindController(ban.player.gameId)) Disconnect(controller, ban.reason);
#endif
}
