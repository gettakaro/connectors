#include "names.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

std::string Lower(const std::string& s) {
    std::string out = s;
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool In(const char* const* list, size_t count, const std::string& lowered) {
    for (size_t i = 0; i < count; i++)
        if (lowered == list[i]) return true;
    return false;
}

// Tokens kept whole rather than camel-split: either a technical word that a camel split
// would shatter (8kMapLabel -> "8k Map Label") or one that is dropped anyway.
const char* const kAtomic[] = {"8kmaplabel", "maplabel",   "mapmarker",   "placement",  "helper",
                               "deprecated", "depricated", "projectilespawner",         "noui",
                               "healthbar",  "ag2",        "old",         "original",   "backup",
                               "variation",  "lore",       "townsfolk",   "notfinished", "donotuse",
                               "8kloretext", "loretext",   "buildtool",   "craftingstation",
                               "npcsummoner", "testitem",  "lvlxx"};

// Dropped while they lead an entity or location name, in order, never below one remaining
// token: what kind of thing the template is, which is what the type, family and faction
// fields already say.
const char* const kLeadingEntity[] = {"base",  "prop",  "teleport", "enemy",     "animal",  "npc",
                                      "townsfolk", "player", "trap",             "shroud",  "zone",
                                      "attack", "projectile", "collider", "wildbeast", "wildlife"};

// The same for items: the item's category, which listItems answers in its description.
const char* const kLeadingItem[] = {"block",    "buildtool",   "voxel",       "prop",    "decoration",
                                    "component", "material",   "food",        "craft",   "tool",
                                    "weapon",   "shield",      "armor",       "vanity",  "ammo",
                                    "consumable", "ability",   "flower",      "8kloretext", "loretext",
                                    "z",        "zz",          "blueprint",   "animal",  "empty",
                                    "instrument", "base",      "vc",          "building",
                                    "craftingstation",         "npcsummoner", "lockpick", "philipp",
                                    "surprise", "ecs",         "no",          "build"};

// Dropped wherever they appear: build bookkeeping, not anything a player or an operator
// would recognise.
const char* const kNoise[] = {"8kmaplabel", "maplabel",  "mapmarker", "ag2",       "old",
                              "deprecated", "depricated", "noui",     "healthbar", "lore",
                              "original",   "backup",    "variation", "placement", "helper",
                              "notfinished", "donotuse", "test",      "projectilespawner"};

// The same for items, plus the words the item pipeline uses to park an asset.
const char* const kNoiseItem[] = {"unused", "hasbugs", "lvlxx", "tx", "testitem"};

// The same for entities: how a template is spawned or hooked into the world, not what it is
// (Enemy_Wildbeast_Rat_hook is a rat; Enemy_Fogger_Heavy_SummonStone the Heavy a summoning
// stone calls up), and the wild-beast family word wherever it sits.
const char* const kNoiseEntity[] = {"hook", "hookpte", "bosshealthbar", "bosshealtbar", "summonstone", "wildbeast"};

bool IsDigits(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s)
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    return true;
}

// `hint`, `hint02`: a quest-design marker.
bool IsHint(const std::string& lowered) {
    if (lowered.compare(0, 4, "hint") != 0) return false;
    for (size_t i = 4; i < lowered.size(); i++)
        if (!std::isdigit(static_cast<unsigned char>(lowered[i]))) return false;
    return true;
}

// `v03`: an asset revision.
bool IsVersion(const std::string& lowered) {
    if (lowered.size() < 2 || lowered[0] != 'v') return false;
    for (size_t i = 1; i < lowered.size(); i++)
        if (!std::isdigit(static_cast<unsigned char>(lowered[i]))) return false;
    return true;
}

// `T0` .. `T9`: the difficulty tier, which becomes a readable suffix rather than a token.
bool IsTier(const std::string& s) {
    return s.size() == 2 && (s[0] == 'T' || s[0] == 't') && std::isdigit(static_cast<unsigned char>(s[1]));
}

// `T1-TX`, `T3-T5`: a tier range an asset was parked under, which is not a tier.
bool IsTierRange(const std::string& s) {
    if (s.size() != 5 || s[2] != '-') return false;
    const std::string l = Lower(s);
    auto slot = [](char c) { return std::isdigit(static_cast<unsigned char>(c)) || c == 'x'; };
    return l[0] == 't' && slot(l[1]) && l[3] == 't' && slot(l[4]);
}

bool IsNoise(const std::string& token, NameKind kind) {
    const std::string lowered = Lower(token);
    if (In(kNoise, sizeof kNoise / sizeof *kNoise, lowered)) return true;
    if (kind == NameKind::Item && In(kNoiseItem, sizeof kNoiseItem / sizeof *kNoiseItem, lowered)) return true;
    if (kind == NameKind::Entity && In(kNoiseEntity, sizeof kNoiseEntity / sizeof *kNoiseEntity, lowered)) return true;
    return IsHint(lowered) || IsVersion(lowered) || IsTierRange(token);
}

bool IsLeading(const std::string& token, NameKind kind) {
    const std::string lowered = Lower(token);
    if (kind == NameKind::Item) return In(kLeadingItem, sizeof kLeadingItem / sizeof *kLeadingItem, lowered);
    return In(kLeadingEntity, sizeof kLeadingEntity / sizeof *kLeadingEntity, lowered);
}

// Split one underscore-separated token on its camel boundaries, and strip a trailing
// variant number off a word (`cryptKeeper01` -> crypt, Keeper). A token that begins with a
// digit keeps its digits: `6x6` is a measurement and `2H` a grip, not enumerators. With
// `keep_digits` the stripped number survives as its own word (`Version2` -> Version, 2).
void CamelSplit(const std::string& token, bool keep_digits, std::vector<std::string>& out) {
    std::vector<std::string> parts;
    std::string current;
    char previous = 0;
    bool has_alpha = false;
    for (char c : token) {
        const bool splits = c >= 'A' && c <= 'Z' &&
                            ((previous >= 'a' && previous <= 'z') ||
                             (previous >= '0' && previous <= '9' && has_alpha));
        if (splits && !current.empty()) {
            parts.push_back(current);
            current.clear();
            has_alpha = false;
        }
        current += c;
        if (std::isalpha(static_cast<unsigned char>(c))) has_alpha = true;
        previous = c;
    }
    if (!current.empty()) parts.push_back(current);

    for (const std::string& part : parts) {
        if (IsTier(part) || part.empty()) {
            out.push_back(part);
            continue;
        }
        if (!std::isalpha(static_cast<unsigned char>(part[0]))) {
            // `01COMMON` is an enumerator in front of a word; `1m`, `2H` and `6x6` are not.
            size_t digits = 0;
            while (digits < part.size() && std::isdigit(static_cast<unsigned char>(part[digits]))) digits++;
            size_t letters = 0;
            while (digits + letters < part.size() &&
                   std::isalpha(static_cast<unsigned char>(part[digits + letters])))
                letters++;
            if (digits > 0 && letters >= 3 && digits + letters == part.size()) {
                out.push_back(part.substr(0, digits));
                out.push_back(part.substr(digits));
            } else {
                out.push_back(part);
            }
            continue;
        }
        size_t end = part.size();
        while (end > 0 && std::isdigit(static_cast<unsigned char>(part[end - 1]))) end--;
        // Keep at least one letter; a word that is nothing but digits never reaches here.
        if (end > 0 && std::isalpha(static_cast<unsigned char>(part[end - 1]))) {
            out.push_back(part.substr(0, end));
            if (keep_digits && end < part.size()) out.push_back(part.substr(end));
        } else {
            out.push_back(part);
        }
    }
}

// First letter up. A word shouted in the code (`REWARD`, `LOOT`) is a word, not an
// acronym, once it is four letters or longer; `NPC`, `DPS` and `2H` keep their shape.
std::string Capitalised(const std::string& token) {
    std::string out = token;
    if (out.empty()) return out;
    bool letters = false, all_upper = true;
    for (char c : out) {
        if (!std::isalpha(static_cast<unsigned char>(c))) continue;
        letters = true;
        if (!std::isupper(static_cast<unsigned char>(c))) all_upper = false;
    }
    if (letters && all_upper && out.size() >= 4)
        for (size_t i = 1; i < out.size(); i++)
            out[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(out[i])));
    out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0])));
    return out;
}

// `01` -> `1`: an enumerator reads as a number, not as an asset suffix.
std::string Number(const std::string& digits) {
    size_t i = 0;
    while (i + 1 < digits.size() && digits[i] == '0') i++;
    return digits.substr(i);
}

// `detail` is how much of the code survives, and it only ever rises to tell two codes
// apart: 0 is the readable derivation, 1 keeps the numbers attached to a word, 2 keeps the
// leading category words as a trailing parenthetical, 3 also camel-splits the tokens that
// are otherwise kept whole. Noise is dropped at every level.
std::string DisplayNameAt(const char* code, NameKind kind, int detail) {
    const std::string raw = code ? code : "";
    const bool keep_digits = detail >= 1;
    const bool keep_leading = detail >= 2;
    const bool split_all = detail >= 3;

    std::vector<std::string> pieces;
    std::string piece;
    for (char c : raw) {
        if (c == '_') {
            if (!piece.empty()) pieces.push_back(piece);
            piece.clear();
        } else {
            piece += c;
        }
    }
    if (!piece.empty()) pieces.push_back(piece);

    std::vector<std::string> tokens;
    for (const std::string& part : pieces) {
        const std::string lowered = Lower(part);
        if (IsNoise(part, kind)) continue;
        if (IsDigits(part) || IsTierRange(part) ||
            (!split_all && (In(kAtomic, sizeof kAtomic / sizeof *kAtomic, lowered) || IsHint(lowered) ||
                            IsVersion(lowered))))
            tokens.push_back(part);
        else
            CamelSplit(part, keep_digits, tokens);
    }

    // A map label spells out its region and its slot before the name that matters:
    // `8kMapLabel_steppes_town_06_Brightwich` is Brightwich. The last enumerator that has a
    // real name after it is where the name starts.
    bool trailing_number = false;
    if (kind == NameKind::Location && !keep_leading) {
        size_t cut = tokens.size();
        for (size_t i = 0; i < tokens.size(); i++) {
            if (!IsDigits(tokens[i])) continue;
            bool named_after = false;
            for (size_t j = i + 1; j < tokens.size(); j++)
                if (!IsDigits(tokens[j]) && !IsNoise(tokens[j], kind)) named_after = true;
            if (named_after) cut = i;
        }
        if (cut < tokens.size()) {
            tokens.erase(tokens.begin(), tokens.begin() + static_cast<long>(cut) + 1);
            trailing_number = !tokens.empty() && IsDigits(tokens.back());
        }
    }

    std::vector<std::string> filtered;
    for (size_t i = 0; i < tokens.size(); i++) {
        if (IsNoise(tokens[i], kind)) continue;
        if (IsDigits(tokens[i])) {
            // An enumerator that leads says nothing; one that follows a name tells two
            // `..._HuntressCamp_1` apart, and at detail 1 it is what separates the codes.
            if (keep_digits && !filtered.empty())
                filtered.push_back(Number(tokens[i]));
            else if (trailing_number && i + 1 == tokens.size() && !filtered.empty())
                filtered.push_back(tokens[i]);
            continue;
        }
        filtered.push_back(tokens[i]);
    }

    std::vector<std::string> named = filtered;
    std::vector<std::string> category;
    while (named.size() > 1 && IsLeading(named.front(), kind)) {
        category.push_back(named.front());
        named.erase(named.begin());
    }

    std::string tier;
    std::vector<std::string> rest;
    for (const std::string& token : named) {
        if (IsTier(token)) {
            if (tier.empty()) tier = token.substr(1);
            continue;
        }
        rest.push_back(token);
    }
    named = rest;
    // Everything was technical: say what is left rather than nothing, and never the code.
    if (named.empty())
        for (const std::string& token : filtered)
            if (!IsTier(token)) named.push_back(token);
    if (named.empty()) CamelSplit(raw, keep_digits, named);

    std::string name;
    for (const std::string& token : named) {
        if (token.empty()) continue;
        if (!name.empty()) name += ' ';
        name += Capitalised(token);
    }
    if (!tier.empty()) {
        if (!name.empty()) name += ' ';
        name += "(Tier " + tier + ")";
    }
    // The category reads after the name rather than in front of it: it is there to tell
    // two otherwise identical names apart, and a restored prefix would often be exactly
    // the code with its underscores opened, which is what the catalogue check refuses.
    if (keep_leading && !category.empty()) {
        std::string words;
        for (const std::string& token : category) {
            if (!words.empty()) words += ' ';
            words += Capitalised(token);
        }
        if (!name.empty()) name += ' ';
        name += "(" + words + ")";
    }
    if (name.empty()) {
        for (char c : raw) name += (c == '_') ? ' ' : c;
    }
    return name;
}

}  // namespace

std::string DisplayName(const char* code, NameKind kind) { return DisplayNameAt(code, kind, 0); }

void DistinctNames(NameKind kind, const char* const* codes, size_t count, std::vector<std::string>& out) {
    out.clear();
    out.reserve(count);
    for (size_t i = 0; i < count; i++) out.push_back(DisplayNameAt(codes[i], kind, 0));

    for (int detail = 1; detail <= 3; detail++) {
        std::map<std::string, std::vector<size_t>> groups;
        for (size_t i = 0; i < count; i++) groups[out[i]].push_back(i);
        std::vector<size_t> again;
        for (const auto& group : groups) {
            std::vector<std::string> distinct;
            for (size_t i : group.second) {
                const std::string code = codes[i] ? codes[i] : "";
                if (std::find(distinct.begin(), distinct.end(), code) == distinct.end()) distinct.push_back(code);
            }
            if (distinct.size() >= 2) again.insert(again.end(), group.second.begin(), group.second.end());
        }
        if (again.empty()) return;
        for (size_t i : again) out[i] = DisplayNameAt(codes[i], kind, detail);
    }

    // Two codes whose only difference is bookkeeping the derivation drops on purpose
    // (`..._AG2`, `..._DEPRECATED`). A numbered variant keeps the answer distinct without
    // putting that bookkeeping in front of an operator.
    std::map<std::string, std::vector<size_t>> groups;
    for (size_t i = 0; i < count; i++) groups[out[i]].push_back(i);
    for (const auto& group : groups) {
        if (group.second.size() < 2) continue;
        std::vector<size_t> order = group.second;
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
            return std::strcmp(codes[a] ? codes[a] : "", codes[b] ? codes[b] : "") < 0;
        });
        std::vector<std::string> seen;
        for (size_t i : order) {
            const std::string code = codes[i] ? codes[i] : "";
            auto at = std::find(seen.begin(), seen.end(), code);
            if (at == seen.end()) {
                seen.push_back(code);
                at = seen.end() - 1;
            }
            const size_t nth = static_cast<size_t>(at - seen.begin()) + 1;
            if (nth > 1) out[i] = group.first + " (variant " + std::to_string(nth) + ")";
        }
    }
}
