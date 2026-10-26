// Argument parsing for the plugin's console commands (RunCommand in http.cpp).
//
// Enshrouded's default character names contain spaces ("Character 1"), so a player argument can
// span several words. A name is given either quoted ("Character 1" or 'Character 1') or bare, in
// which case every way of splitting the words into player(s) and the remaining arguments is tried
// against the online players. Exactly one reading must fit; an ambiguous one is refused, never guessed.
//
// Nothing here touches Windows or the game: tests/console_args_test.cpp links it on the host.
#pragma once
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace console {

struct Arg {
    std::string text;       // quotes removed, \" \' \\ unescaped inside quotes
    bool quoted = false;    // started with a quote: always one argument, never joined with its neighbours
    bool unclosed = false;  // its quote was never closed (it runs to the end of the line)
    size_t begin = 0, end = 0;  // byte span in the line, quotes included
};

// Splits on whitespace. A quote opens a quoted argument only at the start of a word, so the
// apostrophe in "don't" is plain text.
std::vector<Arg> Tokenize(const std::string& line);

struct Player {
    std::string steamId, name, characterName;  // name = Steam persona, characterName = in-game name
};

enum class Match { None, One, Ambiguous };

// SteamID64 first, then the persona or character name (case-insensitive). Ambiguous when several
// online players carry that name.
Match FindPlayer(const std::vector<Player>& online, const std::string& idOrName, const Player** hit);

// One shape a command accepts: `players` player arguments first, then minRest..maxRest further
// arguments that restOk accepts (null = any).
struct Form {
    int players = 1;
    size_t minRest = 0, maxRest = 0;
    std::function<bool(const std::vector<Arg>&)> restOk;
    bool freeText = false;  // the rest is a message: it may hold an unclosed quote
};

struct Parsed {
    size_t form = 0;              // index into the forms
    std::vector<Player> players;  // resolved, in order
    std::vector<Arg> rest;
};

// Resolves args (the words after the command) against the forms. True when exactly one form and
// split fits; otherwise false with err set (unknown player, several players by one name, several
// readings, unclosed quote).
bool ResolveArgs(const std::vector<Arg>& args, const std::vector<Form>& forms, const std::vector<Player>& online,
                 Parsed& out, std::string& err);

}  // namespace console
