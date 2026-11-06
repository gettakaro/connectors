#include "plugin_state.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <iomanip>
#include <set>
#include <sstream>

namespace
{
constexpr std::array<std::pair<TakaroOperationType, const char*>, 19> kOperationNames = {{
    {TakaroOperationType::GetPlayer, "getPlayer"},
    {TakaroOperationType::GetPlayers, "getPlayers"},
    {TakaroOperationType::GetPlayerLocation, "getPlayerLocation"},
    {TakaroOperationType::GetPlayerInventory, "getPlayerInventory"},
    {TakaroOperationType::GiveItem, "giveItem"},
    {TakaroOperationType::ListItems, "listItems"},
    {TakaroOperationType::ListEntities, "listEntities"},
    {TakaroOperationType::ListLocations, "listLocations"},
    {TakaroOperationType::ExecuteConsoleCommand, "executeConsoleCommand"},
    {TakaroOperationType::SendMessage, "sendMessage"},
    {TakaroOperationType::TeleportPlayer, "teleportPlayer"},
    {TakaroOperationType::TestReachability, "testReachability"},
    {TakaroOperationType::KickPlayer, "kickPlayer"},
    {TakaroOperationType::BanPlayer, "banPlayer"},
    {TakaroOperationType::UnbanPlayer, "unbanPlayer"},
    {TakaroOperationType::ListBans, "listBans"},
    {TakaroOperationType::Shutdown, "shutdown"},
    {TakaroOperationType::GetMapInfo, "getMapInfo"},
    {TakaroOperationType::GetMapTile, "getMapTile"},
}};

std::string NewBridgeInstanceId()
{
    static std::atomic<uint64_t> sequence{0};
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    std::ostringstream value;
    value << std::hex << ticks << "-" << ++sequence;
    return value.str();
}
TakaroPluginState g_state;

std::string LowerTrimmed(std::string value)
{
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](unsigned char ch) { return !std::isspace(ch); }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(), value.end());
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

bool ValidPlayer(const TakaroPlayerSnapshot& player)
{
    return !LowerTrimmed(player.gameId).empty() && !LowerTrimmed(player.name).empty() && player.ping >= 0 &&
        (!player.hasLocation || (std::isfinite(player.x) && std::isfinite(player.y) && std::isfinite(player.z)));
}

bool ValidPosition(const TakaroPositionDto& position)
{
    return std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z);
}

bool ValidMetadataObject(const std::optional<std::string>& metadata)
{
    if (!metadata) return true;
    const auto normalized = LowerTrimmed(*metadata);
    if (normalized.size() < 2 || normalized.front() != '{' || normalized.back() != '}') return false;
    const auto parsed = ExtractJsonObject("{\"metadata\":" + *metadata + "}", "metadata");
    return parsed && LowerTrimmed(*parsed) == normalized;
}

bool IsNormalizedIso8601UtcTimestamp(const std::string& value)
{
    if (value.size() != 24 || value[4] != '-' || value[7] != '-' || value[10] != 'T' ||
        value[13] != ':' || value[16] != ':' || value[19] != '.' || value[23] != 'Z') return false;
    for (const size_t index : {0u, 1u, 2u, 3u, 5u, 6u, 8u, 9u, 11u, 12u, 14u, 15u,
        17u, 18u, 20u, 21u, 22u})
    {
        if (value[index] < '0' || value[index] > '9') return false;
    }
    const auto decimal = [&](size_t index, size_t length) {
        int result = 0;
        for (size_t offset = 0; offset < length; ++offset) result = result * 10 + (value[index + offset] - '0');
        return result;
    };
    const int year = decimal(0, 4);
    const int month = decimal(5, 2);
    const int day = decimal(8, 2);
    const int hour = decimal(11, 2);
    const int minute = decimal(14, 2);
    const int second = decimal(17, 2);
    if (month < 1 || month > 12 || hour > 23 || minute > 59 || second > 59) return false;

    const bool leapYear = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    const std::array<int, 12> daysInMonth = {31, leapYear ? 29 : 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return day >= 1 && day <= daysInMonth[static_cast<size_t>(month - 1)];
}

std::string PlayerJson(const TakaroPlayerSnapshot& player, bool includeLocation, bool includeSkills = false);

template <typename T>
std::string JsonArray(const std::vector<T>& values, const std::function<std::string(const T&)>& serialize)
{
    std::string json = "[";
    for (size_t index = 0; index < values.size(); ++index)
    {
        if (index) json += ",";
        json += serialize(values[index]);
    }
    return json + "]";
}

bool ExpectedResultShape(TakaroOperationType type, const TakaroResultValue& value)
{
    switch (type)
    {
    case TakaroOperationType::GetPlayer:
        if (const auto* player = std::get_if<std::optional<TakaroPlayerSnapshot>>(&value))
            return !*player || ValidPlayer(**player);
        return false;
    case TakaroOperationType::GetPlayers:
        if (const auto* players = std::get_if<std::vector<TakaroPlayerSnapshot>>(&value))
            return std::all_of(players->begin(), players->end(), ValidPlayer);
        return false;
    case TakaroOperationType::GetPlayerLocation:
        if (const auto* position = std::get_if<std::optional<TakaroPositionDto>>(&value))
            return !*position || ValidPosition(**position);
        return false;
    case TakaroOperationType::GetPlayerInventory:
    case TakaroOperationType::ListItems:
        if (const auto* items = std::get_if<std::vector<TakaroItemDto>>(&value)) return std::all_of(items->begin(), items->end(), [](const auto& item) {
            return !LowerTrimmed(item.name).empty() && !LowerTrimmed(item.code).empty() &&
                (!item.amount || std::isfinite(*item.amount));
        });
        return false;
    case TakaroOperationType::ListEntities:
        if (const auto* entities = std::get_if<std::vector<TakaroEntityDto>>(&value)) return std::all_of(entities->begin(), entities->end(), [](const auto& entity) {
            const bool validType = !entity.type || *entity.type == "hostile" || *entity.type == "friendly" || *entity.type == "neutral";
            return !LowerTrimmed(entity.code).empty() && !LowerTrimmed(entity.name).empty() &&
                (!entity.description || !LowerTrimmed(*entity.description).empty()) && validType &&
                ValidMetadataObject(entity.metadataJson);
        });
        return false;
    case TakaroOperationType::ListLocations:
        if (const auto* locations = std::get_if<std::vector<TakaroLocationDto>>(&value)) return std::all_of(locations->begin(), locations->end(), [](const auto& location) {
            return ValidPosition(location.position) && !LowerTrimmed(location.name).empty() &&
                (!location.code || !LowerTrimmed(*location.code).empty()) && ValidMetadataObject(location.metadataJson);
        });
        return false;
    case TakaroOperationType::ExecuteConsoleCommand:
        if (const auto* command = std::get_if<TakaroCommandOutputDto>(&value))
            return !LowerTrimmed(command->rawResult).empty() &&
                (!command->errorMessage || !LowerTrimmed(*command->errorMessage).empty());
        return false;
    case TakaroOperationType::TestReachability:
        if (const auto* reachability = std::get_if<TakaroReachabilityDto>(&value))
            return (!reachability->reason || !LowerTrimmed(*reachability->reason).empty()) &&
                (!reachability->latency || (std::isfinite(*reachability->latency) && *reachability->latency >= 0));
        return false;
    case TakaroOperationType::ListBans:
        if (const auto* bans = std::get_if<std::vector<TakaroBanDto>>(&value)) return std::all_of(bans->begin(), bans->end(), [](const auto& ban) {
            return ValidPlayer(ban.player) && !LowerTrimmed(ban.reason).empty() &&
                (!ban.expiresAt || IsNormalizedIso8601UtcTimestamp(*ban.expiresAt));
        });
        return false;
    case TakaroOperationType::GetMapInfo:
        if (const auto* map = std::get_if<TakaroMapInfoDto>(&value))
        {
            const bool finite = std::isfinite(map->mapBlockSize) && std::isfinite(map->maxZoom) &&
                std::isfinite(map->mapSizeX) && std::isfinite(map->mapSizeY) && std::isfinite(map->mapSizeZ);
            const bool nonNegative = map->mapBlockSize >= 0 && map->maxZoom >= 0 && map->mapSizeX >= 0 &&
                map->mapSizeY >= 0 && map->mapSizeZ >= 0;
            return finite && nonNegative && (!map->enabled ||
                (map->mapBlockSize > 0 && map->mapSizeX > 0 && map->mapSizeY > 0 && map->mapSizeZ > 0));
        }
        return false;
    case TakaroOperationType::GetMapTile: return std::holds_alternative<std::string>(value);
    case TakaroOperationType::GiveItem:
    case TakaroOperationType::SendMessage:
    case TakaroOperationType::TeleportPlayer:
    case TakaroOperationType::KickPlayer:
    case TakaroOperationType::BanPlayer:
    case TakaroOperationType::UnbanPlayer:
    case TakaroOperationType::Shutdown: return std::holds_alternative<std::monostate>(value);
    }
    return false;
}

std::string PositionJson(const TakaroPositionDto& value)
{
    std::ostringstream json;
    json << "{\"x\":" << value.x << ",\"y\":" << value.y << ",\"z\":" << value.z;
    if (value.dimension) json << ",\"dimension\":\"" << JsonEscape(*value.dimension) << "\"";
    return json.str() + "}";
}

std::string ResultJson(const TakaroOperationSuccess& result)
{
    const auto& value = result.value;
    if (std::holds_alternative<std::monostate>(value)) return "null";
    if (const auto* player = std::get_if<std::optional<TakaroPlayerSnapshot>>(&value))
        return *player ? PlayerJson(**player, false) : "null";
    if (const auto* players = std::get_if<std::vector<TakaroPlayerSnapshot>>(&value))
        return JsonArray<TakaroPlayerSnapshot>(*players, [](const auto& item) { return PlayerJson(item, false); });
    if (const auto* position = std::get_if<std::optional<TakaroPositionDto>>(&value))
        return *position ? PositionJson(**position) : "null";
    if (const auto* items = std::get_if<std::vector<TakaroItemDto>>(&value)) return JsonArray<TakaroItemDto>(*items, [](const auto& item) {
        std::string json = "{\"name\":\"" + JsonEscape(item.name) + "\",\"code\":\"" + JsonEscape(item.code) + "\"";
        if (item.description) json += ",\"description\":\"" + JsonEscape(*item.description) + "\"";
        if (item.amount) json += ",\"amount\":" + std::to_string(*item.amount);
        if (item.quality) json += ",\"quality\":\"" + JsonEscape(*item.quality) + "\"";
        return json + "}";
    });
    if (const auto* entities = std::get_if<std::vector<TakaroEntityDto>>(&value)) return JsonArray<TakaroEntityDto>(*entities, [](const auto& item) {
        std::string json = "{\"code\":\"" + JsonEscape(item.code) + "\",\"name\":\"" + JsonEscape(item.name) + "\"";
        if (item.description) json += ",\"description\":\"" + JsonEscape(*item.description) + "\"";
        if (item.type) json += ",\"type\":\"" + JsonEscape(*item.type) + "\"";
        if (item.metadataJson) json += ",\"metadata\":" + *item.metadataJson;
        return json + "}";
    });
    if (const auto* locations = std::get_if<std::vector<TakaroLocationDto>>(&value)) return JsonArray<TakaroLocationDto>(*locations, [](const auto& item) {
        std::string json = "{\"position\":" + PositionJson(item.position) + ",\"name\":\"" + JsonEscape(item.name) + "\"";
        if (item.code) json += ",\"code\":\"" + JsonEscape(*item.code) + "\"";
        if (item.metadataJson) json += ",\"metadata\":" + *item.metadataJson;
        return json + "}";
    });
    if (const auto* output = std::get_if<TakaroCommandOutputDto>(&value)) {
        std::string json = "{\"rawResult\":\"" + JsonEscape(output->rawResult) + "\",\"success\":" + (output->success ? "true" : "false");
        if (output->errorMessage) json += ",\"errorMessage\":\"" + JsonEscape(*output->errorMessage) + "\"";
        return json + "}";
    }
    if (const auto* reachability = std::get_if<TakaroReachabilityDto>(&value)) {
        std::string json = std::string("{\"connectable\":") + (reachability->connectable ? "true" : "false");
        if (reachability->reason) json += ",\"reason\":\"" + JsonEscape(*reachability->reason) + "\"";
        if (reachability->latency) json += ",\"latency\":" + std::to_string(*reachability->latency);
        return json + "}";
    }
    if (const auto* bans = std::get_if<std::vector<TakaroBanDto>>(&value)) return JsonArray<TakaroBanDto>(*bans, [](const auto& item) {
        std::string json = "{\"player\":" + PlayerJson(item.player, false) + ",\"reason\":\"" + JsonEscape(item.reason) + "\"";
        if (item.expiresAt) json += ",\"expiresAt\":\"" + JsonEscape(*item.expiresAt) + "\"";
        return json + "}";
    });
    if (const auto* map = std::get_if<TakaroMapInfoDto>(&value)) {
        std::ostringstream json;
        json << "{\"enabled\":" << (map->enabled ? "true" : "false") << ",\"mapBlockSize\":" << map->mapBlockSize
             << ",\"maxZoom\":" << map->maxZoom << ",\"mapSizeX\":" << map->mapSizeX << ",\"mapSizeY\":" << map->mapSizeY
             << ",\"mapSizeZ\":" << map->mapSizeZ << "}";
        return json.str();
    }
    return "\"" + JsonEscape(std::get<std::string>(value)) + "\"";
}

std::string PlayerJson(const TakaroPlayerSnapshot& player, bool includeLocation, bool includeSkills)
{
    std::ostringstream json;
    json << "{\"gameId\":\"" << JsonEscape(player.gameId) << "\",\"name\":\"" << JsonEscape(player.name) << "\"";
    if (!player.platformId.empty()) json << ",\"platformId\":\"" << JsonEscape(player.platformId) << "\"";
    if (!player.steamId.empty()) json << ",\"steamId\":\"" << JsonEscape(player.steamId) << "\"";
    if (player.ping > 0) json << ",\"ping\":" << player.ping;
    if (includeLocation && player.hasLocation)
    {
        json << ",\"location\":{\"x\":" << player.x << ",\"y\":" << player.y << ",\"z\":" << player.z << "}";
    }
    if (includeSkills && player.skills)
    {
        json << ",\"skills\":[";
        for (size_t index = 0; index < player.skills->size(); ++index)
        {
            const auto& reading = (*player.skills)[index];
            if (index > 0) json << ",";
            json << "{\"skill\":\"" << JsonEscape(reading.skill) << "\",\"level\":" << reading.level
                 << ",\"experience\":" << reading.experience << "}";
        }
        json << "]";
    }
    json << "}";
    return json.str();
}

class JsonReader
{
public:
    explicit JsonReader(const std::string& source) : source_(source) {}

    bool ParseActionEnvelope(std::string& action, std::string& argsJson, int64_t& deadlineUnixMs, std::string& error)
    {
        SkipSpace();
        if (!Consume('{')) return Fail(error, "request must be a JSON object");
        std::set<std::string> keys;
        bool haveAction = false;
        bool haveArgs = false;
        bool haveDeadline = false;
        SkipSpace();
        if (Consume('}')) return Fail(error, "request requires action and args");
        while (true)
        {
            std::string key;
            if (!ParseString(key)) return Fail(error, "object key must be a JSON string");
            if (!keys.insert(key).second) return Fail(error, "duplicate object key");
            SkipSpace();
            if (!Consume(':')) return Fail(error, "missing colon after object key");
            SkipSpace();
            if (key == "action")
            {
                if (!ParseString(action) || action.empty()) return Fail(error, "action must be a non-empty string");
                haveAction = true;
            }
            else if (key == "args")
            {
                if (Peek() != '{') return Fail(error, "args must be a JSON object");
                const auto begin = position_;
                if (!SkipValue(0)) return Fail(error, "args is invalid JSON");
                argsJson = source_.substr(begin, position_ - begin);
                haveArgs = true;
            }
            else if (key == "deadlineUnixMs")
            {
                if (!ParsePositiveInteger(deadlineUnixMs)) return Fail(error, "deadlineUnixMs must be a positive integer");
                haveDeadline = true;
            }
            else
            {
                return Fail(error, "unknown action request field");
            }
            SkipSpace();
            if (Consume('}')) break;
            if (!Consume(',')) return Fail(error, "missing comma between object fields");
            SkipSpace();
        }
        SkipSpace();
        if (position_ != source_.size()) return Fail(error, "trailing data after JSON object");
        if (!haveAction || !haveArgs || !haveDeadline)
            return Fail(error, "request requires action, args, and deadlineUnixMs");
        return true;
    }

    std::optional<std::string> ParseTopLevelString(const std::string& desired)
    {
        SkipSpace();
        if (!Consume('{')) return std::nullopt;
        std::set<std::string> keys;
        std::optional<std::string> result;
        SkipSpace();
        if (Consume('}')) return result;
        while (true)
        {
            std::string key;
            if (!ParseString(key) || !keys.insert(key).second) return std::nullopt;
            SkipSpace();
            if (!Consume(':')) return std::nullopt;
            SkipSpace();
            if (key == desired)
            {
                std::string value;
                if (!ParseString(value)) return std::nullopt;
                result = std::move(value);
            }
            else if (!SkipValue(0)) return std::nullopt;
            SkipSpace();
            if (Consume('}')) break;
            if (!Consume(',')) return std::nullopt;
            SkipSpace();
        }
        SkipSpace();
        return position_ == source_.size() ? result : std::nullopt;
    }

    std::optional<std::string> ParseTopLevelObject(const std::string& desired)
    {
        SkipSpace();
        if (!Consume('{')) return std::nullopt;
        std::set<std::string> keys;
        std::optional<std::string> result;
        SkipSpace();
        if (Consume('}')) return result;
        while (true)
        {
            std::string key;
            if (!ParseString(key) || !keys.insert(key).second) return std::nullopt;
            SkipSpace();
            if (!Consume(':')) return std::nullopt;
            SkipSpace();
            if (key == desired)
            {
                if (Peek() != '{') return std::nullopt;
                const auto begin = position_;
                if (!SkipValue(0)) return std::nullopt;
                result = source_.substr(begin, position_ - begin);
            }
            else if (!SkipValue(0)) return std::nullopt;
            SkipSpace();
            if (Consume('}')) break;
            if (!Consume(',')) return std::nullopt;
            SkipSpace();
        }
        SkipSpace();
        return position_ == source_.size() ? result : std::nullopt;
    }

    std::optional<int64_t> ParseTopLevelPositiveInteger(const std::string& desired)
    {
        SkipSpace();
        if (!Consume('{')) return std::nullopt;
        std::set<std::string> keys;
        std::optional<int64_t> result;
        SkipSpace();
        if (Consume('}')) return result;
        while (true)
        {
            std::string key;
            if (!ParseString(key) || !keys.insert(key).second) return std::nullopt;
            SkipSpace();
            if (!Consume(':')) return std::nullopt;
            SkipSpace();
            if (key == desired)
            {
                int64_t value = 0;
                if (!ParsePositiveInteger(value)) return std::nullopt;
                result = value;
            }
            else if (!SkipValue(0)) return std::nullopt;
            SkipSpace();
            if (Consume('}')) break;
            if (!Consume(',')) return std::nullopt;
            SkipSpace();
        }
        SkipSpace();
        return position_ == source_.size() ? result : std::nullopt;
    }

    std::optional<double> ParseTopLevelFiniteNumber(const std::string& desired)
    {
        SkipSpace();
        if (!Consume('{')) return std::nullopt;
        std::set<std::string> keys;
        std::optional<double> result;
        SkipSpace();
        if (Consume('}')) return result;
        while (true)
        {
            std::string key;
            if (!ParseString(key) || !keys.insert(key).second) return std::nullopt;
            SkipSpace();
            if (!Consume(':')) return std::nullopt;
            SkipSpace();
            if (key == desired)
            {
                double value = 0;
                if (!ParseFiniteNumber(value)) return std::nullopt;
                result = value;
            }
            else if (!SkipValue(0)) return std::nullopt;
            SkipSpace();
            if (Consume('}')) break;
            if (!Consume(',')) return std::nullopt;
            SkipSpace();
        }
        SkipSpace();
        return position_ == source_.size() ? result : std::nullopt;
    }

private:
    bool ParsePositiveInteger(int64_t& value)
    {
        const auto begin = position_;
        while (std::isdigit(static_cast<unsigned char>(Peek()))) ++position_;
        if (begin == position_) return false;
        const auto result = std::from_chars(source_.data() + begin, source_.data() + position_, value);
        return result.ec == std::errc{} && result.ptr == source_.data() + position_ && value > 0;
    }

    bool ParseFiniteNumber(double& value)
    {
        const auto begin = position_;
        if (!ParseNumber()) return false;
        const auto result = std::from_chars(source_.data() + begin, source_.data() + position_, value);
        return result.ec == std::errc{} && result.ptr == source_.data() + position_ && std::isfinite(value);
    }

    bool SkipValue(int depth)
    {
        if (depth > 32) return false;
        SkipSpace();
        const char current = Peek();
        if (current == '"')
        {
            std::string ignored;
            return ParseString(ignored);
        }
        if (current == '{')
        {
            ++position_;
            std::set<std::string> keys;
            SkipSpace();
            if (Consume('}')) return true;
            while (true)
            {
                std::string key;
                if (!ParseString(key) || !keys.insert(key).second) return false;
                SkipSpace();
                if (!Consume(':') || !SkipValue(depth + 1)) return false;
                SkipSpace();
                if (Consume('}')) return true;
                if (!Consume(',')) return false;
                SkipSpace();
            }
        }
        if (current == '[')
        {
            ++position_;
            SkipSpace();
            if (Consume(']')) return true;
            while (true)
            {
                if (!SkipValue(depth + 1)) return false;
                SkipSpace();
                if (Consume(']')) return true;
                if (!Consume(',')) return false;
                SkipSpace();
            }
        }
        if (source_.compare(position_, 4, "true") == 0) { position_ += 4; return true; }
        if (source_.compare(position_, 5, "false") == 0) { position_ += 5; return true; }
        if (source_.compare(position_, 4, "null") == 0) { position_ += 4; return true; }
        return ParseNumber();
    }

    bool ParseNumber()
    {
        const auto begin = position_;
        if (Peek() == '-') ++position_;
        if (Peek() == '0') ++position_;
        else
        {
            if (!std::isdigit(static_cast<unsigned char>(Peek()))) return false;
            while (std::isdigit(static_cast<unsigned char>(Peek()))) ++position_;
        }
        if (Peek() == '.')
        {
            ++position_;
            if (!std::isdigit(static_cast<unsigned char>(Peek()))) return false;
            while (std::isdigit(static_cast<unsigned char>(Peek()))) ++position_;
        }
        if (Peek() == 'e' || Peek() == 'E')
        {
            ++position_;
            if (Peek() == '+' || Peek() == '-') ++position_;
            if (!std::isdigit(static_cast<unsigned char>(Peek()))) return false;
            while (std::isdigit(static_cast<unsigned char>(Peek()))) ++position_;
        }
        return position_ > begin;
    }

    bool ParseString(std::string& value)
    {
        SkipSpace();
        if (!Consume('"')) return false;
        value.clear();
        while (position_ < source_.size())
        {
            const unsigned char ch = static_cast<unsigned char>(source_[position_++]);
            if (ch == '"') return true;
            if (ch < 0x20) return false;
            if (ch != '\\') { value += static_cast<char>(ch); continue; }
            if (position_ >= source_.size()) return false;
            const char escaped = source_[position_++];
            switch (escaped)
            {
            case '"': case '\\': case '/': value += escaped; break;
            case 'b': value += '\b'; break;
            case 'f': value += '\f'; break;
            case 'n': value += '\n'; break;
            case 'r': value += '\r'; break;
            case 't': value += '\t'; break;
            case 'u':
                for (int index = 0; index < 4; ++index)
                {
                    if (position_ >= source_.size() || !std::isxdigit(static_cast<unsigned char>(source_[position_++]))) return false;
                }
                value += '?';
                break;
            default: return false;
            }
        }
        return false;
    }

    void SkipSpace()
    {
        while (position_ < source_.size() && std::isspace(static_cast<unsigned char>(source_[position_]))) ++position_;
    }
    char Peek() const { return position_ < source_.size() ? source_[position_] : '\0'; }
    bool Consume(char expected)
    {
        if (Peek() != expected) return false;
        ++position_;
        return true;
    }
    static bool Fail(std::string& error, const char* message) { error = message; return false; }

    const std::string& source_;
    size_t position_ = 0;
};
}

TakaroPluginState::TakaroPluginState(NowFunction now, WallNowFunction wallNow)
    : now_(std::move(now)), wallNow_(std::move(wallNow)), instanceId_(NewBridgeInstanceId()) {}

TakaroPluginState& TakaroState() { return g_state; }

void TakaroPluginState::Configure(std::vector<std::string> exactCommands, std::vector<std::string> commandPrefixes,
    int maxEvents, int maxActionAgeSeconds, int maxPendingActions)
{
    std::lock_guard lock(mutex_);
    exactCommands_ = std::move(exactCommands);
    commandPrefixes_ = std::move(commandPrefixes);
    maxEvents_ = std::clamp(maxEvents, 1, 4096);
    maxActionAgeSeconds_ = std::clamp(maxActionAgeSeconds, 1, 600);
    maxPendingActions_ = std::clamp(maxPendingActions, 1, 1024);
    maxRetainedActions_ = std::clamp(maxPendingActions_ * 2, 2, 2048);
    acceptingActions_ = true;
    while (events_.size() > static_cast<size_t>(maxEvents_)) events_.pop_front();
}

void TakaroPluginState::SetGameOperations(ITakaroGameOperations* operations)
{
    std::lock_guard lock(mutex_);
    operations_ = operations;
}

void TakaroPluginState::SetWorldPhase(TakaroWorldPhase phase, std::string worldName)
{
    std::lock_guard lock(mutex_);
    worldPhase_ = phase;
    worldName_ = std::move(worldName);
    if (worldPhase_ != TakaroWorldPhase::Gameplay) ready_ = false;
}

TakaroWorldPhase TakaroPluginState::WorldPhase() const
{
    std::lock_guard lock(mutex_);
    return worldPhase_;
}

void TakaroPluginState::SetReady(bool ready)
{
    std::lock_guard lock(mutex_);
    ready_ = ready && worldPhase_ == TakaroWorldPhase::Gameplay;
}

void TakaroPluginState::BeginShutdown()
{
    std::lock_guard lock(mutex_);
    acceptingActions_ = false;
    ready_ = false;
    ++generation_;
    for (auto& [_, action] : actions_)
    {
        if (action.state == "pending" || action.state == "processing")
            FailActionLocked(action, "plugin is shutting down");
    }
}

void TakaroPluginState::Reset()
{
    std::lock_guard lock(mutex_);
    ++generation_;
    acceptingActions_ = false;
    ready_ = false;
    worldPhase_ = TakaroWorldPhase::Unknown;
    worldName_.clear();
    operations_ = nullptr;
    remembered_.clear();
    players_.clear();
    endedSessionGameIds_.clear();
    events_.clear();
    actions_.clear();
    nextEventId_ = 1;
    nextActionId_ = 1;
    instanceId_ = NewBridgeInstanceId();
}

std::string TakaroPluginState::HealthJson() const
{
    std::lock_guard lock(mutex_);
    const char* reason = ready_ ? nullptr : TakaroWorldPhaseNotReadyReason(worldPhase_);
    return std::string("{\"reachable\":") + (ready_ ? "true" : "false") +
        ",\"phase\":\"" + TakaroWorldPhaseName(worldPhase_) + "\"" +
        ",\"world\":" + (worldName_.empty() ? "null" : "\"" + JsonEscape(worldName_) + "\"") +
        ",\"reason\":" + (reason ? "\"" + JsonEscape(reason) + "\"" : "null") +
        ",\"players\":" + std::to_string(players_.size()) + "}";
}

std::string TakaroPluginState::PlayersJson() const
{
    std::lock_guard lock(mutex_);
    std::ostringstream json;
    json << "{\"players\":[";
    bool first = true;
    for (const auto& [_, player] : players_)
    {
        if (!first) json << ",";
        first = false;
        json << PlayerJson(player, true, true);
    }
    json << "]}";
    return json.str();
}

std::string TakaroPluginState::EventsJson(uint64_t after, size_t limit) const
{
    std::lock_guard lock(mutex_);
    const auto boundedLimit = std::min(limit, static_cast<size_t>(maxEvents_));
    uint64_t cursor = after;
    size_t emitted = 0;
    std::ostringstream body;
    body << "[";
    bool first = true;
    for (const auto& [id, event] : events_)
    {
        if (id <= after || emitted >= boundedLimit) continue;
        cursor = id;
        if (!first) body << ",";
        first = false;
        ++emitted;
        body << "{\"id\":\"" << id << "\",\"type\":\"" << JsonEscape(event.first) << "\",\"data\":" << event.second << "}";
    }
    body << "]";
    return "{\"instanceId\":\"" + JsonEscape(instanceId_) + "\",\"cursor\":\"" +
        std::to_string(cursor) + "\",\"events\":" + body.str() + "}";
}

std::string TakaroPluginState::ReachabilityJson() const
{
    std::lock_guard lock(mutex_);
    if (ready_) return "{\"connectable\":true}";
    const char* reason = TakaroWorldPhaseNotReadyReason(worldPhase_);
    return std::string("{\"connectable\":false,\"reason\":\"") +
        JsonEscape(reason ? reason : "StarRupture world is not ready.") + "\"}";
}

std::string TakaroPluginState::PlayerListJson() const
{
    std::lock_guard lock(mutex_);
    std::string json = "[";
    for (const auto& [_, player] : players_)
    {
        if (json.size() > 1) json += ",";
        json += PlayerJson(player, false);
    }
    return json + "]";
}

std::string TakaroPluginState::PlayerJsonFor(const std::string& gameId) const
{
    std::lock_guard lock(mutex_);
    const auto found = players_.find(gameId);
    return found == players_.end() ? "null" : PlayerJson(found->second, false);
}

std::string TakaroPluginState::LocationJsonFor(const std::string& gameId) const
{
    std::lock_guard lock(mutex_);
    const auto found = players_.find(gameId);
    if (found != players_.end() && found->second.hasLocation)
        return PositionJson(TakaroPositionDto{found->second.x, found->second.y, found->second.z, std::nullopt});
    // Takaro asks for the position right after a disconnect, when the player has already left.
    const auto remembered = remembered_.find(gameId);
    if (remembered == remembered_.end() || !remembered->second.position ||
        now_() - remembered->second.seenAt > std::chrono::seconds(60))
        return "null";
    return PositionJson(*remembered->second.position);
}

std::string TakaroPluginState::ProgressionJsonFor(const std::string& gameId) const
{
    std::lock_guard lock(mutex_);
    const auto online = players_.find(gameId);
    const std::vector<TakaroSkillReading>* skills = nullptr;
    if (online != players_.end() && online->second.skills) skills = &*online->second.skills;
    const auto remembered = remembered_.find(gameId);
    if (!skills && remembered != remembered_.end() && remembered->second.skills)
        skills = &*remembered->second.skills;
    // A player whose skills are not loaded yet is unknown, never level 0.
    if (!skills) return "null";
    std::string json = "[";
    for (const auto& reading : *skills)
    {
        if (json.size() > 1) json += ",";
        std::string name = reading.skill;
        if (!name.empty()) name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
        char experience[64];
        std::snprintf(experience, sizeof experience, "%.3f", reading.experience);
        json += "{\"key\":\"skill." + JsonEscape(reading.skill) + "\",\"name\":\"" + JsonEscape(name) +
            "\",\"level\":" + std::to_string(reading.level) + ",\"experience\":" + experience + "}";
    }
    return json + "]";
}

std::vector<TakaroQueuedEvent> TakaroPluginState::EventsAfter(
    uint64_t after, size_t limit, std::string& instanceId) const
{
    std::lock_guard lock(mutex_);
    instanceId = instanceId_;
    std::vector<TakaroQueuedEvent> events;
    for (const auto& [id, event] : events_)
    {
        if (id <= after) continue;
        if (events.size() >= limit) break;
        events.push_back({id, event.first, event.second});
    }
    return events;
}

TakaroActionOutcome TakaroPluginState::ActionOutcome(const std::string& actionId) const
{
    std::lock_guard lock(mutex_);
    const auto found = actions_.find(actionId);
    if (found == actions_.end()) return {"missing", "", "unknown action"};
    const auto& action = found->second;
    return {action.state, action.state == "completed" && action.result ? ResultJson(*action.result) : "", action.error};
}

void TakaroPluginState::RememberLocked(const TakaroPlayerSnapshot& player)
{
    if (remembered_.size() >= 4096) remembered_.clear();
    auto& remembered = remembered_[player.gameId];
    remembered.seenAt = now_();
    if (player.hasLocation) remembered.position = TakaroPositionDto{player.x, player.y, player.z, std::nullopt};
    if (player.skills) remembered.skills = player.skills;
}

std::string TakaroPluginState::CreateAction(const std::string& action, const std::string& argsJson, int64_t deadlineUnixMs)
{
    std::lock_guard lock(mutex_);
    TrimActionResultsLocked(now_());
    if (!acceptingActions_) return "{\"error\":\"plugin is shutting down\"}";
    const auto wallNow = wallNow_();
    const auto callerDeadline = std::chrono::system_clock::time_point(std::chrono::milliseconds(deadlineUnixMs));
    if (callerDeadline <= wallNow) return "{\"error\":\"action deadline expired\"}";
    const auto type = ParseTakaroOperationType(action);
    if (!type) return "{\"error\":\"unknown action\"}";
    const auto pending = std::count_if(actions_.begin(), actions_.end(), [](const auto& entry) {
        return entry.second.state == "pending" || entry.second.state == "processing";
    });
    if (pending >= maxPendingActions_) return "{\"error\":\"action queue full\"}";
    const auto id = instanceId_ + "-" + std::to_string(nextActionId_++);
    const auto workToken = nextWorkToken_++;
    const auto now = now_();
    const auto configuredDeadline = wallNow + std::chrono::seconds(maxActionAgeSeconds_);
    const auto effectiveDeadline = std::min(callerDeadline, configuredDeadline);
    actions_.emplace(id, TakaroActionRecord{
        id, workToken, generation_, *type, action, argsJson.empty() ? "{}" : argsJson, "pending", std::nullopt, "", now, now,
        effectiveDeadline, 0, ""});
    return "{\"actionId\":\"" + id + "\"}";
}

std::string TakaroPluginState::ActionJson(const std::string& actionId) const
{
    std::lock_guard lock(mutex_);
    const auto it = actions_.find(actionId);
    if (it == actions_.end()) return "{\"state\":\"missing\",\"error\":\"unknown actionId\"}";
    const auto& action = it->second;
    std::ostringstream json;
    json << "{\"state\":\"" << action.state << "\"";
    if (action.state == "completed" && action.result) json << ",\"result\":" << ResultJson(*action.result);
    if (action.state == "failed") json << ",\"error\":\"" << JsonEscape(action.error) << "\"";
    json << "}";
    return json.str();
}

std::optional<TakaroCompletedActionIdentity> TakaroPluginState::CompletedActionIdentity(
    const std::string& actionId) const
{
    std::lock_guard lock(mutex_);
    const auto found = actions_.find(actionId);
    if (found == actions_.end() || found->second.state != "completed" || !found->second.result)
        return std::nullopt;
    return TakaroCompletedActionIdentity{
        found->second.type, found->second.workToken, found->second.generation};
}

void TakaroPluginState::ProcessPendingActions()
{
    struct Work
    {
        std::string id;
        uint64_t generation;
        TakaroOperationRequest request;
        ITakaroGameOperations* operations;
    };
    std::vector<Work> work;
    const auto now = now_();
    {
        std::lock_guard lock(mutex_);
        for (auto& [id, record] : actions_)
        {
            if (record.state != "pending") continue;
            if (now - record.createdAt > std::chrono::seconds(maxActionAgeSeconds_) || wallNow_() >= record.deadlineAt)
            {
                FailActionLocked(record, record.pendingReason.empty()
                    ? std::string("action expired before game-thread processing")
                    : record.pendingReason);
                continue;
            }
            record.state = "processing";
            auto request = TakaroOperationRequest{
                record.type, record.action, record.argsJson, record.workToken,
                std::chrono::duration_cast<std::chrono::milliseconds>(record.deadlineAt.time_since_epoch()).count(),
                record.operationAttempts};
            request.generation = record.generation;
            work.push_back({id, record.generation, std::move(request), operations_});
        }
    }

    for (const auto& item : work)
    {
        TakaroOperationResult result = TakaroOperationFailure{"game operation failed"};
        if (wallNow_() >= std::chrono::system_clock::time_point(std::chrono::milliseconds(item.request.deadlineUnixMs)))
        {
            result = TakaroOperationFailure{"action expired before game operation execution"};
        }
        else if (!item.operations)
        {
            result = TakaroOperationFailure{"game operation is unsupported by the pinned SDK bridge"};
        }
        else if (item.request.type == TakaroOperationType::ExecuteConsoleCommand)
        {
            const auto command = ExtractJsonString(item.request.argsJson, "command").value_or("");
            bool allowed = false;
            { std::lock_guard lock(mutex_); allowed = IsCommandAllowedLocked(command); }
            result = allowed ? item.operations->Execute(item.request)
                             : TakaroOperationResult{TakaroOperationFailure{"command blocked by plugin allowlist"}};
        }
        else
        {
            result = item.operations->Execute(item.request);
        }

        std::lock_guard lock(mutex_);
        const auto found = actions_.find(item.id);
        if (found == actions_.end() || found->second.state != "processing" ||
            found->second.workToken != item.request.workToken || found->second.generation != item.generation) continue;
        if (now_() - found->second.createdAt > std::chrono::seconds(maxActionAgeSeconds_) ||
            wallNow_() >= found->second.deadlineAt)
        {
            FailActionLocked(found->second, "action timed out during game-thread processing");
            continue;
        }
        if (const auto* success = std::get_if<TakaroOperationSuccess>(&result))
        {
            CompleteActionLocked(found->second, *success);
        }
        else if (const auto* failure = std::get_if<TakaroOperationFailure>(&result))
        {
            FailActionLocked(found->second, failure->error);
        }
        else
        {
            found->second.state = "pending";
            found->second.pendingReason = std::get<TakaroOperationDeferred>(result).pendingReason;
            ++found->second.operationAttempts;
            found->second.updatedAt = now_();
        }
    }

    std::lock_guard lock(mutex_);
    TrimActionResultsLocked(now_());
}

void TakaroPluginState::AddLogEvent(const std::string& level, const std::string& message)
{
    // No events are exposed until their capability has dedicated-server and Takaro-visible proof.
    (void)level;
    (void)message;
}

void TakaroPluginState::AddPlayerConnected(const TakaroPlayerSnapshot& player)
{
    if (!ValidPlayer(player)) return;
    std::lock_guard lock(mutex_);
    players_[player.gameId] = player;
    RememberLocked(player);
    endedSessionGameIds_.erase(player.gameId);
    AddEventLocked("player-connected", "{\"player\":" + PlayerJson(player, false) + "}");
}

void TakaroPluginState::AddPlayerDisconnected(const TakaroPlayerSnapshot& player)
{
    if (!ValidPlayer(player)) return;
    std::lock_guard lock(mutex_);
    players_.erase(player.gameId);
    AddEventLocked("player-disconnected", "{\"player\":" + PlayerJson(player, false) + "}");
}

std::vector<TakaroPlayerSnapshot> TakaroPluginState::EndAllPlayerSessions(const std::string& reason)
{
    std::lock_guard lock(mutex_);
    std::vector<TakaroPlayerSnapshot> ended;
    for (const auto& [gameId, player] : players_)
    {
        if (endedSessionGameIds_.count(gameId)) continue;
        AddEventLocked("player-disconnected",
            "{\"player\":" + PlayerJson(player, false) + ",\"msg\":\"" + JsonEscape(reason) + "\"}");
        endedSessionGameIds_.insert(gameId);
        ended.push_back(player);
    }
    players_.clear();
    return ended;
}

void TakaroPluginState::AddChatMessage(const TakaroPlayerSnapshot& player, const std::string& message)
{
    if (!ValidPlayer(player) || message.empty()) return;
    std::lock_guard lock(mutex_);
    AddEventLocked("chat-message", "{\"player\":" + PlayerJson(player, false) +
        ",\"msg\":\"" + JsonEscape(message) + "\",\"channel\":\"global\"}");
}

void TakaroPluginState::AddPlayerDeath(
    const TakaroPlayerSnapshot& player,
    const std::optional<TakaroPlayerSnapshot>& attacker,
    const std::optional<TakaroPositionDto>& position)
{
    if (!ValidPlayer(player) || (attacker && !ValidPlayer(*attacker)) ||
        (position && !ValidPosition(*position))) return;
    std::string data = "{\"player\":" + PlayerJson(player, false);
    if (attacker) data += ",\"attacker\":" + PlayerJson(*attacker, false);
    if (position) data += ",\"position\":" + PositionJson(*position);
    std::lock_guard lock(mutex_);
    AddEventLocked("player-death", data + "}");
}

void TakaroPluginState::AddEntityKilled(
    const TakaroPlayerSnapshot& player, const std::string& entity, const std::string& weapon)
{
    if (!ValidPlayer(player) || LowerTrimmed(entity).empty() || LowerTrimmed(weapon).empty()) return;
    std::lock_guard lock(mutex_);
    AddEventLocked("entity-killed", "{\"player\":" + PlayerJson(player, false) +
        ",\"entity\":\"" + JsonEscape(entity) + "\",\"weapon\":\"" + JsonEscape(weapon) + "\"}");
}

void TakaroPluginState::UpsertPlayer(const TakaroPlayerSnapshot& player)
{
    if (!ValidPlayer(player)) return;
    std::lock_guard lock(mutex_);
    players_[player.gameId] = player;
    RememberLocked(player);
}

void TakaroPluginState::RemovePlayer(const std::string& gameId)
{
    std::lock_guard lock(mutex_);
    players_.erase(gameId);
}

void TakaroPluginState::ReplacePlayers(const std::vector<TakaroPlayerSnapshot>& players)
{
    std::map<std::string, TakaroPlayerSnapshot> replacement;
    for (const auto& player : players) if (ValidPlayer(player)) replacement[player.gameId] = player;
    std::lock_guard lock(mutex_);
    players_ = std::move(replacement);
    for (const auto& [_, player] : players_) RememberLocked(player);
}

void TakaroPluginState::ClearWorldState()
{
    std::lock_guard lock(mutex_);
    ++generation_;
    ready_ = false;
    worldPhase_ = TakaroWorldPhase::Unknown;
    worldName_.clear();
    players_.clear();
    endedSessionGameIds_.clear();
    for (auto& [_, action] : actions_)
    {
        if (action.state == "pending" || action.state == "processing")
            FailActionLocked(action, "world reset before game-thread processing");
    }
}

size_t TakaroPluginState::RetainedActionCount() const
{
    std::lock_guard lock(mutex_);
    return actions_.size();
}

bool TakaroPluginState::IsCommandAllowedLocked(const std::string& command) const
{
    const auto normalized = LowerTrimmed(command);
    if (normalized.empty()) return false;
    for (const auto& exact : exactCommands_) if (LowerTrimmed(exact) == normalized) return true;
    for (const auto& prefix : commandPrefixes_)
    {
        const auto normalizedPrefix = LowerTrimmed(prefix);
        if (!normalizedPrefix.empty() && normalized.rfind(normalizedPrefix, 0) == 0) return true;
    }
    return false;
}

void TakaroPluginState::CompleteActionLocked(TakaroActionRecord& record, const TakaroOperationSuccess& result)
{
    if (result.type != record.type || !ExpectedResultShape(record.type, result.value))
    {
        FailActionLocked(record, "game operation returned an invalid typed result");
        return;
    }
    record.state = "completed";
    record.result = result;
    record.updatedAt = now_();
}

void TakaroPluginState::FailActionLocked(TakaroActionRecord& record, const std::string& error)
{
    record.state = "failed";
    record.error = error;
    record.updatedAt = now_();
}

void TakaroPluginState::TrimActionResultsLocked(TimePoint now)
{
    const auto cutoff = now - std::chrono::seconds(maxActionAgeSeconds_);
    for (auto it = actions_.begin(); it != actions_.end();)
    {
        if ((it->second.state == "completed" || it->second.state == "failed") && it->second.updatedAt < cutoff) it = actions_.erase(it);
        else ++it;
    }
    while (actions_.size() > static_cast<size_t>(maxRetainedActions_))
    {
        auto oldest = actions_.end();
        for (auto it = actions_.begin(); it != actions_.end(); ++it)
        {
            if (it->second.state != "completed" && it->second.state != "failed") continue;
            if (oldest == actions_.end() || it->second.updatedAt < oldest->second.updatedAt) oldest = it;
        }
        if (oldest == actions_.end()) break;
        actions_.erase(oldest);
    }
}

void TakaroPluginState::AddEventLocked(const std::string& type, const std::string& dataJson)
{
    events_.push_back({nextEventId_++, {type, dataJson}});
    while (events_.size() > static_cast<size_t>(maxEvents_)) events_.pop_front();
}

std::string JsonEscape(const std::string& value)
{
    std::ostringstream escaped;
    for (const unsigned char ch : value)
    {
        switch (ch)
        {
        case '\\': escaped << "\\\\"; break;
        case '"': escaped << "\\\""; break;
        case '\n': escaped << "\\n"; break;
        case '\r': escaped << "\\r"; break;
        case '\t': escaped << "\\t"; break;
        default:
            if (ch < 0x20) escaped << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(ch) << std::dec;
            else escaped << static_cast<char>(ch);
        }
    }
    return escaped.str();
}

std::optional<std::string> ExtractJsonString(const std::string& json, const std::string& key)
{
    JsonReader reader(json);
    return reader.ParseTopLevelString(key);
}

std::optional<std::string> ExtractJsonObject(const std::string& json, const std::string& key)
{
    JsonReader reader(json);
    return reader.ParseTopLevelObject(key);
}

std::optional<int64_t> ExtractJsonPositiveInteger(const std::string& json, const std::string& key)
{
    JsonReader reader(json);
    return reader.ParseTopLevelPositiveInteger(key);
}

std::optional<double> ExtractJsonFiniteNumber(const std::string& json, const std::string& key)
{
    JsonReader reader(json);
    return reader.ParseTopLevelFiniteNumber(key);
}

bool ParseActionRequest(const std::string& json, std::string& action, std::string& argsJson,
    int64_t& deadlineUnixMs, std::string& error)
{
    JsonReader reader(json);
    return reader.ParseActionEnvelope(action, argsJson, deadlineUnixMs, error);
}

const std::array<TakaroOperationType, 19>& AllTakaroOperationTypes()
{
    static const std::array<TakaroOperationType, 19> types = [] {
        std::array<TakaroOperationType, 19> result{};
        for (size_t index = 0; index < kOperationNames.size(); ++index) result[index] = kOperationNames[index].first;
        return result;
    }();
    return types;
}

std::string TakaroOperationName(TakaroOperationType type)
{
    const auto found = std::find_if(kOperationNames.begin(), kOperationNames.end(),
        [type](const auto& entry) { return entry.first == type; });
    return found == kOperationNames.end() ? "" : found->second;
}

std::optional<TakaroOperationType> ParseTakaroOperationType(const std::string& action)
{
    const auto found = std::find_if(kOperationNames.begin(), kOperationNames.end(),
        [&action](const auto& entry) { return action == entry.second; });
    return found == kOperationNames.end() ? std::nullopt : std::optional<TakaroOperationType>(found->first);
}
