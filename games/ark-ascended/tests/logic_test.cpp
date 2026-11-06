// Rules and durable state that sit next to the game: console refusals, timestamps, log filtering, item codes,
#include <unistd.h>
// catalogue search, and that bans and known players survive a restart.
#include "../native/src/logic.h"

#include <cmath>
#include <cstdio>
#include <filesystem>

using namespace takaro;
namespace fs = std::filesystem;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

static tj::Value Args(const char* text) { return *tj::parse(text); }

static void test_progression() {
    CHECK(!ProgressBetween(100, 200, 200));
    CHECK(!ProgressBetween(100, 300, 200));
    CHECK(!ProgressBetween(100, std::nan(""), 200));
    CHECK(*ProgressBetween(150, 100, 200) == 0.5);
    CHECK(*ProgressBetween(500, 100, 200) == 1.0);  // XP past the threshold until the point is spent
    CHECK(*ProgressBetween(50, 100, 200) == 0.0);

    CHECK(!MakeProgressReading(0, 10, 0, 20));
    CHECK(!MakeProgressReading(100001, 10, 0, 20));
    CHECK(!MakeProgressReading(5, -1, 0, 20));
    CHECK(!MakeProgressReading(5, std::nan(""), 0, 20));
    CHECK(!MakeProgressReading(5, INFINITY, 0, 20));
    const auto capped = MakeProgressReading(105, 1000, 900, 900);
    CHECK(capped && capped->level == 105 && !capped->progress);

    CHECK(ProgressionJson(false, std::nullopt).is_null());
    const tj::Value none = ProgressionJson(true, std::nullopt);
    CHECK(none.is_array() && none.as_array().empty());

    const tj::Value tracks = ProgressionJson(true, MakeProgressReading(12, 150, 100, 200));
    CHECK(tracks.is_array() && tracks.as_array().size() == 1);
    const tj::Value& track = tracks.as_array()[0];
    CHECK(track.get("key")->as_string() == "character");
    CHECK(track.get("level")->as_number() == 12);
    CHECK(track.get("experience")->as_number() == 150);
    CHECK(track.get("progress")->as_number() == 0.5);
    const tj::Value atCap = ProgressionJson(true, capped);
    CHECK(atCap.as_array()[0].get("progress") == nullptr);
}

static void test_iso() {
    int64_t t = 0;
    CHECK(ParseIso8601("1970-01-01T00:00:00Z", t) && t == 0);
    CHECK(ParseIso8601("2026-11-06T04:00:00.123Z", t) && t == 1793937600 + 0);
    CHECK(ParseIso8601("2026-11-06T05:00:00+01:00", t) && t == 1793937600);
    CHECK(ParseIso8601("2026-11-06T03:00:00-01:00", t) && t == 1793937600);
    for (const char* bad : {"", "tomorrow", "2026-11-06", "2026-11-06T04:00:00", "2026-13-06T04:00:00Z", "2026-11-06T04:00:00Zjunk", "2026-11-06T04:00:00.Z"})
        CHECK(!ParseIso8601(bad, t));
}

static void test_console() {
    CHECK(ConsoleRefusal("ListPlayers").empty());
    CHECK(ConsoleRefusal("cheat SetTimeOfDay 12:00").empty());
    CHECK(ConsoleRefusal("  Broadcast exit now ").empty());
    for (const char* stop : {"DoExit", "doexit", " quit", "exit", "cheat DoExit", "admincheat   QUIT now"})
        CHECK(ConsoleRefusal(stop) == "Use the dedicated shutdown action");
    CHECK(!ConsoleRefusal("debug crash").empty());
    CHECK(!ConsoleRefusal("").empty());
    CHECK(!ConsoleRefusal("   ").empty());
    CHECK(!ConsoleRefusal("cheat").empty());
    CHECK(!ConsoleRefusal("say a\nDoExit").empty());
    CHECK(!ConsoleRefusal(std::string(1025, 'a')).empty());
}

static void test_log_filter() {
    CHECK(LogLineAllowed("Server: Tribe Example was destroyed"));
    CHECK(!LogLineAllowed(""));
    CHECK(!LogLineAllowed("   "));
    CHECK(!LogLineAllowed("ServerAdminPassword=abc"));
    CHECK(!LogLineAllowed("connect from 10.0.0.5:7777"));
    CHECK(!LogLineAllowed("Commandline: TheIsland_WP?listen"));
    CHECK(!LogLineAllowed("RCON client connected"));
    CHECK(!LogLineAllowed("line\twith tab"));
    CHECK(!LogLineAllowed(std::string(513, 'a')));
    CHECK(LogLineAllowed("ARK Version: 96.9"));
}

static void test_item_code() {
    const char* quoted = "Blueprint'/Game/PrimalEarth/CoreBlueprints/Resources/PrimalItemResource_Wood.PrimalItemResource_Wood'";
    CHECK(ItemCodeOf(Args("{\"item\":\"/Game/A/B.B\"}")) == "Blueprint'/Game/A/B.B'");
    CHECK(ItemCodeOf(tj::obj({{"item", tj::str(quoted)}})) == quoted);
    CHECK(ItemCodeOf(Args("{\"itemCode\":\"/Game/A/B.B\"}")) == "Blueprint'/Game/A/B.B'");
    CHECK(ItemCodeOf(Args("{\"item\":{\"code\":\"/Game/A/B.B\"}}")) == "Blueprint'/Game/A/B.B'");
    CHECK(ItemCodeOf(Args("{\"item\":null,\"name\":\"/Game/A/B.B\"}")) == "Blueprint'/Game/A/B.B'");
    CHECK(ItemCodeOf(Args("{}")).empty());
    CHECK(ItemCodeOf(Args("{\"item\":\"wood\"}")).empty());
    CHECK(ItemCodeOf(Args("{\"item\":\"Blueprint'/Game/A/B.B\"}")).empty());
    CHECK(ItemCodeOf(Args("{\"item\":\"/Game/A B\"}")).empty());
    CHECK(ItemCodeOf(Args("{\"item\":\"/Game/A';DoExit\"}")).empty());
}

static void test_catalog() {
    const std::vector<CatalogRow> rows{{"/Game/Wood.Wood", "Wood"}, {"/Game/Stone.Stone", "Stone"}, {"/Game/Pick.Pick", "Stone Pick"}};
    CHECK(CatalogJson(rows).as_array().size() == 3);
    CHECK(CatalogSearch(rows, " STONE ").as_array().size() == 2);
    CHECK(CatalogSearch(rows, "game/wood").as_array().size() == 1);
    CHECK(CatalogSearch(rows, "nope").as_array().empty());
}

static void test_cosmetics() {
    CHECK(IsCosmeticCode("Blueprint'/Game/Packs/AAA/Skins/PrimalItemCostume_AAA_Achatina.PrimalItemCostume_AAA_Achatina'"));
    CHECK(!IsCosmeticCode("Blueprint'/Game/PrimalEarth/CoreBlueprints/Weapons/PrimalItemAmmo_SimpleBullet.PrimalItemAmmo_SimpleBullet'"));
}

static void test_ban_list() {
    const auto rows = ParseBanList("00029a9c,Limon,0\r\n\n  \nabc\nxyz,Two Words,15\n");
    CHECK(rows.size() == 3);
    CHECK(rows[0].id == "00029a9c" && rows[0].name == "Limon");
    CHECK(rows[1].id == "abc" && rows[1].name.empty());
    CHECK(rows[2].id == "xyz" && rows[2].name == "Two Words");
    CHECK(ParseBanList("").empty());
}

static void test_persistence(const fs::path& dir) {
    const fs::path bans = dir / "state" / "bans.json", known = dir / "state" / "known-players.json";
    {
        BanStore store(bans);
        Ban timed;
        timed.reason = "cheating \"hard\"";
        timed.expiresAt = "2026-11-06T04:00:00Z";
        CHECK(ParseIso8601(timed.expiresAt, timed.expiresEpoch));
        store.Set("a", timed);
        store.Set("b", Ban{"forever", "", 0});
        store.Set("c", Ban{"gone", "", 0});
        store.Erase("c");
        KnownPlayers players(known);
        CHECK(players.Remember("a", "Alice"));
        CHECK(!players.Remember("a", "Alice"));
        CHECK(players.Remember("a", "Alice2"));
        CHECK(!players.Remember("", "x"));
        CHECK(!players.Remember("b", ""));
    }
    BanStore store(bans);
    const auto rows = store.Rows();
    CHECK(rows.size() == 2);
    CHECK(rows.at("a").reason == "cheating \"hard\"");
    CHECK(rows.at("b").expiresAt.empty());
    CHECK(store.Expired(rows.at("a").expiresEpoch - 1).empty());
    const auto expired = store.Expired(rows.at("a").expiresEpoch);
    CHECK(expired.size() == 1 && expired[0] == "a");
    CHECK(KnownPlayers(known).Name("a") == "Alice2");
    CHECK(KnownPlayers(known).Name("nobody").empty());
    CHECK(!fs::exists(fs::path(bans) += ".tmp"));
}

static void test_unwritable_ban_store(const fs::path& dir) {
    // A directory where the file belongs: the temporary file writes but cannot replace it.
    const fs::path blocked = dir / "blocked.json";
    fs::create_directories(blocked / "child");
    BanStore store(blocked);
    CHECK(!store.Set("a", Ban{"x", "", 0}));
    CHECK(store.Rows().empty());
}

static void test_corrupt_files(const fs::path& dir) {
    const fs::path file = dir / "broken.json";
    WriteFileAtomic(file, "{not json");
    CHECK(BanStore(file).Rows().empty());
    CHECK(KnownPlayers(file).Name("a").empty());
}

int main() {
    const fs::path dir = fs::temp_directory_path() / ("logic-test-" + std::to_string(::getpid()));
    test_iso();
    test_progression();
    test_console();
    test_log_filter();
    test_item_code();
    test_catalog();
    test_cosmetics();
    test_ban_list();
    test_persistence(dir);
    test_unwritable_ban_store(dir);
    test_corrupt_files(dir);
    fs::remove_all(dir);
    if (failures) { std::printf("%d check(s) failed\n", failures); return 1; }
    std::printf("logic tests passed\n");
    return 0;
}
