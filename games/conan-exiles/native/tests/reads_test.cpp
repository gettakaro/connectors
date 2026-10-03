// Host tests for the read actions (lane L2a): a fake Conan server built in an in-memory image
// from the reflection-dump fixture (tests/fixtures/reads-layout-25639945.json, the real class
// layouts of build 25639945), read through the same UE::Mem interface the library uses, plus the
// Linux safe self-reader against this process. Covers: name pool and object walk, reflection
// lookups, the PlayerLayout offsets against the dump, getPlayers/getPlayer/getPlayerLocation/
// getPlayerInventory DTOs, the catalogues (filters, duplicate names, string-table texts, region),
// torn reads, freed pointers, every startup self-check and its game-thread fallback.
#include "common.h"
#include "conan/catalogue.h"
#include "conan/player_snapshot.h"
#include "conan/reads.h"
#include "takaro/json_util.h"
#include "ue/mem.h"
#include "ue/safe_reflect.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <sys/mman.h>
#include <vector>

using namespace conan;

static int g_failed = 0, g_ran = 0;
#define CHECK(cond, ...)                                                \
    do {                                                                \
        g_ran++;                                                        \
        if (!(cond)) {                                                  \
            g_failed++;                                                 \
            printf("FAIL %s:%d: %s\n     ", __FILE__, __LINE__, #cond); \
            printf(__VA_ARGS__);                                        \
            printf("\n");                                               \
        }                                                               \
    } while (0)
#define EQ(a, b) CHECK((a) == (b), "got '%s'\n     want '%s'", std::string(a).c_str(), std::string(b).c_str())

static JsonValue J(const std::string& text) {
    JsonValue v;
    if (!JsonParse(text, v)) printf("bad test json: %s\n", text.c_str());
    return v;
}

// ------------------------------------------------------------------ in-memory image
class ImageMem : public UE::Mem {
public:
    bool Read(uintptr_t addr, void* out, size_t n) const override {
        Count();
        if (hook) hook(addr, n);
        auto it = regions.upper_bound(addr);
        if (it == regions.begin()) return false;
        --it;
        if (addr + n > it->first + it->second.size()) return false;
        memcpy(out, it->second.data() + (addr - it->first), n);
        return true;
    }
    uint8_t* At(uintptr_t addr) {
        auto it = regions.upper_bound(addr);
        --it;
        return it->second.data() + (addr - it->first);
    }
    std::map<uintptr_t, std::vector<uint8_t>> regions;
    mutable std::function<void(uintptr_t, size_t)> hook;  // called before every read (tearing tests)
};

// ------------------------------------------------------------------ fake UE world
struct World {
    ImageMem mem;
    uintptr_t next = 0x10000000;
    uintptr_t nameBlocks = 0, objObjects = 0, chunk = 0, block0 = 0;
    uint32_t cursor = 0;
    int32_t numObjects = 0;
    std::map<std::string, uint32_t> nameIdx;
    std::map<std::string, uintptr_t> classes, fieldClasses;
    uintptr_t metaClass = 0, metaFunction = 0, metaScriptStruct = 0, metaBPClass = 0, metaUDS = 0, metaDataTable = 0;
    std::map<uintptr_t, std::string> textOf;  // FText address -> what Conv_TextToString returns

    uintptr_t Alloc(size_t n, size_t align = 16) {
        next = (next + align - 1) & ~(uintptr_t)(align - 1);
        uintptr_t a = next;
        mem.regions[a] = std::vector<uint8_t>(n, 0);
        next += n + 64;  // gaps between regions: reads across them fail like unmapped memory
        return a;
    }
    template <typename T>
    void W(uintptr_t a, T v) {
        memcpy(mem.At(a), &v, sizeof v);
    }
    template <typename T>
    T R(uintptr_t a) {
        T v;
        memcpy(&v, mem.At(a), sizeof v);
        return v;
    }

    uint32_t Name(const std::string& s) {
        auto it = nameIdx.find(s);
        if (it != nameIdx.end()) return it->second;
        if (cursor & 1) cursor++;
        uint32_t idx = cursor / 2;
        uint16_t h = (uint16_t)(s.size() << 6);
        memcpy(mem.At(block0 + cursor), &h, 2);
        memcpy(mem.At(block0 + cursor + 2), s.data(), s.size());
        cursor += 2 + (uint32_t)s.size();
        if (cursor & 1) cursor++;
        W<uint32_t>(nameBlocks - 4, cursor);
        nameIdx[s] = idx;
        return idx;
    }
    void FNameAt(uintptr_t a, const std::string& s, uint32_t number = 0) {
        W<uint32_t>(a, Name(s));
        W<uint32_t>(a + 4, number);
    }

    World() {
        uintptr_t nb = Alloc(16 + 8 * 4);
        nameBlocks = nb + 16;
        block0 = Alloc(0x20000);
        W<uint32_t>(nameBlocks - 8, 0);  // CurrentBlock
        W<uintptr_t>(nameBlocks, block0);
        Name("None");
        objObjects = Alloc(32);
        uintptr_t chunks = Alloc(8 * 4);
        chunk = Alloc((size_t)65536 * 0x18);
        W<uintptr_t>(objObjects, chunks);
        W<uintptr_t>(chunks, chunk);
        // meta classes (their class is "Class")
        metaClass = Obj(0, "Class", 0, 0x100);
        W<uintptr_t>(metaClass + 0x10, metaClass);
        metaFunction = Obj(metaClass, "Function", 0, 0x100);
        metaScriptStruct = Obj(metaClass, "ScriptStruct", 0, 0x100);
        metaBPClass = Obj(metaClass, "BlueprintGeneratedClass", 0, 0x100);
        metaUDS = Obj(metaClass, "UserDefinedStruct", 0, 0x100);
        Obj(0, "Default__Placeholder", 0, 0x40, 0, true);  // a dead slot the walk must skip
    }

    // A UObject registered in the object array.
    uintptr_t Obj(uintptr_t cls, const std::string& name, uintptr_t outer, size_t size, uint32_t flags = 0,
                  bool dead = false, uint32_t number = 0) {
        uintptr_t o = Alloc(size < 0x40 ? 0x40 : size);
        int32_t idx = numObjects++;
        W<uint32_t>(o + 0x08, flags);
        W<int32_t>(o + 0x0C, idx);
        W<uintptr_t>(o + 0x10, cls);
        FNameAt(o + 0x18, name, number);
        W<uintptr_t>(o + 0x20, outer);
        uintptr_t item = chunk + (uintptr_t)idx * 0x18;
        W<uint32_t>(item + 4, dead ? 0x20000000 : 0);
        W<uintptr_t>(item + 8, o);
        W<int32_t>(objObjects + 8, numObjects);
        return o;
    }
    void Kill(uintptr_t o) { W<uint32_t>(chunk + (uintptr_t)R<int32_t>(o + 0x0C) * 0x18 + 4, 0x20000000); }

    uintptr_t Package(const std::string& path) { return Obj(0, path, 0, 0x40); }

    uintptr_t FieldClass(const std::string& type) {
        auto it = fieldClasses.find(type);
        if (it != fieldClasses.end()) return it->second;
        uintptr_t fc = Alloc(0x40);
        FNameAt(fc + 8, type);
        return fieldClasses[type] = fc;
    }
    void AddProp(uintptr_t st, const std::string& name, const std::string& type, int32_t offset, int32_t size) {
        uintptr_t p = Alloc(0x80);
        W<uintptr_t>(p + 0x08, FieldClass(type));
        FNameAt(p + 0x20, name);
        W<int32_t>(p + 0x30, size);
        W<int32_t>(p + 0x44, offset);
        // append to the end of the chain (order is irrelevant to the lookups)
        W<uintptr_t>(p + 0x18, R<uintptr_t>(st + 0x50));
        W<uintptr_t>(st + 0x50, p);
    }
    uintptr_t AddFunction(uintptr_t cls, const std::string& name, uint16_t parmsSize) {
        uintptr_t f = Obj(metaFunction, name, cls, 0xE0);
        W<uint16_t>(f + 0xB6, parmsSize);
        W<uintptr_t>(f + 0x28, R<uintptr_t>(cls + 0x48));
        W<uintptr_t>(cls + 0x48, f);
        return f;
    }
    uintptr_t Class(const std::string& name, const std::string& kind, uintptr_t super) {
        uintptr_t meta = kind == "BlueprintGeneratedClass" ? metaBPClass
                         : kind == "ScriptStruct"         ? metaScriptStruct
                         : kind == "UserDefinedStruct"    ? metaUDS
                                                          : metaClass;
        uintptr_t c = Obj(meta, name, Package("/Script/Fake"), 0x100);
        W<uintptr_t>(c + 0x40, super);
        classes[name] = c;
        return c;
    }

    // Builds every class of the fixture (and its super chain) with the real offsets.
    std::string LoadFixture(const std::string& path) {
        std::ifstream f(path);
        std::stringstream ss;
        ss << f.rdbuf();
        JsonValue doc;
        if (!JsonParse(ss.str(), doc)) return "fixture unreadable: " + path;
        const JsonValue* structs = doc.get("structs");
        for (auto& s : structs->arr) {
            std::vector<std::string> chain;
            for (auto& c : s.get("superChain")->arr) chain.push_back(c.str);
            uintptr_t super = 0;
            for (size_t i = chain.size(); i-- > 0;) {
                if (!classes.count(chain[i])) Class(chain[i], "Class", super);
                super = classes[chain[i]];
            }
            std::string name = s.get("name")->str;
            uintptr_t c = classes.count(name) ? classes[name] : Class(name, s.get("kind")->str, super);
            W<uintptr_t>(c + 0x40, super);
            for (auto& p : s.get("properties")->arr)
                AddProp(c, p.get("name")->str, p.get("type")->str, (int32_t)p.get("offset")->num, (int32_t)p.get("size")->num);
            for (auto& fn : s.get("functions")->arr) {
                uintptr_t f = AddFunction(c, fn.get("name")->str, (uint16_t)fn.get("parmsSize")->num);
                for (auto& p : fn.get("params")->arr)
                    AddProp(f, p.get("name")->str, p.get("type")->str, (int32_t)p.get("offset")->num,
                            (int32_t)p.get("size")->num);
            }
        }
        metaDataTable = classes["DataTable"];
        return "";
    }

    void FString(uintptr_t at, const std::string& s) {
        if (s.empty()) {
            W<uintptr_t>(at, 0);
            W<int32_t>(at + 8, 0);
            W<int32_t>(at + 12, 0);
            return;
        }
        uintptr_t d = Alloc((s.size() + 1) * 2);
        for (size_t i = 0; i < s.size(); i++) W<uint16_t>(d + 2 * i, (uint16_t)(unsigned char)s[i]);
        W<uintptr_t>(at, d);
        W<int32_t>(at + 8, (int32_t)s.size() + 1);
        W<int32_t>(at + 12, (int32_t)s.size() + 1);
    }
    // kind 0 plain, 1 string table, 2 unknown vtable
    void FText(uintptr_t at, const std::string& s, int kind = 0) {
        uintptr_t data = Alloc(0x40);
        if (kind == 0) {
            W<uintptr_t>(data, TextLayout::kPlainVtable);
            FString(data + 0x20, s);
        } else if (kind == 1) {
            W<uintptr_t>(data, TextLayout::kStringTableVtable);
            uintptr_t ref = Alloc(0x40), entry = Alloc(0x40);
            W<uintptr_t>(data + 0x18, ref);
            W<uintptr_t>(ref + 0x18, entry);
            FString(entry + 0x10, s);
        } else if (kind == 3) {  // a vtable the seeds do not know, laid out like the plain one
            W<uintptr_t>(data, 0xbeef000);
            FString(data + 0x20, s);
        } else {
            W<uintptr_t>(data, 0xdead000);
        }
        W<uintptr_t>(at, data);
        textOf[at] = s;
    }

    uintptr_t DataTable(const std::string& path, const std::string& rowStructName,
                        const std::vector<std::pair<std::string, uintptr_t>>& rows) {
        size_t dot = path.rfind('.');
        uintptr_t pkg = Package(path.substr(0, dot));
        uintptr_t t = Obj(metaDataTable, path.substr(dot + 1), pkg, 0x100);
        W<uintptr_t>(t + 0x28, classes[rowStructName]);
        uintptr_t elems = Alloc(rows.size() * 24 + 8);
        for (size_t i = 0; i < rows.size(); i++) {
            FNameAt(elems + i * 24, rows[i].first);
            W<uintptr_t>(elems + i * 24 + 8, rows[i].second);
        }
        W<uintptr_t>(t + 0x30, elems);
        W<int32_t>(t + 0x38, (int32_t)rows.size());
        W<int32_t>(t + 0x3C, (int32_t)rows.size());
        W<int32_t>(t + 0x58, (int32_t)rows.size());
        W<int32_t>(t + 0x60, -1);
        W<int32_t>(t + 0x64, 0);
        return t;
    }
};

static int32_t Off(World& w, const std::string& cls, const std::string& prop) {
    UE::Reflection r(w.mem, w.objObjects, w.nameBlocks);
    return r.Prop(w.classes[cls], prop).offset;
}

// A complete fake server: one GameState with two players (one mounted, one dead), inventories,
// items with stats, and the four DataTables.
struct Server {
    World w;
    uintptr_t gs = 0, ps1 = 0, pc1 = 0, pawn1 = 0, root1 = 0, ps2 = 0, pc2 = 0, pawn2 = 0, root2 = 0;
    uintptr_t backpack1 = 0, hotbar1 = 0, equip1 = 0;
    std::vector<uintptr_t> items;
    std::map<uintptr_t, std::pair<int32_t, std::pair<float, float>>> engineStats;  // what GetIntStat/GetFloatStat return
    float pingOverride = -1;  // GetPingInMilliseconds returns this instead of ExactPing when >= 0
    bool textLies = false;     // Conv_TextToString disagrees with the worker decode
    int gameJobs = 0;
    int textCalls = 0;

    uintptr_t Item(uintptr_t inv, int32_t templateId, int32_t stack, float dur, float maxDur,
                   const char* cls = "GameItem") {
        uintptr_t it = w.Obj(w.classes[cls], cls, 0, 600);
        w.W<int32_t>(it + Off(w, "GameItem", "TemplateId"), templateId);
        w.W<uintptr_t>(it + Off(w, "GameItem", "m_OwnerInventory"), inv);
        uintptr_t ints = w.Alloc(48 * 3), floats = w.Alloc(48 * 3);
        int ni = 0, nf = 0;
        auto stat = [&](uintptr_t arr, int i, int id, const void* v) {
            w.W<int32_t>(arr + 48 * i + 0xC, id);
            memcpy(w.mem.At(arr + 48 * i + 0x14), v, 4);
        };
        int32_t maxStack = 1000, created = 1791018252;
        stat(ints, ni++, 0, &maxStack);
        stat(ints, ni++, ItemStat::kStackSize, &stack);
        stat(ints, ni++, 22, &created);
        if (maxDur > 0) {
            stat(floats, nf++, ItemStat::kMaxDurability, &maxDur);
            stat(floats, nf++, ItemStat::kDurability, &dur);
        }
        float weight = 0.06f;
        stat(floats, nf++, 5, &weight);
        w.W<uintptr_t>(it + 0x128, ints);
        w.W<int32_t>(it + 0x130, ni);
        w.W<int32_t>(it + 0x134, 3);
        w.W<uintptr_t>(it + 0x138, floats);
        w.W<int32_t>(it + 0x140, nf);
        w.W<int32_t>(it + 0x144, 3);
        engineStats[it] = {stack, {maxDur > 0 ? dur : 0, maxDur > 0 ? maxDur : 0}};
        items.push_back(it);
        return it;
    }
    uintptr_t Inventory(const char* cls, std::vector<uintptr_t> slots) {
        uintptr_t inv = w.Obj(w.classes["ItemInventory"], cls, 0, 1000);
        uintptr_t data = w.Alloc(8 * (slots.size() + 1));
        w.W<uintptr_t>(inv + Off(w, "ItemInventory", "ItemList"), data);
        w.W<int32_t>(inv + Off(w, "ItemInventory", "ItemList") + 8, (int32_t)slots.size());
        w.W<int32_t>(inv + Off(w, "ItemInventory", "ItemList") + 12, (int32_t)slots.size());
        return inv;
    }
    void SetSlots(uintptr_t inv, const std::vector<uintptr_t>& slots) {
        uintptr_t data = w.R<uintptr_t>(inv + Off(w, "ItemInventory", "ItemList"));
        for (size_t i = 0; i < slots.size(); i++) w.W<uintptr_t>(data + 8 * i, slots[i]);
    }
    void Player(uintptr_t& ps, uintptr_t& pc, uintptr_t& pawn, uintptr_t& root, const std::string& steam,
                const std::string& name, const std::string& character, const std::string& ip, float ping, double x, double y,
                double z, const std::string& urlId) {
        ps = w.Obj(w.classes["PlayerState"], "PlayerState", 0, 1000);
        pc = w.Obj(w.classes["ConanPlayerController"], "FunCombat_PlayerController_C", 0, 6000);
        pawn = w.Obj(w.classes["BasePlayerChar_C"], "BasePlayerChar_C", 0, 16000);
        root = w.Obj(w.classes["SceneComponent"], "CollisionCylinder", 0, 700);
        w.W<uintptr_t>(ps + Off(w, "Actor", "Owner"), pc);
        w.FString(ps + Off(w, "PlayerState", "PlayerNamePrivate"), name);
        w.FString(ps + Off(w, "PlayerState", "SavedNetworkAddress"), ip);
        w.W<float>(ps + 824, ping);
        w.FString(pc + Off(w, "ConanPlayerController", "UserIDFromURLOptions"), urlId);
        // PlayerState.UniqueID: FUniqueNetIdRepl {vtable, TSharedPtr<FUniqueNetIdString>} -> id FString at +0x10
        uintptr_t netId = w.Alloc(0x40);
        w.W<uintptr_t>(netId, 0x16dbe80);
        w.FString(netId + 0x10, steam);
        w.FNameAt(netId + 0x20, "STEAM");
        int32_t uid = Off(w, "PlayerState", "UniqueID");
        w.W<uintptr_t>(ps + uid, 0x1272598);
        w.W<uintptr_t>(ps + uid + 8, netId);
        w.W<uintptr_t>(pc + Off(w, "Controller", "Pawn"), pawn);
        w.W<uintptr_t>(pawn + Off(w, "Actor", "RootComponent"), root);
        w.FString(pawn + Off(w, "BaseBPChar_C", "CharacterName"), character);
        double v[3] = {x, y, z};
        memcpy(w.mem.At(root + Off(w, "SceneComponent", "RelativeLocation")), v, 24);
        memcpy(w.mem.At(root + 0x210), v, 24);
    }

    std::string Build() {
        std::string err = w.LoadFixture("tests/fixtures/reads-layout-25639945.json");
        if (!err.empty()) return err;
        w.Obj(w.classes["KismetTextLibrary"], "Default__KismetTextLibrary", 0, 0x40, 0x10);
        uintptr_t level = w.Package("/Game/Maps/ConanSandbox/ConanSandbox");
        uintptr_t world = w.Obj(w.metaClass, "ConanSandbox", level, 0x40);
        uintptr_t persistent = w.Obj(w.metaClass, "PersistentLevel", world, 0x40);
        uintptr_t gsClass = w.Class("BaseGameState_C", "BlueprintGeneratedClass", w.classes["GameStateBase"]);
        w.Obj(gsClass, "Default__BaseGameState_C", 0, 900, 0x10);  // the CDO is not a live GameState
        gs = w.Obj(gsClass, "BaseGameState_C", persistent, 900, 0, false, 2147480827);
        Player(ps1, pc1, pawn1, root1, "76561198000735875", "Limon#67642", "werwerwer", "192.168.129.15", 55.5f, 101746.72,
               319894.28, -21582.48, "76561198000735875");
        Player(ps2, pc2, pawn2, root2, "76561198000000002", "Second", "rider", "10.0.0.2", 120.4f, 1, 2, 3,
               "A-1HFFLI28NN");  // a newer account: the login URL carries the Funcom id, not the Steam64
        // player 2 rides a mount: AttachParent set, RelativeLocation is local, ComponentToWorld is the world
        w.W<uintptr_t>(root2 + Off(w, "SceneComponent", "AttachParent"), root1);
        double world2[3] = {5000.5, -6000.25, -100};
        memcpy(w.mem.At(root2 + 0x210), world2, 24);
        // a dead third player state whose controller was freed: must be skipped
        uintptr_t ps3 = w.Obj(w.classes["PlayerState"], "PlayerState", 0, 1000);
        w.W<uintptr_t>(ps3 + Off(w, "Actor", "Owner"), 0x7f0000001000);  // unmapped
        uintptr_t arr = w.Alloc(8 * 4);
        w.W<uintptr_t>(arr, ps1);
        w.W<uintptr_t>(arr + 8, ps2);
        w.W<uintptr_t>(arr + 16, ps3);
        int32_t pa = Off(w, "GameStateBase", "PlayerArray");
        w.W<uintptr_t>(gs + pa, arr);
        w.W<int32_t>(gs + pa + 8, 3);
        w.W<int32_t>(gs + pa + 12, 4);

        backpack1 = Inventory("ItemInventory", {});
        hotbar1 = Inventory("ShortcutBarInventory", {});
        equip1 = Inventory("EquipmentInventory", {});
        uintptr_t data;
        // backpack: 5 + 1000 Stone in two stacks, an axe at 87.5 % durability; hotbar: a second axe
        // at full durability; equipment: sparse, the two fist weapons in slots 9 and 10
        std::vector<uintptr_t> bp = {Item(backpack1, 10001, 5, 0, 0), 0, Item(backpack1, 10001, 1000, 0, 0),
                                     Item(backpack1, 51001, 1, 175, 200)};
        data = w.Alloc(8 * bp.size());
        int32_t il = Off(w, "ItemInventory", "ItemList");
        w.W<uintptr_t>(backpack1 + il, data);
        w.W<int32_t>(backpack1 + il + 8, (int32_t)bp.size());
        w.W<int32_t>(backpack1 + il + 12, (int32_t)bp.size());
        SetSlots(backpack1, bp);
        // slot 1: the shortcut a wielded weapon leaves behind (live 2026-10-03: counted as a 3rd sword)
        std::vector<uintptr_t> hb = {Item(hotbar1, 51001, 1, 200, 200), Item(hotbar1, 51001, 1, 0, 0, "ShortcutRefItem")};
        data = w.Alloc(8 * 8);
        w.W<uintptr_t>(hotbar1 + il, data);
        w.W<int32_t>(hotbar1 + il + 8, 8);
        w.W<int32_t>(hotbar1 + il + 12, 8);
        SetSlots(hotbar1, hb);
        std::vector<uintptr_t> eq(16, 0);
        eq[9] = Item(equip1, 51205, 1, 0, 0);
        eq[10] = Item(equip1, 51204, 1, 0, 0);
        data = w.Alloc(8 * 16);
        w.W<uintptr_t>(equip1 + il, data);
        w.W<int32_t>(equip1 + il + 8, 16);
        w.W<int32_t>(equip1 + il + 12, 16);
        SetSlots(equip1, eq);
        w.W<uintptr_t>(pawn1 + Off(w, "BasePlayerChar_C", "BackpackInventory"), backpack1);
        w.W<uintptr_t>(pawn1 + Off(w, "BasePlayerChar_C", "ShortcutBarInventory"), hotbar1);
        w.W<uintptr_t>(pawn1 + Off(w, "BaseBPChar_C", "EquipmentInventory"), equip1);

        // ---- tables
        int32_t nameOff = Off(w, "ItemTableRow", "Name"), descOff = Off(w, "ItemTableRow", "ShortDesc");
        auto itemRow = [&](const std::string& name, const std::string& desc, int kind = 0) {
            uintptr_t row = w.Alloc(1048);
            w.FText(row + nameOff, name, kind);
            w.FText(row + descOff, desc);
            return row;
        };
        w.DataTable("/Game/Items/ItemTable.ItemTable", "ItemTableRow",
                    {{"10001", itemRow("Stone", "Roughly hewn chunk of stone")},
                     {"12001", itemRow("Plant Fiber", "Fiber")},
                     {"51001", itemRow("Stone Hatchet", "A crude axe")},
                     {"51706", itemRow("Abysmal Blade", "A sword")},
                     {"51709", itemRow("Abysmal Blade", "A sword")},
                     {"95916", itemRow("Anvil Keeper Greataxe", "Greataxe", 1)},
                     {"8934", itemRow("\"Guardian in the Unnamed City\" by Vladimir Shapovalov", "A framed painting")},
                     {"51204", itemRow("XX_Unarmed Right", "XX_ShortDesc")},
                     {"51205", itemRow("XX_Unarmed Left", "XX_ShortDesc")},
                     {"77777", itemRow("Dev Thing", "x")},
                     {"387", itemRow("5th Anniversary Cake", "XX_ShortDesc")},
                     {"60000", itemRow("Mystery Text", "Unknown vtable row", 2)},
                     {"60001", itemRow("Learned One", "x", 3)},
                     {"60002", itemRow("Learned Two", "y", 3)}});
        auto codeRow = [&](int32_t id) {
            uintptr_t row = w.Alloc(8);
            w.W<int32_t>(row, id);
            return row;
        };
        w.DataTable("/Game/Systems/Survival/Gathering/Gathering_v2/ItemNameToTemplateID.ItemNameToTemplateID",
                    "ItemNameToTemplateIDStruct",
                    {{"Stone", codeRow(10001)},
                     {"stone_alias", codeRow(10001)},
                     {"PlantFiber", codeRow(12001)},
                     {"DarkDregs_Sword", codeRow(51706)},
                     {"DarkDregs_Sword_Endgame", codeRow(51709)},
                     {"Hatchet_Stone", codeRow(51001)},
                     {"NotInItemTable", codeRow(99999)}});
        // the same-struct XP table must not be taken for the code table
        w.DataTable("/Game/Systems/Survival/Gathering/Gathering_v2/ItemNameToHarvestXPValue.ItemNameToHarvestXPValue",
                    "ItemNameToTemplateIDStruct", {{"Stone", codeRow(3)}});
        int32_t sName = Off(w, "SpawnTableRow", "Name"), sClass = Off(w, "SpawnTableRow", "NPCClass");
        auto spawnRow = [&](const std::string& name, const std::string& cls) {
            uintptr_t row = w.Alloc(456);
            w.FText(row + sName, name);
            w.FNameAt(row + sClass + 16, cls);
            return row;
        };
        w.DataTable("/Game/Systems/SpawnTable/SpawnDataTable.SpawnDataTable", "SpawnTableRow",
                    {{"Wildlife_Imp", spawnRow("Imp", "BP_NPC_Wildlife_Imp_C")},
                     {"Exile_Tanner_1_Cimmerian", spawnRow("Cimmerian Tanner I", "HumanoidNPCCharacter_C")},
                     {"XX_Elk_bc_3", spawnRow("Elk", "BP_Elk_C")},
                     {"Test_Dummy", spawnRow("", "BP_Dummy_C")}});
        int32_t mName = Off(w, "MapMarkerTableRow", "Name"), mRegion = Off(w, "MapMarkerTableRow", "Region"),
                mLoc = Off(w, "MapMarkerTableRow", "Location"), mRad = Off(w, "MapMarkerTableRow", "DiscoveryRadius");
        auto marker = [&](const std::string& name, const std::string& region, double x, double y, double z, float r) {
            uintptr_t row = w.Alloc(112);
            w.FText(row + mName, name);
            w.FNameAt(row + mRegion, region);
            double v[3] = {x, y, z};
            memcpy(w.mem.At(row + mLoc), v, 24);
            w.W<float>(row + mRad, r);
            return row;
        };
        w.DataTable("/Game/Systems/Map/MapMarkers_ConanSandbox.MapMarkers_ConanSandbox", "MapMarkerTableRow",
                    {{"0", marker("Slithering Beach", "ExiledLands", 215176.56, 117179.09, -19697.14, 5000)},
                     {"1", marker("Skulker's End", "ExiledLands", -128702.31, 216849.72, -20861.46, 7000)},
                     {"300", marker("The Gate of Arcadia", "IsleOfSiptah", 1, 2, 3, 4000)}});
        return "";
    }

    // The fake game thread: runs jobs inline and emulates the engine getters.
    GameCalls Calls() {
        GameCalls g;
        g.run = [this](std::function<void()> fn, int) {
            gameJobs++;
            fn();
            return true;
        };
        g.call = [this](uintptr_t obj, uintptr_t fn, void* parms) {
            std::string name = UE::Reflection(w.mem, w.objObjects, w.nameBlocks).ObjName(fn);
            uint8_t* p = (uint8_t*)parms;
            if (name == "GetPingInMilliseconds") {
                float v = pingOverride >= 0 ? pingOverride : w.R<float>(obj + 824);
                memcpy(p, &v, 4);
            } else if (name == "K2_GetActorLocation") {
                uintptr_t root = w.R<uintptr_t>(obj + Off(w, "Actor", "RootComponent"));
                memcpy(p, w.mem.At(root + 0x210), 24);
            } else if (name == "GetIntStat") {
                int32_t v = p[0] == ItemStat::kStackSize ? engineStats[obj].first : 0;
                memcpy(p + 4, &v, 4);
            } else if (name == "GetFloatStat") {
                float v = p[0] == ItemStat::kDurability      ? engineStats[obj].second.first
                          : p[0] == ItemStat::kMaxDurability ? engineStats[obj].second.second
                                                             : 0;
                memcpy(p + 4, &v, 4);
            } else if (name == "Conv_TextToString") {
                textCalls++;
                // Find which FText was copied in (by its ITextData pointer).
                uintptr_t data;
                memcpy(&data, p, 8);
                std::string s = "?";
                for (auto& t : w.textOf)
                    if (w.R<uintptr_t>(t.first) == data) s = t.second;
                if (textLies) s += " (engine)";
                // Return value FString in our own (host) memory, as the engine would allocate it.
                static std::list<std::u16string> keep;  // stable addresses
                keep.emplace_back(s.begin(), s.end());
                keep.back().push_back(u'\0');
                uintptr_t d = (uintptr_t)keep.back().data();
                int32_t n = (int32_t)keep.back().size();
                memcpy(p + 16, &d, 8);
                memcpy(p + 24, &n, 4);
                memcpy(p + 28, &n, 4);
            }
        };
        return g;
    }
};

// Host memory (the Conv_TextToString return buffer lives there) plus the image.
class HostAndImage : public UE::Mem {
public:
    explicit HostAndImage(ImageMem& img) : img_(img) {}
    bool Read(uintptr_t a, void* out, size_t n) const override {
        Count();
        if (img_.Read(a, out, n)) return true;
        return UE::SelfMem().Read(a, out, n);
    }
    ImageMem& img_;
};

static std::string Dump(const takaro::ActionResult& r) { return r.ok ? takaro::JsonDump(r.payload) : "ERROR " + r.error; }

static ReadService* NewService(Server& s, HostAndImage& mem) {
    ReadOptions o;
    o.mem = &mem;
    o.objObjects = s.w.objObjects;
    o.nameBlocks = s.w.nameBlocks;
    o.game = s.Calls();
    o.warmupThread = false;
    o.listWaitMs = 10;
    return new ReadService(o);
}

// ------------------------------------------------------------------ tests
static void TestSelfMem() {
    const UE::Mem& m = UE::SelfMem();
    uint64_t v = 0x1234567890abcdefULL, got = 0;
    CHECK(m.Read((uintptr_t)&v, &got, 8) && got == v, "self read");
    void* page = mmap(nullptr, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(!m.Read((uintptr_t)page, &got, 8), "unreadable page returns false, no crash");
    CHECK(!m.Read(8, &got, 8), "null page");
    uint64_t a = 1, b = 2, c = 3, oa = 0, ob = 0, oc = 0, op = 0;
    UE::Mem::Span spans[4] = {{(uintptr_t)&a, &oa, 8, false},
                              {(uintptr_t)page, &op, 8, false},
                              {(uintptr_t)&b, &ob, 8, false},
                              {(uintptr_t)&c, &oc, 8, false}};
    m.ReadMany(spans, 4);
    CHECK(spans[0].ok && !spans[1].ok && spans[2].ok && spans[3].ok && oa == 1 && ob == 2 && oc == 3,
          "batched reads continue after a bad span: %d %d %d %d", spans[0].ok, spans[1].ok, spans[2].ok, spans[3].ok);
    std::vector<uint64_t> many(3000), outs(3000);
    std::vector<UE::Mem::Span> sp(3000);
    for (size_t i = 0; i < many.size(); i++) {
        many[i] = i * 7;
        sp[i] = {(uintptr_t)&many[i], &outs[i], 8, false};
    }
    sp[1500].addr = (uintptr_t)page;
    m.ReadMany(sp.data(), sp.size());
    bool all = true;
    for (size_t i = 0; i < many.size(); i++) all = all && (i == 1500 ? !sp[i].ok : (sp[i].ok && outs[i] == i * 7));
    CHECK(all, "3000 spans over several batches with one hole");
    munmap(page, 4096);
}

static void TestLayout(Server& s) {
    UE::Reflection r(s.w.mem, s.w.objObjects, s.w.nameBlocks);
    CHECK(r.Scan({"GameStateBase", "PlayerState", "Actor", "Controller", "SceneComponent", "ConanPlayerController",
                  "BasePlayerChar_C", "BaseBPChar_C", "ItemInventory", "GameItem", "Pawn"}) > 0,
          "scan");
    PlayerLayout l;
    std::string err;
    CHECK(l.Resolve(r, err), "%s", err.c_str());
    // the offsets S3 proved live (El-Limon evidence spikes/S3-data.md), resolved by name
    CHECK(l.playerArray == 776 && l.psOwner == 360 && l.psName == 896 && l.psAddress == 832 && l.pcUserId == 2992 &&
              l.pcPawn == 816 && l.rootComponent == 456 && l.attachParent == 216 && l.relativeLocation == 336 &&
              l.backpack == 10392 && l.hotbar == 10128 && l.equipment == 6648 && l.itemList == 536 &&
              l.templateId == 240 && l.ownerInventory == 472 && l.characterName == 6688,
          "offsets from the dump fixture");
    CHECK(r.Type("BasePlayerChar_C") && r.IsA(r.Type("BasePlayerChar_C"), r.Type("Pawn")), "BP class chain");
    CHECK(r.Function(r.Type("BasePlayerChar_C"), "K2_GetActorLocation").ok(), "function found through supers");
    CHECK(r.Function(r.Type("GameItem"), "GetIntStat").parmsSize == 8, "parms size");
    CHECK(r.Instances(l.gameStateBase).size() == 1, "the live GameState, not the CDO");
    EQ(r.Path(s.gs), "/Game/Maps/ConanSandbox/ConanSandbox.ConanSandbox.PersistentLevel.BaseGameState_C_2147480826");
}

static void TestHelpers() {
    CHECK(IsInternalName("XX_Elk_bc_3") && IsInternalName("xxx Mask") && IsInternalName("  Dev Thing") &&
              IsInternalName("test_x") && IsInternalName("DEPRECATED sword") && IsInternalName("Do not use") &&
              IsInternalName("") && IsInternalName("   "),
          "internal names");
    CHECK(!IsInternalName("Stone") && !IsInternalName("Xochi Mask") && !IsInternalName("Develop") &&
              !IsInternalName("Testament") && !IsInternalName("X marks"),
          "real names stay");
    EQ(SortKey("\"Guardian\""), "guardian\"");
}

static void TestReads() {
    Server s;
    std::string err = s.Build();
    CHECK(err.empty(), "%s", err.c_str());
    if (!err.empty()) return;
    TestLayout(s);
    HostAndImage mem(s.w.mem);
    std::unique_ptr<ReadService> rs(NewService(s, mem));

    std::string status;
    CHECK(rs->WarmupStep(status), "warm-up completes: %s", status.c_str());

    // ---- catalogues
    auto r = rs->Execute("listItems", J("{}"));
    EQ(Dump(r),
       "[{\"code\":\"387\",\"name\":\"5th Anniversary Cake\"},"
       "{\"code\":\"DarkDregs_Sword\",\"name\":\"Abysmal Blade (#51706)\",\"description\":\"A sword\"},"
       "{\"code\":\"DarkDregs_Sword_Endgame\",\"name\":\"Abysmal Blade (#51709)\",\"description\":\"A sword\"},"
       "{\"code\":\"95916\",\"name\":\"Anvil Keeper Greataxe\",\"description\":\"Greataxe\"},"
       "{\"code\":\"8934\",\"name\":\"\\\"Guardian in the Unnamed City\\\" by Vladimir Shapovalov\",\"description\":\"A "
       "framed painting\"},"
       "{\"code\":\"60001\",\"name\":\"Learned One\",\"description\":\"x\"},"
       "{\"code\":\"60002\",\"name\":\"Learned Two\",\"description\":\"y\"},"
       "{\"code\":\"60000\",\"name\":\"Mystery Text\",\"description\":\"Unknown vtable row\"},"
       "{\"code\":\"PlantFiber\",\"name\":\"Plant Fiber\",\"description\":\"Fiber\"},"
       "{\"code\":\"Stone\",\"name\":\"Stone\",\"description\":\"Roughly hewn chunk of stone\"},"
       "{\"code\":\"Hatchet_Stone\",\"name\":\"Stone Hatchet\",\"description\":\"A crude axe\"}]");
    CHECK(rs->ResolveItemCode("stone") == 10001 && rs->ResolveItemCode("stone_alias") == 10001 &&
              rs->ResolveItemCode("51706") == 51706 && rs->ResolveItemCode("NotInItemTable") == 0 &&
              rs->ResolveItemCode("99999") == 0,
          "item code resolution for giveItem");
    r = rs->Execute("listEntities", J("[]"));
    EQ(Dump(r),
       "[{\"code\":\"Exile_Tanner_1_Cimmerian\",\"name\":\"Cimmerian Tanner I\",\"metadata\":{\"npcClass\":"
       "\"HumanoidNPCCharacter_C\"}},{\"code\":\"Wildlife_Imp\",\"name\":\"Imp\",\"metadata\":{\"npcClass\":"
       "\"BP_NPC_Wildlife_Imp_C\"}}]");
    CHECK(rs->EntityNameForClass("bp_npc_wildlife_imp_c") == "Imp" && rs->EntityNameForClass("BP_Nope_C").empty(),
          "entity name by NPC class (entity-killed fallback)");
    r = rs->Execute("listLocations", J("{}"));
    EQ(Dump(r),
       "[{\"code\":\"marker:0\",\"name\":\"Slithering Beach\",\"position\":{\"x\":215176.56,\"y\":117179.09,\"z\":-19697.14},"
       "\"radius\":5000},{\"code\":\"marker:1\",\"name\":\"Skulker's End\",\"position\":{\"x\":-128702.31,\"y\":216849.72,"
       "\"z\":-20861.46},\"radius\":7000}]");

    // ---- players
    r = rs->Execute("getPlayers", J("{}"));
    EQ(Dump(r),
       "[{\"gameId\":\"76561198000735875\",\"name\":\"Limon#67642\",\"steamId\":\"76561198000735875\",\"platformId\":"
       "\"steam:76561198000735875\",\"ip\":\"192.168.129.15\",\"ping\":56},{\"gameId\":\"76561198000000002\",\"name\":"
       "\"Second\",\"steamId\":\"76561198000000002\",\"platformId\":\"steam:76561198000000002\",\"ip\":\"10.0.0.2\","
       "\"ping\":120}]");
    std::string h = rs->HealthJson();
    CHECK(h.find("\"pingCheck\":\"passed\"") != std::string::npos && h.find("\"locationCheck\":\"passed\"") != std::string::npos &&
              h.find("\"textCheck\":\"passed\"") != std::string::npos,
          "%s", h.c_str());
    r = rs->Execute("getPlayer", J("{\"gameId\":\"76561198000735875\"}"));
    CHECK(Dump(r).find("\"name\":\"Limon#67642\"") != std::string::npos, "%s", Dump(r).c_str());
    r = rs->Execute("getPlayer", J("{\"player\":{\"platformId\":\"steam:76561198000000002\"}}"));
    CHECK(Dump(r).find("\"name\":\"Second\"") != std::string::npos, "nested platformId: %s", Dump(r).c_str());
    r = rs->Execute("getPlayer", J("{\"gameId\":\"werwerwer\",\"name\":null}"));
    CHECK(Dump(r).find("\"gameId\":\"76561198000735875\"") != std::string::npos, "by character name, explicit null");
    r = rs->Execute("getPlayer", J("{\"gameId\":\"76561198000099999\"}"));
    EQ(Dump(r), "{\"gameId\":\"76561198000099999\",\"name\":\"76561198000099999\",\"steamId\":\"76561198000099999\","
                "\"platformId\":\"steam:76561198000099999\"}");
    r = rs->Execute("getPlayer", J("{}"));
    CHECK(!r.ok, "no identifier");
    r = rs->Execute("getPlayer", J("{\"gameId\":\"A-1HFFLI28NN\"}"));
    CHECK(Dump(r).find("\"gameId\":\"76561198000000002\"") != std::string::npos,
          "a Funcom account id finds the player, whose gameId stays the Steam64: %s", Dump(r).c_str());
    // Conv_TextToString ran for the unknown vtables only (2 rows of 0xdead000, 1 to learn 0xbeef000) plus the
    // self-check samples, not for every text.
    CHECK(s.textCalls <= 2 + 1 + 12, "game-thread text calls: %d", s.textCalls);

    // ---- a new character's intro: no pawn and no position seen yet -> the controller's own root
    {
        uintptr_t pawn2 = s.w.R<uintptr_t>(s.pc2 + 816);
        uintptr_t pcRoot = s.w.Obj(s.w.classes["SceneComponent"], "TransformComponent0", 0, 700);
        double v[3] = {7.5, 8.5, -9.5};
        memcpy(s.w.mem.At(pcRoot + Off(s.w, "SceneComponent", "RelativeLocation")), v, 24);
        s.w.W<uintptr_t>(s.pc2 + Off(s.w, "Actor", "RootComponent"), pcRoot);
        s.w.W<uintptr_t>(s.pc2 + 816, 0);
        r = rs->Execute("getPlayerLocation", J("{\"gameId\":\"76561198000000002\"}"));
        EQ(Dump(r), "{\"x\":7.5,\"y\":8.5,\"z\":-9.5}");
        s.w.W<uintptr_t>(s.pc2 + 816, pawn2);
    }

    // ---- location
    r = rs->Execute("getPlayerLocation", J("{\"gameId\":\"76561198000735875\"}"));
    EQ(Dump(r), "{\"x\":101746.72,\"y\":319894.28,\"z\":-21582.48}");
    r = rs->Execute("getPlayerLocation", J("{\"player\":{\"gameId\":\"76561198000000002\"}}"));
    EQ(Dump(r), "{\"x\":5000.5,\"y\":-6000.25,\"z\":-100}");  // mounted: ComponentToWorld

    // ---- inventory
    r = rs->Execute("getPlayerInventory", J("{\"gameId\":\"76561198000735875\"}"));
    EQ(Dump(r),
       "[{\"code\":\"Stone\",\"name\":\"Stone\",\"amount\":1005},{\"code\":\"Hatchet_Stone\",\"name\":\"Stone Hatchet\","
       "\"amount\":1,\"quality\":\"88\"},{\"code\":\"Hatchet_Stone\",\"name\":\"Stone Hatchet\",\"amount\":1,\"quality\":"
       "\"100\"}]");
    h = rs->HealthJson();
    CHECK(h.find("\"statsCheck\":\"passed\"") != std::string::npos, "%s", h.c_str());
    r = rs->Execute("getPlayerInventory", J("{\"gameId\":\"76561198000000002\"}"));
    CHECK(Dump(r) == "[]", "player 2 has no inventories: %s", Dump(r).c_str());

    // ---- player 1 dies: no pawn. Location answers the last known position, inventory refuses.
    uintptr_t pawn = s.pawn1;
    s.w.W<uintptr_t>(s.pc1 + 816, 0);
    r = rs->Execute("getPlayerLocation", J("{\"gameId\":\"76561198000735875\"}"));
    EQ(Dump(r), "{\"x\":101746.72,\"y\":319894.28,\"z\":-21582.48}");
    r = rs->Execute("getPlayerInventory", J("{\"gameId\":\"76561198000735875\"}"));
    CHECK(!r.ok && r.error.find("no character") != std::string::npos, "%s", r.error.c_str());
    s.w.W<uintptr_t>(s.pc1 + 816, pawn);

    // ---- a torn PlayerArray: the array is reallocated between the first header read and the re-check
    {
        int32_t pa = 776;
        uintptr_t arrAt = s.gs + pa;
        uintptr_t oldData = s.w.R<uintptr_t>(arrAt);
        uintptr_t newData = s.w.Alloc(32);
        memcpy(s.w.mem.At(newData), s.w.mem.At(oldData), 24);
        int hits = 0;
        s.w.mem.hook = [&](uintptr_t a, size_t) {
            if (a == arrAt && ++hits == 2) s.w.W<uintptr_t>(arrAt, newData);  // just before the re-check
        };
        r = rs->Execute("getPlayers", J("{}"));
        s.w.mem.hook = nullptr;
        CHECK(r.ok && r.payload.arr.size() == 2 && hits >= 3, "torn PlayerArray is retried (%d reads): %s", hits,
              Dump(r).c_str());
    }
    // ---- a torn inventory item (freed: owner no longer points back) on every worker read -> game-thread copy
    {
        uintptr_t it = s.items[0];
        int32_t ownerOff = 472;
        uintptr_t real = s.w.R<uintptr_t>(it + ownerOff);
        int jobs0 = s.gameJobs;
        s.w.mem.hook = [&](uintptr_t a, size_t) {
            // While on the worker the item looks freed; the fake game thread restores it.
            if (a == it + ownerOff) s.w.W<uintptr_t>(it + ownerOff, s.gameJobs > jobs0 ? real : 0);
        };
        r = rs->Execute("getPlayerInventory", J("{\"gameId\":\"76561198000735875\"}"));
        s.w.mem.hook = nullptr;
        s.w.W<uintptr_t>(it + ownerOff, real);
        CHECK(r.ok && Dump(r).find("\"amount\":1005") != std::string::npos && s.gameJobs > jobs0,
              "torn inventory falls back to one game-thread copy: %s", Dump(r).c_str());
    }
    // ---- the GameState goes away (world reload): a clean error, then recovery
    s.w.Kill(s.gs);
    r = rs->Execute("getPlayers", J("{}"));
    CHECK(!r.ok && r.error.find("GameState") != std::string::npos, "%s", Dump(r).c_str());
}

static void TestSelfCheckFallbacks() {
    Server s;
    std::string err = s.Build();
    if (!err.empty()) return;
    HostAndImage mem(s.w.mem);
    s.pingOverride = 42.0f;  // the engine getter disagrees with ExactPing@824
    s.textLies = true;       // Conv_TextToString disagrees with the worker FText decode
    s.engineStats[s.items[0]].first = 6;  // GetIntStat disagrees with the decoded stack (5)
    std::unique_ptr<ReadService> rs(NewService(s, mem));
    std::string status;
    CHECK(rs->WarmupStep(status), "%s", status.c_str());
    auto r = rs->Execute("listItems", J("{}"));
    CHECK(Dump(r).find("\"name\":\"Stone (engine)\"") != std::string::npos,
          "failed text check: names come from Conv_TextToString: %s", Dump(r).substr(0, 300).c_str());
    r = rs->Execute("getPlayers", J("{}"));
    CHECK(Dump(r).find("\"ping\":42") != std::string::npos, "failed ping check: engine getter: %s", Dump(r).c_str());
    r = rs->Execute("getPlayerInventory", J("{\"gameId\":\"76561198000735875\"}"));
    CHECK(Dump(r).find("\"amount\":1006") != std::string::npos, "failed stats check: engine getters: %s",
          Dump(r).c_str());
    std::string h = rs->HealthJson();
    CHECK(h.find("\"pingCheck\":\"failed") != std::string::npos && h.find("\"textCheck\":\"failed") != std::string::npos &&
              h.find("\"statsCheck\":\"failed") != std::string::npos,
          "%s", h.c_str());
}

static void TestNotLoaded() {
    // An engine that has not loaded the world: every read answers a structured error, no crash.
    World w;
    HostAndImage mem(w.mem);
    ReadOptions o;
    o.mem = &mem;
    o.objObjects = w.objObjects;
    o.nameBlocks = w.nameBlocks;
    o.warmupThread = false;
    o.listWaitMs = 10;
    ReadService rs(o);
    std::string status;
    CHECK(!rs.WarmupStep(status), "%s", status.c_str());
    auto r = rs.Execute("getPlayers", J("{}"));
    CHECK(!r.ok && r.error.find("getPlayers:") == 0, "%s", r.error.c_str());
    r = rs.Execute("listItems", J("{}"));
    CHECK(!r.ok && r.error.find("still being built") != std::string::npos, "%s", r.error.c_str());
    r = rs.Execute("getPlayer", J("{\"gameId\":\"76561198000000001\"}"));
    CHECK(r.ok, "getPlayer always answers a player record: %s", Dump(r).c_str());
    // Garbage globals (pointing at unmapped memory) are just as harmless.
    ReadOptions g = o;
    g.mem = &UE::SelfMem();
    g.objObjects = 0x7f0000000000;
    g.nameBlocks = 0x7f0000100000;
    ReadService bad(g);
    r = bad.Execute("getPlayers", J("{}"));
    CHECK(!r.ok, "unmapped globals: %s", r.error.c_str());
}

int main() {
    TestSelfMem();
    TestHelpers();
    TestReads();
    TestSelfCheckFallbacks();
    TestNotLoaded();
    printf("reads tests: %d/%d checks passed\n", g_ran - g_failed, g_ran);
    return g_failed ? 1 : 0;
}
