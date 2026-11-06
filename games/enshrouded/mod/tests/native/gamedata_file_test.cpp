// takaro/enshrouded-gamedata.json upgrades (src/gamedata_file.cpp): files earlier plugins wrote pick up the embedded
// sections, edited sections are kept, broken files are left alone.
#include "gamedata_file.h"
#include "testlib.h"

#include <sys/stat.h>
#include <unistd.h>

namespace {

std::string g_embedded;

std::vector<std::string> Logs;
void Sink(const std::string& l) {
    Logs.push_back(l);
    fprintf(stderr, "   | %s\n", l.c_str());
}
bool Logged(const std::string& part) {
    for (auto& l : Logs)
        if (l.find(part) != std::string::npos) return true;
    return false;
}

bool Exists(const std::string& p) { return access(p.c_str(), F_OK) == 0; }
int CountBak(const std::string& dir) {
    std::string cmd = "ls '" + dir + "' | grep -c '\\.bak-' > '" + dir + "/.count'";
    if (system(cmd.c_str()) < 0) return -1;
    return atoi(t::ReadFile(dir + "/.count").c_str());
}

const JsonValue* Layout(const JsonValue& root, const char* build) {
    const JsonValue* b = root.get("builds");
    b = b ? b->get(build) : nullptr;
    return b ? b->get("layout") : nullptr;
}
bool HasLevels(const JsonValue& root) {
    const JsonValue* l = Layout(root, "1273043");
    return l && l->get("attributesComponent") && l->get("attributesLevel") && l->get("attributesExperience") &&
           l->get("attributesExperienceRequired");
}
JsonValue FileRoot(const std::string& path) { return t::J(t::ReadFile(path)); }

void ShippedTableComplete() {
    t::Group("gamedata: every embedded and fixture section is in the shipped table");
    JsonValue e = t::J(g_embedded);
    for (auto& b : e.get("builds")->obj) {
        std::string h = gamedata_file::SectionHash(b.second);
        CHECK_MSG(gamedata_file::KnownShipped(b.first, h),
                  "add {\"" + b.first + "\", \"" + h + "\"} to kShipped in src/gamedata_file.cpp");
    }
    for (const char* f : {"tests/fixtures/gamedata-0.8.1.json", "tests/fixtures/gamedata-1273043-before-levels.json"}) {
        JsonValue old = t::J(t::ReadFile(f));
        for (auto& b : old.get("builds")->obj)
            CHECK(gamedata_file::KnownShipped(b.first, gamedata_file::SectionHash(b.second)));
    }
    // what the plugin writes reads back as the same sections
    JsonValue back = t::J(gamedata_file::Pretty(e));
    for (auto& b : e.get("builds")->obj)
        CHECK_EQ(gamedata_file::SectionHash(*back.get("builds")->get(b.first)), gamedata_file::SectionHash(b.second));
    // key order and whitespace do not make a section "edited"
    CHECK_EQ(gamedata_file::SectionHash(t::J("{\"a\": \"1\", \"b\": {\"c\": 2}}")),
             gamedata_file::SectionHash(t::J("{\"b\":{\"c\":2},\"a\":\"1\"}")));
}

void FreshInstall() {
    t::Group("gamedata: fresh install writes the embedded file");
    std::string dir = t::TempDir("gd-fresh"), path = dir + "/takaro/enshrouded-gamedata.json";
    Logs.clear();
    auto r = gamedata_file::Apply(path, g_embedded, Sink);
    CHECK(Exists(path));
    CHECK(t::ReadFile(path) == g_embedded);
    CHECK(!r.fromFile);
    CHECK(HasLevels(r.root));
    CHECK(Logged("gamedata: wrote default "));
    CHECK_EQ(CountBak(dir + "/takaro"), 0);
}

void UpgradeFrom081() {
    t::Group("gamedata: a 0.8.1 file is upgraded, gets 1273043, and is not rewritten again");
    std::string dir = t::TempDir("gd-081"), path = dir + "/enshrouded-gamedata.json";
    std::string old = t::ReadFile("tests/fixtures/gamedata-0.8.1.json");
    t::WriteFile(path, old);
    Logs.clear();
    auto r = gamedata_file::Apply(path, g_embedded, Sink);
    CHECK(r.fromFile && r.write);
    CHECK(Logged("build 1267041: upgraded the section an earlier plugin wrote"));
    CHECK(Logged("build 1273043: added this plugin's section"));
    CHECK(Logged("saved " + path));
    JsonValue disk = FileRoot(path), e = t::J(g_embedded);
    for (auto& b : e.get("builds")->obj)
        CHECK_EQ(gamedata_file::SectionHash(*disk.get("builds")->get(b.first)), gamedata_file::SectionHash(b.second));
    CHECK(HasLevels(disk));
    CHECK(HasLevels(r.root));
    CHECK_EQ(CountBak(dir), 1);
    std::string after = t::ReadFile(path);
    Logs.clear();
    auto r2 = gamedata_file::Apply(path, g_embedded, Sink);
    CHECK(!r2.write);
    CHECK(Logs.empty());
    CHECK(t::ReadFile(path) == after);
    CHECK_EQ(CountBak(dir), 1);
}

void UpgradeBeforeLevels() {
    t::Group("gamedata: a 1273043 file from before character levels gets the attributes* layout");
    std::string dir = t::TempDir("gd-pre"), path = dir + "/enshrouded-gamedata.json";
    std::string old = t::ReadFile("tests/fixtures/gamedata-1273043-before-levels.json");
    CHECK(!HasLevels(t::J(old)));
    t::WriteFile(path, old);
    struct stat before {};
    stat(path.c_str(), &before);
    chmod(path.c_str(), 0640);
    Logs.clear();
    auto r = gamedata_file::Apply(path, g_embedded, Sink);
    CHECK(Logged("build 1273043: upgraded the section an earlier plugin wrote"));
    CHECK(!Logged("build 1267041"));  // already current
    CHECK(HasLevels(FileRoot(path)));
    CHECK(HasLevels(r.root));
    struct stat after {};
    stat(path.c_str(), &after);
    CHECK_EQ((long)after.st_ino, (long)before.st_ino);  // rewritten in place: owner, mode and ACL stay
    CHECK_EQ((int)(after.st_mode & 0777), 0640);
    CHECK_EQ(CountBak(dir), 1);
}

void HandEditedKept() {
    t::Group("gamedata: an edited section is kept and only gets the keys it lacks");
    std::string dir = t::TempDir("gd-edit"), path = dir + "/enshrouded-gamedata.json";
    JsonValue f = t::J(t::ReadFile("tests/fixtures/gamedata-1273043-before-levels.json"));
    // the owner moved one function and one layout value
    JsonValue* sec = nullptr;
    for (auto& b : f.obj)
        if (b.first == "builds")
            for (auto& s : b.second.obj)
                if (s.first == "1273043") sec = &s.second;
    CHECK(sec != nullptr);
    for (auto& g : sec->obj) {
        if (g.first == "functions")
            for (auto& fn : g.second.obj)
                if (fn.first == "logSink") fn.second = t::J("{\"rva\": \"0x123450\"}");
        if (g.first == "layout")
            for (auto& l : g.second.obj)
                if (l.first == "chatOff") l.second = t::J("\"0x68\"");
    }
    t::WriteFile(path, gamedata_file::Pretty(f));
    Logs.clear();
    auto r = gamedata_file::Apply(path, g_embedded, Sink);
    CHECK(Logged("build 1273043: kept your edited section, merged 5 missing key(s) from this plugin: "
                 "layout.itemTables, layout.attributesComponent, layout.attributesLevel, layout.attributesExperience, "
                 "layout.attributesExperienceRequired"));
    JsonValue disk = FileRoot(path);
    const JsonValue* s = disk.get("builds")->get("1273043");
    CHECK_EQ(s->get("functions")->get("logSink")->get("rva")->str, std::string("0x123450"));
    CHECK(s->get("functions")->get("logSink")->get("verify") == nullptr);  // entries are not mixed
    CHECK_EQ(s->get("layout")->get("chatOff")->str, std::string("0x68"));
    CHECK(HasLevels(disk));
    CHECK(HasLevels(r.root));
    std::string after = t::ReadFile(path);
    Logs.clear();
    auto r2 = gamedata_file::Apply(path, g_embedded, Sink);
    CHECK(!r2.write);
    CHECK(Logged("build 1273043: kept your edited section (it differs from this plugin's; nothing to merge)"));
    CHECK(t::ReadFile(path) == after);
    CHECK_EQ(CountBak(dir), 1);
}

void FileOnlySectionKept() {
    t::Group("gamedata: a section only the file has is kept");
    std::string dir = t::TempDir("gd-extra"), path = dir + "/enshrouded-gamedata.json";
    JsonValue e = t::J(g_embedded);
    for (auto& b : e.obj)
        if (b.first == "builds") b.second.obj.emplace_back("9999999", t::J("{\"note\": \"mine\", \"layout\": {\"x\": \"0x1\"}}"));
    std::string text = gamedata_file::Pretty(e);
    t::WriteFile(path, text);
    Logs.clear();
    auto r = gamedata_file::Apply(path, g_embedded, Sink);
    CHECK(!r.write);
    CHECK(Logged("build 9999999: kept the file's own section"));
    CHECK(t::ReadFile(path) == text);
    CHECK(r.root.get("builds")->get("9999999") != nullptr);
    // next to an upgrade, too
    JsonValue old = t::J(t::ReadFile("tests/fixtures/gamedata-0.8.1.json"));
    for (auto& b : old.obj)
        if (b.first == "builds") b.second.obj.emplace_back("9999999", t::J("{\"note\": \"mine\"}"));
    t::WriteFile(path, gamedata_file::Pretty(old));
    gamedata_file::Apply(path, g_embedded, Sink);
    JsonValue disk = FileRoot(path);
    CHECK_EQ(disk.get("builds")->get("9999999")->get("note")->str, std::string("mine"));
    CHECK(HasLevels(disk));
}

void BrokenFileUntouched() {
    t::Group("gamedata: broken JSON is left untouched and the embedded data is used");
    std::string dir = t::TempDir("gd-broken"), path = dir + "/enshrouded-gamedata.json";
    for (std::string bad : {std::string("{ \"builds\": { \"1273043\": { ,,, } "), std::string("[1, 2]"),
                            std::string("{\"schema\": 1}")}) {
        t::WriteFile(path, bad);
        Logs.clear();
        auto r = gamedata_file::Apply(path, g_embedded, Sink);
        CHECK(!r.fromFile && !r.write);
        CHECK(Logged("left it untouched"));
        CHECK(t::ReadFile(path) == bad);
        CHECK(HasLevels(r.root));
        CHECK_EQ(CountBak(dir), 0);
    }
}

void UnchangedNotRewritten() {
    t::Group("gamedata: a current file (also with a BOM) is not rewritten");
    std::string dir = t::TempDir("gd-same"), path = dir + "/enshrouded-gamedata.json";
    t::WriteFile(path, "\xEF\xBB\xBF" + g_embedded);
    struct stat before {};
    stat(path.c_str(), &before);
    Logs.clear();
    auto r = gamedata_file::Apply(path, g_embedded, Sink);
    CHECK(r.fromFile && !r.write);
    CHECK(Logs.empty());
    CHECK(t::ReadFile(path) == "\xEF\xBB\xBF" + g_embedded);
    CHECK_EQ(CountBak(dir), 0);
}

}  // namespace

void RunGamedataFileTests() {
    g_embedded = t::ReadFile("gamedata/enshrouded-gamedata.json");
    if (g_embedded.empty()) {
        CHECK_MSG(false, "cannot read gamedata/enshrouded-gamedata.json");
        return;
    }
    ShippedTableComplete();
    FreshInstall();
    UpgradeFrom081();
    UpgradeBeforeLevels();
    HandEditedKept();
    FileOnlySectionKept();
    BrokenFileUntouched();
    UnchangedNotRewritten();
}
