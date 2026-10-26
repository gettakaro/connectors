#include "console_args.h"

#include <cctype>

namespace console {

namespace {

std::string Lower(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

// The player name a run of words spells: one word as given, several bare words joined by one space.
// A quoted word stands alone, and an unclosed quote is never part of a name.
bool SpanName(const std::vector<Arg>& a, size_t from, size_t to, std::string& out) {
    if (to - from == 1) {
        if (a[from].unclosed) return false;
        out = a[from].text;
        return true;
    }
    out.clear();
    for (size_t i = from; i < to; i++) {
        if (a[i].quoted) return false;
        out += (i > from ? " " : "") + a[i].text;
    }
    return true;
}

std::string Describe(const Player& p) {
    std::string n = p.characterName.empty() ? p.name : p.characterName;
    return n + " (" + p.steamId + ")";
}

}  // namespace

std::vector<Arg> Tokenize(const std::string& s) {
    std::vector<Arg> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && isspace((unsigned char)s[i])) i++;
        if (i >= s.size()) break;
        Arg a;
        a.begin = i;
        if (s[i] == '"' || s[i] == '\'') {
            char q = s[i++];
            a.quoted = true;
            a.unclosed = true;
            while (i < s.size()) {
                if (s[i] == '\\' && i + 1 < s.size() && (s[i + 1] == q || s[i + 1] == '\\')) {
                    a.text += s[i + 1];
                    i += 2;
                } else if (s[i] == q) {
                    a.unclosed = false;
                    i++;
                    break;
                } else {
                    a.text += s[i++];
                }
            }
        } else {
            while (i < s.size() && !isspace((unsigned char)s[i])) a.text += s[i++];
        }
        a.end = i;
        out.push_back(a);
    }
    return out;
}

Match FindPlayer(const std::vector<Player>& online, const std::string& idOrName, const Player** hit) {
    for (auto& p : online)
        if (!idOrName.empty() && p.steamId == idOrName) {
            if (hit) *hit = &p;
            return Match::One;
        }
    std::string want = Lower(idOrName);
    const Player* found = nullptr;
    int n = 0;
    for (auto& p : online)
        if (!want.empty() && (Lower(p.name) == want || (!p.characterName.empty() && Lower(p.characterName) == want))) {
            found = &p;
            n++;
        }
    if (n == 0) return Match::None;
    if (hit) *hit = found;
    return n == 1 ? Match::One : Match::Ambiguous;
}

bool ResolveArgs(const std::vector<Arg>& args, const std::vector<Form>& forms, const std::vector<Player>& online,
                 Parsed& out, std::string& err) {
    std::vector<Parsed> fits;          // every player span names exactly one online player
    std::vector<std::string> shapes;   // how each fit (or name clash) reads, for the ambiguity message
    std::string clash;                 // a name several online players share
    std::vector<std::vector<std::string>> misses;  // per structurally valid reading: the names that matched nobody
    bool unclosed = false;
    for (auto& a : args) unclosed |= a.unclosed;
    const size_t n = args.size();

    for (size_t f = 0; f < forms.size(); f++) {
        const Form& form = forms[f];
        std::vector<size_t> cuts(form.players + 1, 0);  // player i is args[cuts[i], cuts[i+1])
        std::function<void(int)> walk = [&](int p) {
            if (p == form.players) {
                size_t rest = n - cuts[p];
                if (rest < form.minRest || rest > form.maxRest) return;
                std::vector<Arg> tail(args.begin() + cuts[p], args.end());
                for (auto& a : tail)
                    if (a.unclosed && !form.freeText) return;
                if (form.restOk && !form.restOk(tail)) return;
                Parsed cand;
                cand.form = f;
                cand.rest = tail;
                std::vector<std::string> names, missing;
                std::string shared;
                for (int i = 0; i < form.players; i++) {
                    std::string name;
                    if (!SpanName(args, cuts[i], cuts[i + 1], name)) return;
                    const Player* hit = nullptr;
                    Match m = FindPlayer(online, name, &hit);
                    names.push_back("'" + name + "'");
                    if (m == Match::None) missing.push_back(name);
                    if (m == Match::Ambiguous && shared.empty()) shared = name;
                    if (m == Match::One) cand.players.push_back(*hit);
                }
                std::string shape;
                for (size_t i = 0; i < names.size(); i++) shape += (i ? " and " : "") + names[i];
                if (!missing.empty()) {
                    misses.push_back(missing);
                    return;
                }
                shapes.push_back(shape);
                if (shared.empty()) {
                    fits.push_back(cand);
                } else if (clash.empty()) {
                    clash = "'" + shared + "' is the name of several online players:";
                    for (auto& o : online)
                        if (FindPlayer({o}, shared, nullptr) == Match::One) clash += " " + Describe(o) + ",";
                    clash.back() = ';';
                    clash += " use the SteamID64";
                }
                return;
            }
            // leave at least one word for each remaining player
            for (size_t e = cuts[p] + 1; e + (form.players - p - 1) <= n; e++) {
                cuts[p + 1] = e;
                walk(p + 1);
            }
        };
        if (n >= (size_t)form.players) walk(0);
    }

    if (fits.size() == 1 && shapes.size() == 1) {
        out = fits[0];
        return true;
    }
    if (shapes.size() > 1) {
        err = "ambiguous player name: could be";
        for (size_t i = 0; i < shapes.size(); i++) err += (i ? " or " : " ") + shapes[i];
        err += "; quote the name (\"Character 1\") or use the SteamID64";
    } else if (!clash.empty()) {
        err = clash;
    } else if (unclosed) {
        err = "unclosed quote; write names with spaces as \"Character 1\"";
    } else if (misses.size() == 1) {
        err = "player not online:";
        for (size_t i = 0; i < misses[0].size(); i++) err += (i ? ", " : " ") + misses[0][i];
    } else if (!misses.empty()) {
        std::string line;
        for (size_t i = 0; i < n; i++) line += (i ? " " : "") + args[i].text;
        err = "no online player matches '" + line + "'";
    } else {
        err.clear();  // no form fits the number or kind of arguments: the caller shows its usage
    }
    return false;
}

}  // namespace console
