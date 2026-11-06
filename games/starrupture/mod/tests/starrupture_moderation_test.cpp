#include "starrupture_moderation.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace
{
void Expect(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}

struct TemporaryDirectory
{
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("starrupture-moderation-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    TemporaryDirectory() { std::filesystem::create_directories(path); }
    ~TemporaryDirectory() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
};

TakaroBanDto Ban(const std::string& reason = "testing")
{
    TakaroBanDto ban;
    ban.player.gameId = "stable-player-id";
    ban.player.name = "Player";
    ban.player.platformId = "starrupture:stable";
    ban.reason = reason;
    return ban;
}
}

int main()
{
    try
    {
        using namespace std::chrono;
        Expect(ParseTakaroUtcTimestamp("2028-02-29T12:34:56.789Z").has_value(), "valid UTC timestamp rejected");
        Expect(!ParseTakaroUtcTimestamp("2027-02-29T12:34:56.789Z"), "invalid leap day accepted");
        Expect(!ParseTakaroUtcTimestamp("2028-02-29T12:34:56Z"), "non-normalized timestamp accepted");
        Expect(!ParseTakaroUtcTimestamp("2028-02-29T12:34:60.000Z"), "leap second accepted");

        TemporaryDirectory temp;
        const auto registryPath = temp.path / "TakaroStarRupture.bans";
        auto now = *ParseTakaroUtcTimestamp("2028-01-01T00:00:00.000Z");
        TakaroBanRegistry registry(registryPath, [&] { return now; });
        Expect(registry.Initialize().ok, "missing registry did not initialize empty");
        Expect(registry.Healthy(), "missing registry was marked unhealthy");
        Expect(registry.List().bans.empty(), "missing registry was not empty");

        auto permanent = Ban("first reason");
        Expect(registry.Upsert(permanent).ok, "permanent ban failed");
        Expect(std::filesystem::exists(registryPath), "ban registry was not persisted");
        Expect(!std::filesystem::exists(registryPath.string() + ".tmp"), "atomic-write temporary file leaked");
        auto listed = registry.List();
        Expect(listed.ok && listed.bans.size() == 1, "persisted ban was not listed");
        Expect(listed.bans.front().reason == "first reason", "persisted ban changed");

        Expect(registry.Upsert(Ban("replacement reason")).ok, "idempotent ban replacement failed");
        listed = registry.List();
        Expect(listed.bans.size() == 1 && listed.bans.front().reason == "replacement reason",
            "same-player ban was duplicated instead of replaced");

        TakaroBanRegistry reloaded(registryPath, [&] { return now; });
        Expect(reloaded.Initialize().ok, "valid registry failed to reload");
        listed = reloaded.List();
        Expect(listed.ok && listed.bans.size() == 1, "reloaded registry lost its ban");
        Expect(reloaded.Remove("missing-player").ok, "idempotent unban failed");
        Expect(reloaded.Remove("stable-player-id").ok, "existing unban failed");
        Expect(reloaded.List().bans.empty(), "unban did not remove player");

        auto temporary = Ban("temporary");
        temporary.expiresAt = "2028-01-01T00:00:01.000Z";
        Expect(reloaded.Upsert(temporary).ok, "temporary ban failed");
        now += seconds(2);
        listed = reloaded.List();
        Expect(listed.ok && listed.bans.empty(), "expired ban remained active");

        {
            std::ofstream corrupt(registryPath, std::ios::binary | std::ios::trunc);
            corrupt << "not-a-takaro-ban-registry\nprivate-data-must-survive";
        }
        const std::string corruptBytes = "not-a-takaro-ban-registry\nprivate-data-must-survive";
        TakaroBanRegistry damaged(registryPath, [&] { return now; });
        Expect(!damaged.Initialize().ok && !damaged.Healthy(), "corrupt registry was accepted");
        Expect(!damaged.Upsert(Ban()).ok, "corrupt registry accepted a mutation");
        Expect(!damaged.Remove("stable-player-id").ok, "corrupt registry accepted an unban");
        std::ifstream preserved(registryPath, std::ios::binary);
        const std::string actual((std::istreambuf_iterator<char>(preserved)), std::istreambuf_iterator<char>());
        Expect(actual == corruptBytes, "corrupt registry was overwritten");

        std::cout << "starrupture_moderation_test: passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "starrupture_moderation_test: " << error.what() << "\n";
        return 1;
    }
}
