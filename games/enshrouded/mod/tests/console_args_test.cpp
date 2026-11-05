// Console argument parsing (console_args.cpp): quoted and bare player names with spaces, SteamID64,
// and the refusals (unknown, shared name, ambiguous split, unclosed quote). The forms mirror RunCommand.
#include "console_args.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace console;

namespace {

int failures = 0, checks = 0;

void Check(bool ok, const std::string& what) {
    checks++;
    if (!ok) {
        std::printf("  FAIL %s\n", what.c_str());
        failures++;
    }
}

bool IsNum(const std::string& s) {
    char* end = nullptr;
    strtod(s.c_str(), &end);
    return !s.empty() && end && *end == 0;
}

const std::vector<Form> kOne = {{1, 0, 0}};  // level, location, inventory, kick
Form Whisper() {
    Form f{1, 1, (size_t)-1};
    f.freeText = true;
    return f;
}
const std::vector<Form> kTeleport = {
    {1, 3, 3, [](const std::vector<Arg>& r) { return IsNum(r[0].text) && IsNum(r[1].text) && IsNum(r[2].text); }},
    {2, 0, 0}};
const std::vector<Form> kGive = {{1, 1, 3, [](const std::vector<Arg>& r) {
                                      return (r.size() < 2 || IsNum(r[1].text)) && (r.size() < 3 || IsNum(r[2].text));
                                  }}};

struct Result {
    bool ok;
    Parsed p;
    std::string err;
};

Result Run(const std::string& args, const std::vector<Form>& forms, const std::vector<Player>& online) {
    Result r;
    r.ok = ResolveArgs(Tokenize(args), forms, online, r.p, r.err);
    return r;
}

bool Has(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

}  // namespace

int main() {
    // ---- tokenizer ----
    {
        auto t = Tokenize("  give \"Character 1\"  'Mr O\\'Neil' Wood\t5 ");
        Check(t.size() == 5, "tokenize count");
        Check(t[1].text == "Character 1" && t[1].quoted && !t[1].unclosed, "double-quoted word");
        Check(t[2].text == "Mr O'Neil" && t[2].quoted, "single-quoted word with escaped quote");
        Check(t[3].text == "Wood" && !t[3].quoted, "bare word");
        Check(t[1].begin == 7 && t[1].end == 20, "quoted word span includes the quotes");
        auto e = Tokenize("say \"a \\\\ b\\\" c\"");
        Check(e.size() == 2 && e[1].text == "a \\ b\" c", "escaped backslash and quote");
        auto apos = Tokenize("say don't \"stop\"");
        Check(apos.size() == 3 && apos[1].text == "don't" && !apos[1].quoted, "apostrophe inside a word is text");
        auto bad = Tokenize("level \"Character 1");
        Check(bad.size() == 2 && bad[1].unclosed && bad[1].text == "Character 1", "unclosed quote runs to the end");
        auto empty = Tokenize("x \"\"");
        Check(empty.size() == 2 && empty[1].text.empty() && empty[1].quoted, "empty quoted word");
        Check(Tokenize("   ").empty(), "blank line");
    }

    const std::vector<Player> online = {
        {"76561198000000001", "SteamAlice", "Character 1"},
        {"76561198000000002", "bob", "Bob"},
        {"76561198000000003", "Carol Persona", "Carol"},
    };

    // ---- quoted name ----
    {
        auto r = Run("\"Character 1\"", kOne, online);
        Check(r.ok && r.p.players[0].steamId == "76561198000000001", "quoted name");
        r = Run("'character 1'", kOne, online);
        Check(r.ok && r.p.players[0].steamId == "76561198000000001", "single-quoted, case-insensitive");
    }
    // ---- bare multi-word name ----
    {
        auto r = Run("Character 1", kOne, online);
        Check(r.ok && r.p.players[0].steamId == "76561198000000001", "bare trailing multi-word name");
        r = Run("Character   1", kOne, online);
        Check(r.ok && r.p.players[0].steamId == "76561198000000001", "repeated spaces collapse");
        r = Run("Carol Persona", kOne, online);
        Check(r.ok && r.p.players[0].steamId == "76561198000000003", "Steam persona with a space");
    }
    // ---- SteamID64 and single words, as before ----
    {
        auto r = Run("76561198000000002", kOne, online);
        Check(r.ok && r.p.players[0].name == "bob", "SteamID64");
        r = Run("BOB", kOne, online);
        Check(r.ok && r.p.players[0].steamId == "76561198000000002", "single-word name, case-insensitive");
        r = Run("SteamAlice", kOne, online);
        Check(r.ok && r.p.players[0].steamId == "76561198000000001", "persona of a player with a character name");
    }
    // ---- unknown ----
    {
        auto r = Run("Character 9", kOne, online);
        Check(!r.ok && r.err == "player not online: Character 9", "unknown name: " + r.err);
        r = Run("765611980000000099", kOne, online);
        Check(!r.ok && Has(r.err, "player not online"), "unknown SteamID64");
        r = Run("Nobody here 1 2 3", kTeleport, online);
        Check(!r.ok && Has(r.err, "no online player matches 'Nobody here 1 2 3'"), "unknown teleport player: " + r.err);
    }
    // ---- unbalanced quote ----
    {
        auto r = Run("\"Character 1", kOne, online);
        Check(!r.ok && Has(r.err, "unclosed quote"), "unclosed quote refused: " + r.err);
        r = Run("\"Character 1\" Wood 'x", kGive, online);
        Check(!r.ok && Has(r.err, "unclosed quote"), "unclosed quote in the rest refused: " + r.err);
        r = Run("Character 1 it's 'fine", {Whisper()}, online);
        Check(r.ok && r.p.players[0].steamId == "76561198000000001" && r.p.rest[0].text == "it's",
              "whisper text may hold an unclosed quote");
    }
    // ---- unclosed quote is caught before the arity check (RunCommand), the same for every command ----
    {
        const std::string want = "unclosed quote; write names with spaces as \"Character 1\"";
        // the quote swallows the item code: one argument, so `give` would fail its arity check first
        auto give = Tokenize("\"Character 1 Material_T1_Stone_Flint 1");
        Check(give.size() == 1 && UnclosedQuoteError(give) == want, "give: unclosed quote reported");
        Check(UnclosedQuoteError(Tokenize("\"Character 1")) == want, "level: unclosed quote reported");
        Check(UnclosedQuoteError(Tokenize("'Character 1 10 20 30")) == want, "teleport: unclosed single quote reported");
        Check(UnclosedQuoteError(Tokenize("\"Character 1\" Wood 5")).empty(), "closed quotes pass");
        Check(UnclosedQuoteError(Tokenize("bob Wood")).empty(), "no quotes pass");
        Check(UnclosedQuoteError(Tokenize("")).empty(), "no arguments pass");
        // say: the whole line is the message; whisper: everything after the player
        Check(UnclosedQuoteError(Tokenize("\"hello there"), 0).empty(), "say text may open a quote");
        Check(UnclosedQuoteError(Tokenize("bob it's \"fine"), 1).empty(), "whisper text may open a quote");
        Check(UnclosedQuoteError(Tokenize("\"Character 1 hello"), 1) == want, "whisper: unclosed quote in the player refused");
        // ResolveArgs gives the same text when called without the pre-check
        auto r = Run("\"Character 1", kOne, online);
        Check(r.err == want, "ResolveArgs uses the same message: " + r.err);
    }
    // ---- console output names a player by character name, the persona until it is known ----
    {
        Check(DisplayName(online[0]) == "Character 1", "display name is the character name");
        Check(DisplayName({"76561198000000009", "Limon", ""}) == "Limon", "display name falls back to the persona");
        auto r = Run("SteamAlice", kOne, online);
        Check(r.ok && DisplayName(r.p.players[0]) == "Character 1", "found by persona, named by character");
        r = Run("Bob Bob", kOne, {{"76561198000000002", "bob", "Bob"}, {"76561198000000004", "Bob", "Bob Bob"}});
        Check(r.ok && DisplayName(r.p.players[0]) == "Bob Bob", "multi-word character name shown whole");
    }
    // ---- several players share a name ----
    {
        auto twins = online;
        twins.push_back({"76561198000000004", "Dave", "Character 1"});
        auto r = Run("Character 1", kOne, twins);
        Check(!r.ok && Has(r.err, "several online players") && Has(r.err, "76561198000000001") &&
                  Has(r.err, "76561198000000004") && Has(r.err, "SteamID64"),
              "shared name refused: " + r.err);
        r = Run("\"Character 1\"", kOne, twins);
        Check(!r.ok && Has(r.err, "several online players"), "shared quoted name refused");
        r = Run("76561198000000004", kOne, twins);
        Check(r.ok && r.p.players[0].name == "Dave", "SteamID64 resolves a shared name");
    }
    // ---- ambiguous split ----
    {
        auto both = online;
        both.push_back({"76561198000000005", "Char", "Character"});
        auto r = Run("Character 1 hello there", {Whisper()}, both);
        Check(!r.ok && Has(r.err, "ambiguous") && Has(r.err, "'Character'") && Has(r.err, "'Character 1'"),
              "whisper split refused: " + r.err);
        r = Run("\"Character 1\" hello there", {Whisper()}, both);
        Check(r.ok && r.p.players[0].steamId == "76561198000000001" && r.p.rest.size() == 2, "quoting settles it");
        r = Run("Character hello", {Whisper()}, both);
        Check(r.ok && r.p.players[0].steamId == "76561198000000005", "no clash when only one reading fits");
        r = Run("Character 1", kOne, both);
        Check(r.ok && r.p.players[0].steamId == "76561198000000001", "whole-line name has one reading");
        // teleport: "Character" + 1 2 3 and "Character 1" + 2 3 ... only the first has three numbers
        r = Run("Character 1 2 3", kTeleport, both);
        Check(r.ok && r.p.form == 0 && r.p.players[0].steamId == "76561198000000005", "teleport coordinates");
    }
    // ---- give ----
    {
        auto r = Run("Character 1 Wood", kGive, online);
        Check(r.ok && r.p.players[0].steamId == "76561198000000001" && r.p.rest.size() == 1 && r.p.rest[0].text == "Wood",
              "give bare name, code only");
        r = Run("Character 1 Wood 5 3", kGive, online);
        Check(r.ok && r.p.rest.size() == 3 && r.p.rest[1].text == "5", "give bare name, amount and level");
        r = Run("\"Character 1\" Wood 5", kGive, online);
        Check(r.ok && r.p.rest.size() == 2, "give quoted name");
        r = Run("76561198000000002 Wood", kGive, online);
        Check(r.ok && r.p.players[0].name == "bob", "give SteamID64");
        r = Run("Character 1", kGive, online);
        Check(!r.ok, "give without an item code");
    }
    // ---- tp player to player ----
    {
        auto r = Run("Character 1 Carol Persona", kTeleport, online);
        Check(r.ok && r.p.form == 1 && r.p.players[0].steamId == "76561198000000001" &&
                  r.p.players[1].steamId == "76561198000000003",
              "tp two bare multi-word names");
        r = Run("\"Character 1\" bob", kTeleport, online);
        Check(r.ok && r.p.form == 1 && r.p.players[1].name == "bob", "tp quoted and bare");
        r = Run("bob 10 20.5 -3", kTeleport, online);
        Check(r.ok && r.p.form == 0 && r.p.rest[1].text == "20.5", "teleport single word to coordinates");
        r = Run("bob Ghost", kTeleport, online);
        Check(!r.ok && r.err == "player not online: Ghost", "tp unknown target: " + r.err);
    }
    // ---- nothing fits the shape ----
    {
        auto r = Run("bob", kTeleport, online);
        Check(!r.ok && r.err.empty(), "wrong shape leaves err empty for the usage");
    }

    std::printf("console_args_test: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
