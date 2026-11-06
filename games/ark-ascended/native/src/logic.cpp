#include "logic.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace takaro {
namespace {

std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string Trimmed(const std::string& s) {
    const char* space = " \t\r\n";
    const auto a = s.find_first_not_of(space);
    if (a == std::string::npos) return {};
    return s.substr(a, s.find_last_not_of(space) - a + 1);
}

int64_t DaysFromCivil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<int64_t>(era) * 146097 + static_cast<int64_t>(doe) - 719468;
}

std::string ReadAll(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

bool HasDottedQuad(const std::string& text) {
    for (size_t i = 0; i < text.size(); ++i) {
        size_t j = i;
        int groups = 0;
        while (groups < 4) {
            size_t digits = 0;
            while (j < text.size() && std::isdigit(static_cast<unsigned char>(text[j])) && digits < 3) { ++j; ++digits; }
            if (digits == 0) break;
            if (++groups == 4) return true;
            if (j >= text.size() || text[j] != '.') break;
            ++j;
        }
    }
    return false;
}

}  // namespace

bool ParseIso8601(const std::string& text, int64_t& epochSeconds) {
    int y, mo, d, h, mi, s, used = 0;
    if (std::sscanf(text.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d%n", &y, &mo, &d, &h, &mi, &s, &used) != 6) return false;
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || s > 60) return false;
    size_t at = static_cast<size_t>(used);
    if (at < text.size() && text[at] == '.') {
        ++at;
        const size_t start = at;
        while (at < text.size() && std::isdigit(static_cast<unsigned char>(text[at]))) ++at;
        if (at == start) return false;
    }
    int64_t offset = 0;
    if (at >= text.size()) return false;
    if (text[at] == 'Z') {
        if (at + 1 != text.size()) return false;
    } else if (text[at] == '+' || text[at] == '-') {
        int oh = 0, om = 0;
        if (std::sscanf(text.c_str() + at + 1, "%2d:%2d", &oh, &om) != 2 || text.size() != at + 6) return false;
        offset = (oh * 3600 + om * 60) * (text[at] == '-' ? -1 : 1);
    } else {
        return false;
    }
    epochSeconds = DaysFromCivil(y, static_cast<unsigned>(mo), static_cast<unsigned>(d)) * 86400 + h * 3600 + mi * 60 + s - offset;
    return true;
}

std::string ConsoleRefusal(const std::string& command) {
    const std::string text = Trimmed(command);
    if (text.empty()) return "command is required";
    if (text.size() > 1024) return "command is longer than 1024 bytes";
    for (unsigned char c : text)
        if (c < 0x20 || c == 0x7f) return "command contains control characters";
    std::string word = Lower(text.substr(0, text.find(' ')));
    if (word == "cheat" || word == "admincheat") {
        const auto next = text.find_first_not_of(' ', word.size());
        if (next == std::string::npos) return "command is required";
        word = Lower(text.substr(next, text.find(' ', next) - next));
    }
    // Exit has its own action, which saves and reports departures first; the engine debug commands crash on purpose.
    for (const char* blocked : {"doexit", "exit", "quit"})
        if (word == blocked) return "Use the dedicated shutdown action";
    if (word == "debug") return "Engine debug commands are not available";
    return {};
}

bool LogLineAllowed(const std::string& line) {
    if (Trimmed(line).empty() || line.size() > 512) return false;
    for (unsigned char c : line)
        if (c < 0x20 || c == 0x7f) return false;
    const std::string lower = Lower(line);
    for (const char* marker : {"commandline", "password", "token", "authorization", "auth=", "rcon", "steam_", "ip address", "?listen"})
        if (lower.find(marker) != std::string::npos) return false;
    return !HasDottedQuad(line);
}

tj::Value CatalogJson(const std::vector<CatalogRow>& rows) {
    tj::Value out = tj::Value::array();
    for (const auto& row : rows) out.push(tj::obj({{"code", tj::str(row.code)}, {"name", tj::str(row.name)}}));
    return out;
}

tj::Value CatalogSearch(const std::vector<CatalogRow>& rows, const std::string& term) {
    const std::string needle = Lower(Trimmed(term));
    tj::Value out = tj::Value::array();
    for (const auto& row : rows)
        if (Lower(row.code).find(needle) != std::string::npos || Lower(row.name).find(needle) != std::string::npos)
            out.push(tj::obj({{"code", tj::str(row.code)}, {"name", tj::str(row.name)}}));
    return out;
}

std::string ItemCodeOf(const tj::Value& args) {
    std::string code;
    const tj::Value* item = args.get("item");
    const tj::Value* nested = item != nullptr && item->is_object() ? item->get("code") : nullptr;
    for (const tj::Value* v : {item, args.get("itemCode"), args.get("code"), args.get("name"), nested}) {
        if (v != nullptr && v->is_string() && !Trimmed(v->as_string()).empty()) { code = Trimmed(v->as_string()); break; }
    }
    if (code.empty()) return {};
    const bool quoteStart = code.rfind("Blueprint'", 0) == 0, quoteEnd = code.back() == '\'';
    if (quoteStart != quoteEnd) return {};
    const std::string path = quoteStart ? code.substr(10, code.size() - 11) : code;
    if (path.rfind("/Game/", 0) != 0 && path.rfind("/Script/", 0) != 0) return {};
    for (unsigned char c : path)
        if (!std::isalnum(c) && c != '/' && c != '_' && c != '.' && c != '-') return {};
    return quoteStart ? code : "Blueprint'" + path + "'";
}

bool IsCosmeticCode(const std::string& code) {
    return code.find("/PrimalItemCostume_") != std::string::npos;
}

std::vector<NativeBan> ParseBanList(const std::string& text) {
    std::vector<NativeBan> out;
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        line = Trimmed(line);
        const auto comma = line.find(',');
        const std::string id = line.substr(0, comma);
        if (id.empty()) continue;
        std::string name;
        if (comma != std::string::npos) name = line.substr(comma + 1, line.find(',', comma + 1) - comma - 1);
        out.push_back({id, name});
    }
    return out;
}

bool WriteFileAtomic(const std::filesystem::path& path, const std::string& text) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::filesystem::path temp = path;
    temp += ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out << text;
        if (!out.flush()) return false;
    }
    std::filesystem::rename(temp, path, ec);
    return !ec;
}

BanStore::BanStore(std::filesystem::path file) : file_(std::move(file)) {
    if (file_.empty()) return;
    const auto parsed = tj::parse(ReadAll(file_));
    if (!parsed || !parsed->is_object()) return;
    for (const auto& [id, row] : parsed->as_object()) {
        Ban ban;
        if (const tj::Value* r = row.get("reason"); r != nullptr && r->is_string()) ban.reason = r->as_string();
        if (const tj::Value* e = row.get("expiresAt"); e != nullptr && e->is_string() && ParseIso8601(e->as_string(), ban.expiresEpoch))
            ban.expiresAt = e->as_string();
        rows_[id] = std::move(ban);
    }
}

bool BanStore::Save() const {
    if (file_.empty()) return true;
    tj::Value out = tj::Value::object();
    for (const auto& [id, ban] : rows_) {
        tj::Value row = tj::obj({{"reason", tj::str(ban.reason)}});
        if (!ban.expiresAt.empty()) row.set("expiresAt", tj::str(ban.expiresAt));
        out.set(id, std::move(row));
    }
    return WriteFileAtomic(file_, tj::dump(out));
}

bool BanStore::Set(const std::string& id, Ban ban) {
    std::lock_guard<std::mutex> lock(mu_);
    const auto old = rows_.find(id);
    const std::optional<Ban> previous = old != rows_.end() ? std::optional<Ban>(old->second) : std::nullopt;
    rows_[id] = std::move(ban);
    if (Save()) return true;
    if (previous) rows_[id] = *previous;
    else rows_.erase(id);
    return false;
}

void BanStore::Erase(const std::string& id) {
    std::lock_guard<std::mutex> lock(mu_);
    if (rows_.erase(id) > 0) Save();
}

std::map<std::string, Ban> BanStore::Rows() const {
    std::lock_guard<std::mutex> lock(mu_);
    return rows_;
}

std::vector<std::string> BanStore::Expired(int64_t now) const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<std::string> out;
    for (const auto& [id, ban] : rows_)
        if (!ban.expiresAt.empty() && ban.expiresEpoch <= now) out.push_back(id);
    return out;
}

KnownPlayers::KnownPlayers(std::filesystem::path file) : file_(std::move(file)) {
    if (file_.empty()) return;
    const auto parsed = tj::parse(ReadAll(file_));
    if (!parsed || !parsed->is_object()) return;
    for (const auto& [id, name] : parsed->as_object())
        if (name.is_string()) names_[id] = name.as_string();
}

bool KnownPlayers::Remember(const std::string& id, const std::string& name) {
    if (id.empty() || name.empty()) return false;
    std::lock_guard<std::mutex> lock(mu_);
    auto [it, added] = names_.try_emplace(id, name);
    if (!added && it->second == name) return false;
    it->second = name;
    if (!file_.empty()) {
        tj::Value out = tj::Value::object();
        for (const auto& [key, value] : names_) out.set(key, tj::str(value));
        WriteFileAtomic(file_, tj::dump(out));
    }
    return true;
}

std::string KnownPlayers::Name(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mu_);
    const auto it = names_.find(id);
    return it == names_.end() ? std::string() : it->second;
}

std::optional<double> ProgressBetween(double experience, double previous, double next) {
    if (!std::isfinite(previous) || !std::isfinite(next) || next <= previous) return std::nullopt;
    const double fraction = std::clamp((experience - previous) / (next - previous), 0.0, 1.0);
    return std::round(fraction * 10000.0) / 10000.0;
}

std::optional<ProgressReading> MakeProgressReading(int level, double experience, double previous, double next) {
    if (level < 1 || level > 100000 || !std::isfinite(experience) || experience < 0) return std::nullopt;
    return ProgressReading{level, std::round(experience * 1000.0) / 1000.0, ProgressBetween(experience, previous, next)};
}

tj::Value ProgressionJson(bool knownPlayer, const std::optional<ProgressReading>& reading) {
    if (!knownPlayer) return tj::Value::null();
    tj::Value tracks = tj::Value::array();
    if (!reading) return tracks;
    tj::Value track = tj::obj({{"key", tj::str("character")}, {"name", tj::str("Level")},
                               {"level", tj::num(reading->level)}, {"experience", tj::num(reading->experience)}});
    if (reading->progress) track.set("progress", tj::num(*reading->progress));
    tracks.push(std::move(track));
    return tracks;
}

std::optional<Survivor> MakeSurvivor(uint64_t playerDataId, const std::string& name, const std::string& accountName) {
    if (playerDataId == 0) return std::nullopt;
    std::string shown = name.empty() ? accountName : name;
    if (shown.empty()) shown = "Survivor";
    return Survivor{std::to_string(playerDataId), shown.substr(0, 255)};
}

tj::Value SurvivorJson(const Survivor& survivor) {
    return tj::obj({{"id", tj::str(survivor.id)}, {"name", tj::str(survivor.name)}});
}

bool IsSurvivorSwitch(const std::optional<Survivor>& known, const Survivor& seen) {
    return known && known->id != seen.id;
}

}  // namespace takaro
