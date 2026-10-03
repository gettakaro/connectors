#include "conan/player_snapshot.h"

#include <cstring>

namespace conan {

using UE::Reflection;

bool PlayerLayout::Resolve(const Reflection& r, std::string& error) {
    std::string missing;
    auto type = [&](const char* name) {
        uintptr_t t = r.Type(name);
        if (!t) missing += std::string(missing.empty() ? "" : ", ") + name;
        return t;
    };
    gameStateBase = type("GameStateBase");
    uintptr_t playerState = type("PlayerState"), actor = type("Actor"), controller = type("Controller"),
              scene = type("SceneComponent");
    pcClass = type("ConanPlayerController");
    basePlayerChar = type("BasePlayerChar_C");
    baseBPChar = type("BaseBPChar_C");
    itemInventory = type("ItemInventory");
    gameItem = type("GameItem");
    if (!missing.empty()) {
        error = "classes not loaded: " + missing;
        return false;
    }
    auto off = [&](uintptr_t st, const char* cls, const char* name, const char* t, int32_t size) {
        int32_t o = r.Offset(st, name, t, size);
        if (o < 0) missing += std::string(missing.empty() ? "" : ", ") + cls + "." + name;
        return o;
    };
    playerArray = off(gameStateBase, "GameStateBase", "PlayerArray", "ArrayProperty", 16);
    psOwner = off(actor, "Actor", "Owner", "ObjectProperty", 8);
    psName = off(playerState, "PlayerState", "PlayerNamePrivate", "StrProperty", 16);
    psAddress = off(playerState, "PlayerState", "SavedNetworkAddress", "StrProperty", 16);
    pcUserId = off(pcClass, "ConanPlayerController", "UserIDFromURLOptions", "StrProperty", 16);
    pcPawn = off(controller, "Controller", "Pawn", "ObjectProperty", 8);
    rootComponent = off(actor, "Actor", "RootComponent", "ObjectProperty", 8);
    attachParent = off(scene, "SceneComponent", "AttachParent", "ObjectProperty", 8);
    relativeLocation = off(scene, "SceneComponent", "RelativeLocation", "StructProperty", 24);
    characterName = off(baseBPChar, "BaseBPChar_C", "CharacterName", "StrProperty", 16);
    backpack = off(basePlayerChar, "BasePlayerChar_C", "BackpackInventory", "ObjectProperty", 8);
    hotbar = off(basePlayerChar, "BasePlayerChar_C", "ShortcutBarInventory", "ObjectProperty", 8);
    equipment = off(baseBPChar, "BaseBPChar_C", "EquipmentInventory", "ObjectProperty", 8);
    itemList = off(itemInventory, "ItemInventory", "ItemList", "ArrayProperty", 16);
    templateId = off(gameItem, "GameItem", "TemplateId", "IntProperty", 4);
    ownerInventory = off(gameItem, "GameItem", "m_OwnerInventory", "ObjectProperty", 8);
    if (!missing.empty()) {
        error = "properties not found by reflection: " + missing;
        return false;
    }
    // The unreflected ExactPing float sits in the gap right before SavedNetworkAddress (S3 §6).
    if (psAddress != kExactPing + 8) {
        error = "PlayerState layout moved (SavedNetworkAddress at " + std::to_string(psAddress) +
                "); the pinned ExactPing offset does not apply";
        return false;
    }
    return true;
}

bool PlayerReader::ReadPtrArray(uintptr_t at, int32_t maxNum, std::vector<uintptr_t>& out) const {
    for (int attempt = 0; attempt < 3; attempt++) {
        struct {
            uintptr_t data;
            int32_t num, max;
        } a, b;
        if (!m_.Read(at, &a, sizeof a)) return false;
        if (a.num < 0 || a.num > maxNum || a.max < a.num || (a.num && !UE::Plausible(a.data))) return false;
        out.assign((size_t)a.num, 0);
        if (a.num && !m_.Read(a.data, out.data(), (size_t)a.num * 8)) continue;
        if (!m_.Read(at, &b, sizeof b)) return false;
        if (a.data == b.data && a.num == b.num) return true;  // not reallocated or resized meanwhile
    }
    out.clear();
    return false;
}

bool PlayerReader::Players(const std::vector<uintptr_t>& gameStates, std::vector<PlayerRecord>& out,
                           std::string& error) const {
    out.clear();
    bool anyState = false;
    for (uintptr_t gs : gameStates) {
        if (!r_.Alive(gs)) continue;
        anyState = true;
        std::vector<uintptr_t> states;
        if (!ReadPtrArray(gs + (uintptr_t)l_.playerArray, 256, states)) {
            error = "PlayerArray changed during every read (torn), try again";
            return false;
        }
        for (uintptr_t ps : states) {
            if (!r_.Alive(ps)) continue;
            PlayerRecord p;
            p.ps = ps;
            p.pc = m_.Rd<uintptr_t>(ps + (uintptr_t)l_.psOwner);
            if (!r_.Alive(p.pc) || !r_.InstanceOf(p.pc, l_.pcClass)) continue;
            if (!m_.ReadFString(p.pc + (uintptr_t)l_.pcUserId, p.steam64, 64) || p.steam64.empty()) continue;
            bool dup = false;
            for (auto& o : out) dup = dup || o.steam64 == p.steam64;
            if (dup) continue;
            m_.ReadFString(ps + (uintptr_t)l_.psName, p.name, 256);
            m_.ReadFString(ps + (uintptr_t)l_.psAddress, p.ip, 128);
            float ping = -1;
            if (m_.Get(ps + PlayerLayout::kExactPing, ping) && ping >= 0 && ping < 100000) p.ping = ping;
            uintptr_t pawn = m_.Rd<uintptr_t>(p.pc + (uintptr_t)l_.pcPawn);
            if (pawn && r_.Alive(pawn)) {
                p.pawn = pawn;
                if (r_.InstanceOf(pawn, l_.baseBPChar)) m_.ReadFString(pawn + (uintptr_t)l_.characterName, p.characterName, 256);
            }
            out.push_back(std::move(p));
        }
    }
    if (!anyState) {
        error = "no live GameState";
        return false;
    }
    return true;
}

bool PlayerReader::Location(uintptr_t pawn, conan::Location& out, std::string& error) const {
    if (!r_.Alive(pawn)) {
        error = "the player has no live character (dead, loading or logged out)";
        return false;
    }
    uintptr_t root = m_.Rd<uintptr_t>(pawn + (uintptr_t)l_.rootComponent);
    if (!r_.Alive(root)) {
        error = "the character has no root component";
        return false;
    }
    uintptr_t parent = 0;
    if (!m_.Get(root + (uintptr_t)l_.attachParent, parent) ||
        !m_.Read(root + (uintptr_t)l_.relativeLocation, out.rel, sizeof out.rel) ||
        !m_.Read(root + PlayerLayout::kComponentToWorldT, out.world, sizeof out.world)) {
        error = "location unreadable";
        return false;
    }
    out.attached = parent != 0;
    const double* v = out.attached ? out.world : out.rel;
    out.x = v[0];
    out.y = v[1];
    out.z = v[2];
    for (double d : {out.x, out.y, out.z})
        if (!(d > -1e8 && d < 1e8)) {
            error = "location out of range";
            return false;
        }
    return true;
}

bool PlayerReader::Stats(uintptr_t item, std::vector<std::pair<int, int32_t>>& ints,
                         std::vector<std::pair<int, float>>& floats) const {
    ints.clear();
    floats.clear();
    for (int which = 0; which < 2; which++) {
        struct {
            uintptr_t data;
            int32_t num, max;
        } a;
        uintptr_t at = item + (uintptr_t)(which ? PlayerLayout::kFloatStats : PlayerLayout::kIntStats);
        if (!m_.Read(at, &a, sizeof a)) return false;
        if (a.num < 0 || a.num > 256 || a.max < a.num || (a.num && !UE::Plausible(a.data))) return false;
        std::vector<uint8_t> buf((size_t)a.num * PlayerLayout::kStatStride);
        if (a.num && !m_.Read(a.data, buf.data(), buf.size())) return false;
        for (int32_t i = 0; i < a.num; i++) {
            const uint8_t* e = buf.data() + (size_t)i * PlayerLayout::kStatStride;
            int32_t id;
            memcpy(&id, e + PlayerLayout::kStatId, 4);
            if (id < 0 || id > 255) return false;
            if (which == 0) {
                int32_t v;
                memcpy(&v, e + PlayerLayout::kStatValue, 4);
                ints.push_back({id, v});
            } else {
                float v;
                memcpy(&v, e + PlayerLayout::kStatValue, 4);
                floats.push_back({id, v});
            }
        }
    }
    return true;
}

bool PlayerReader::OneInventory(uintptr_t inv, int which, std::vector<InvItem>& out, std::string& error) const {
    if (!inv) return true;  // e.g. no hotbar component on this pawn type
    if (!r_.Alive(inv)) {
        error = "inventory object is not alive";
        return false;
    }
    for (int attempt = 0; attempt < 2; attempt++) {
        std::vector<uintptr_t> slots;
        if (!ReadPtrArray(inv + (uintptr_t)l_.itemList, 4096, slots)) continue;
        std::vector<InvItem> items;
        bool torn = false;
        for (size_t s = 0; s < slots.size() && !torn; s++) {
            uintptr_t it = slots[s];
            if (!it) continue;  // empty slot (equipment is sparse)
            InvItem e;
            e.item = it;
            e.inventory = which;
            e.slot = (int)s;
            uintptr_t owner = 0;
            if (!m_.Get(it + (uintptr_t)l_.templateId, e.templateId) ||
                !m_.Get(it + (uintptr_t)l_.ownerInventory, owner) || owner != inv) {
                torn = true;  // freed or moved while we read: the item no longer points back
                break;
            }
            std::vector<std::pair<int, int32_t>> ints;
            std::vector<std::pair<int, float>> floats;
            if (!Stats(it, ints, floats)) {
                torn = true;
                break;
            }
            for (auto& kv : ints)
                if (kv.first == ItemStat::kStackSize) e.stack = kv.second;
            for (auto& kv : floats) {
                if (kv.first == ItemStat::kDurability) e.durability = kv.second;
                if (kv.first == ItemStat::kMaxDurability) e.maxDurability = kv.second;
            }
            if (e.stack < 1 || e.stack > 1000000 || e.templateId <= 0) {
                torn = true;
                break;
            }
            items.push_back(e);
        }
        std::vector<uintptr_t> again;
        if (torn || !ReadPtrArray(inv + (uintptr_t)l_.itemList, 4096, again) || again != slots) continue;
        out.insert(out.end(), items.begin(), items.end());
        return true;
    }
    error = "inventory changed during every read (torn)";
    return false;
}

bool PlayerReader::Inventory(uintptr_t pawn, std::vector<InvItem>& out, std::string& error) const {
    out.clear();
    if (!r_.Alive(pawn)) {
        error = "the player has no live character (dead, loading or logged out)";
        return false;
    }
    if (!r_.InstanceOf(pawn, l_.basePlayerChar)) {
        error = "the player is not controlling their character (class " + r_.ClassName(pawn) + ")";
        return false;
    }
    const uintptr_t inv[3] = {m_.Rd<uintptr_t>(pawn + (uintptr_t)l_.backpack), m_.Rd<uintptr_t>(pawn + (uintptr_t)l_.hotbar),
                              m_.Rd<uintptr_t>(pawn + (uintptr_t)l_.equipment)};
    for (int i = 0; i < 3; i++)
        if (!OneInventory(inv[i], i, out, error)) return false;
    return true;
}

}  // namespace conan
