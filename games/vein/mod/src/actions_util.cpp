#include "actions_util.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cctype>
#include <cerrno>
#include <cstdlib>

namespace ActionsUtil {

std::string Lower(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

bool LooksLikeSteamId64(uint64_t v) { return (v >> 32) == 0x01100001ull && (uint32_t)v != 0; }

std::string NormalizeGameId(const std::string& raw) {
    std::string s = raw;
    size_t colon = s.rfind(':');
    if (colon != std::string::npos) s = s.substr(colon + 1);
    std::string digits;
    for (char c : s)
        if (isdigit((unsigned char)c)) digits += c;
    // Only a *pure* 17-digit id counts: "Player17Digits1234" must stay a name, not become an id.
    if (digits.size() == 17 && digits.size() == s.size()) return digits;
    return Lower(raw);
}

bool IsGeneratedArtefact(const std::string& name) {
    return name.empty() || name.rfind("SKEL_", 0) == 0 || name.rfind("REINST_", 0) == 0 ||
           name.rfind("Default__", 0) == 0 || name.rfind("TRASHCLASS_", 0) == 0 ||
           name.rfind("PLACEHOLDER-", 0) == 0;
}

// ---- lane L3c: readable catalogue names -----------------------------------------------------

std::string HumaniseCode(const std::string& code) {
    if (code.empty()) return code;
    std::string s = code;
    // Blueprint decorations.
    if (s.rfind("BP_", 0) == 0) s = s.substr(3);
    if (s.size() > 2 && s.compare(s.size() - 2, 2, "_C") == 0) s = s.substr(0, s.size() - 2);
    if (s.size() <= 1) return code;  // the decorations were all there was

    // Split into words on '_' and on camel-case boundaries. A run of capitals stays one word
    // ("USD", "CL") except for the last capital when a lower-case letter follows it ("USDItem" ->
    // "USD Item"). Digits stay attached to the word they follow ("T1", "Cigarettes01").
    std::vector<std::string> words;
    std::string cur;
    auto flush = [&] {
        if (!cur.empty()) words.push_back(cur);
        cur.clear();
    };
    for (size_t i = 0; i < s.size(); i++) {
        char c = s[i];
        if (c == '_' || c == '-' || c == ' ') {
            flush();
            continue;
        }
        bool upper = isupper((unsigned char)c) != 0;
        if (upper && !cur.empty()) {
            char prev = cur.back();
            bool prevLowerOrDigit = islower((unsigned char)prev) || isdigit((unsigned char)prev);
            bool nextLower = i + 1 < s.size() && islower((unsigned char)s[i + 1]);
            if (prevLowerOrDigit || (isupper((unsigned char)prev) && nextLower)) flush();
        }
        cur += c;
    }
    flush();
    if (words.empty()) return code;

    // A trailing "Item" is a naming convention, not part of the label - but never drop the only
    // word ("Item" stays "Item").
    if (words.size() > 1 && words.back() == "Item") words.pop_back();

    std::string out;
    for (size_t i = 0; i < words.size(); i++) {
        if (i) out += ' ';
        out += words[i];
    }
    return out.empty() ? code : out;
}

std::string ItemCodeFromSoftPath(const std::string& raw) {
    if (raw.empty()) return raw;
    std::string s = raw;
    // An object path is "<package>.<object>": the object part is already the class name.
    size_t dot = s.find_last_of('.');
    size_t slash = s.find_last_of('/');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
        s = s.substr(dot + 1);
    else if (slash != std::string::npos)
        s = s.substr(slash + 1);
    if (s.empty()) return "";
    // A cooked Blueprint's generated class is the asset name plus "_C".
    if (s.size() < 2 || s.compare(s.size() - 2, 2, "_C") != 0) s += "_C";
    return s;
}

std::vector<std::string> Words(const std::string& s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && isspace((unsigned char)s[i])) i++;
        size_t start = i;
        while (i < s.size() && !isspace((unsigned char)s[i])) i++;
        if (i > start) out.push_back(s.substr(start, i - start));
    }
    return out;
}

std::string Rest(const std::string& s, size_t skipWords) {
    size_t i = 0, w = 0;
    while (i < s.size() && w < skipWords) {
        while (i < s.size() && isspace((unsigned char)s[i])) i++;
        while (i < s.size() && !isspace((unsigned char)s[i])) i++;
        w++;
    }
    while (i < s.size() && isspace((unsigned char)s[i])) i++;
    return s.substr(i);
}

bool MatchesSearch(const CatalogueEntry& entry, const std::string& lowerNeedle) {
    if (lowerNeedle.empty()) return true;
    return Lower(entry.code).find(lowerNeedle) != std::string::npos ||
           Lower(entry.name).find(lowerNeedle) != std::string::npos;
}

int LookupIndex(const std::vector<CatalogueEntry>& entries, const std::string& needle, std::string& err) {
    // F11 backward compatibility: an asset path (what the inventory used to report, and what
    // Takaro may have stored) resolves to the same item as its class short name.
    if (needle.find('/') != std::string::npos || needle.find('.') != std::string::npos) {
        std::string folded = ItemCodeFromSoftPath(needle);
        if (!folded.empty() && folded != needle) {
            std::string ignored;
            int hit = LookupIndex(entries, folded, ignored);
            if (hit >= 0) return hit;
        }
    }
    std::string want = Lower(needle);
    for (size_t i = 0; i < entries.size(); i++)
        if (Lower(entries[i].code) == want) return (int)i;
    for (size_t i = 0; i < entries.size(); i++)
        if (Lower(entries[i].name) == want) return (int)i;
    int hit = -1;
    size_t n = 0;
    for (size_t i = 0; i < entries.size(); i++) {
        if (!MatchesSearch(entries[i], want)) continue;
        hit = (int)i;
        n++;
    }
    if (n == 1) return hit;
    err = n ? ("item code '" + needle + "' is ambiguous (" + std::to_string(n) + " matches)")
            : ("unknown item code '" + needle + "'");
    return -1;
}


// ---- lane L3b: admin grant list -------------------------------------------------------------

std::vector<std::string> ParseSteamIdList(const std::string& raw, std::vector<std::string>* rejected) {
    std::vector<std::string> out;
    std::string tok;
    auto flush = [&]() {
        // trim
        size_t b = 0, e = tok.size();
        while (b < e && (isspace((unsigned char)tok[b]) || tok[b] == '"' || tok[b] == '\'')) b++;
        while (e > b && (isspace((unsigned char)tok[e - 1]) || tok[e - 1] == '"' || tok[e - 1] == '\'')) e--;
        std::string t = tok.substr(b, e - b);
        tok.clear();
        if (t.empty()) return;
        std::string id = NormalizeGameId(t);
        bool ok = id.size() == 17;
        if (ok)
            for (char ch : id)
                if (!isdigit((unsigned char)ch)) ok = false;
        if (ok) {
            errno = 0;
            unsigned long long v = strtoull(id.c_str(), nullptr, 10);
            ok = errno == 0 && LooksLikeSteamId64((uint64_t)v);
        }
        if (!ok) {
            if (rejected) rejected->push_back(t);
            return;
        }
        for (const std::string& seen : out)
            if (seen == id) return;
        out.push_back(id);
    };
    for (char ch : raw) {
        if (ch == ',' || ch == ';' || isspace((unsigned char)ch))
            flush();
        else
            tok.push_back(ch);
    }
    flush();
    return out;
}

// ---- lane L3d: broadcast path selection ------------------------------------------------------

BroadcastPath ChooseBroadcastPath(const std::string& viaPref, const BroadcastCaps& caps, std::string& err) {
    std::string pref = Lower(viaPref);
    // trim
    while (!pref.empty() && isspace((unsigned char)pref.front())) pref.erase(pref.begin());
    while (!pref.empty() && isspace((unsigned char)pref.back())) pref.pop_back();

    const bool chatUsable = caps.sendChat && caps.haveSender;  // NEVER chat without a real sender

    if (pref == "chat") {
        if (chatUsable) return BroadcastPath::kChatWithSender;
        // fall through to the safe paths rather than crashing the server
    } else if (pref == "servermessage" || pref == "gamestate") {
        if (caps.gameStateServerMsg) return BroadcastPath::kGameStateServerMsg;
    } else if (pref == "admin") {
        if (caps.adminServerMsg) return BroadcastPath::kAdminServerMsg;
    }

    if (caps.gameStateServerMsg) return BroadcastPath::kGameStateServerMsg;
    if (caps.adminServerMsg) return BroadcastPath::kAdminServerMsg;
    if (chatUsable) return BroadcastPath::kChatWithSender;

    if (caps.sendChat && !caps.haveSender)
        err = "no sender available: NetMulticast_SendChat crashes on a null sender and no server-message "
              "path resolved";
    else
        err = "no broadcast path available: neither AVeinGameStateBase::NetMulticast_BroadcastServerMessage "
              "nor UAdminComponent::Server_SendServerMessage resolved";
    return BroadcastPath::kNone;
}

const char* BroadcastPathSymbol(BroadcastPath p) {
    switch (p) {
        case BroadcastPath::kGameStateServerMsg:
            return "AVeinGameStateBase::NetMulticast_BroadcastServerMessage";
        case BroadcastPath::kAdminServerMsg:
            return "UAdminComponent::Server_SendServerMessage";
        case BroadcastPath::kChatWithSender:
            return "AVeinGameStateBase::NetMulticast_SendChat";
        case BroadcastPath::kNone:
        default:
            return "";
    }
}

std::string RenderMessage(const std::string& senderName, const std::string& text) {
    std::string s = senderName;
    while (!s.empty() && isspace((unsigned char)s.front())) s.erase(s.begin());
    while (!s.empty() && isspace((unsigned char)s.back())) s.pop_back();
    if (s.empty()) return text;
    return "[" + s + "] " + text;
}

std::string ResolveSenderName(const std::string& override, const std::string& configured,
                              const std::string& serverName) {
    for (const std::string* candidate : {&override, &configured, &serverName}) {
        std::string s = *candidate;
        while (!s.empty() && isspace((unsigned char)s.front())) s.erase(s.begin());
        while (!s.empty() && isspace((unsigned char)s.back())) s.pop_back();
        if (!s.empty()) return s;
    }
    return "Server";
}

}  // namespace ActionsUtil

// ---- lane L3e -------------------------------------------------------------------------------

double ActionsUtil::Distance3(const double a[3], const double b[3]) {
    double dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

bool ActionsUtil::TeleportArrived(const double before[3], const double after[3], const double target[3],
                                  double toleranceCm) {
    return Distance3(before, after) > 1.0 && Distance3(target, after) <= toleranceCm;
}

bool ActionsUtil::GiveArrived(int before, int after) { return after > before; }

std::string ActionsUtil::JsonStrArray(const std::vector<std::string>& v) {
    std::string o = "[";
    for (size_t i = 0; i < v.size(); i++) o += (i ? "," : "") + JsonStr(v[i]);
    return o + "]";
}

// ---- lane L3f -------------------------------------------------------------------------------

bool ActionsUtil::IsPlayerInventoryClass(const std::string& className) {
    if (className.empty()) return false;
    std::string l = Lower(className);
    static const char* kNotThePlayers[] = {"corpse", "cache",   "container", "storage", "stash",
                                           "loot",   "vehicle", "crafting",  "shop",    "vendor",
                                           "grave",  "deadbody"};
    for (auto* bad : kNotThePlayers)
        if (l.find(bad) != std::string::npos) return false;
    return l.find("inventory") != std::string::npos;
}

std::string ActionsUtil::GuidDigits(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    if (!a && !b && !c && !d) return "";
    char buf[40];
    snprintf(buf, sizeof buf, "%08X%08X%08X%08X", a, b, c, d);
    return buf;
}

std::vector<int> ActionsUtil::StackSplit(int amount, int maxStack) {
    std::vector<int> out;
    if (amount <= 0) return out;
    if (maxStack < 1) maxStack = 1;
    // A safety net, not a policy: `amount` is already clamped to 1000 by the handler.
    for (int remaining = amount; remaining > 0 && out.size() < 4096;) {
        int n = remaining > maxStack ? maxStack : remaining;
        out.push_back(n);
        remaining -= n;
    }
    return out;
}

// ---- lane L2c: the `weapon` field of entity-killed ---------------------------------------------

std::string ActionsUtil::KillWeaponName(const std::string& causerClass, const std::string& itemName) {
    std::string lower;
    lower.reserve(causerClass.size());
    for (char c : causerClass) lower += (char)tolower((unsigned char)c);
    // A pawn is never a weapon, whatever else is known about it: BP_VeinPlayerCharacter_C is the
    // debug kill's causer, BP_Zombie_C and BP_Wolf_C are bites. This test comes FIRST so that a
    // stray item name can never turn a bite into a swing.
    static const char* kNotWeapon[] = {"character", "zombie", "animal", "pawn",
                                       "vehicle",   "volume", "controller"};
    for (const char* n : kNotWeapon)
        if (lower.find(n) != std::string::npos) return "";
    if (!itemName.empty()) return itemName;
    if (causerClass.empty()) return "";
    static const char* kWeapon[] = {"equippeditem", "weapon", "projectile", "bullet",
                                    "arrow",        "blade",  "throwable",  "explosive"};
    for (const char* n : kWeapon)
        if (lower.find(n) != std::string::npos) return HumaniseCode(causerClass);
    return "";
}

std::string ActionsUtil::LocationRefusal(bool hasPawn, bool spawned, bool hasLoc, const std::string& pawnClass) {
    if (!hasPawn) return "player has no pawn yet";
    if (!spawned)
        return "player is on the character screen (pawn is " + (pawnClass.empty() ? "unknown" : pawnClass) +
               "), so there is no position in the world";
    if (!hasLoc) return "pawn has no readable root component";
    return "";
}
