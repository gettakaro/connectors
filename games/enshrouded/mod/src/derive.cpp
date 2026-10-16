#include "derive.h"

#include "profile.h"
#include "scan.h"

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>

#include "../third_party/minhook/src/hde/hde64.h"

namespace derive {
namespace {

// ---- instruction view --------------------------------------------------------------------------------------------
enum Reg { RAX = 0, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8, R9, R10, R11, R12, R13, R14, R15, RIP = 100, NONE = -1 };

struct Ins {
    uint32_t rva = 0, len = 0;
    uint8_t op = 0, op2 = 0;  // op2 for 0F xx
    bool w = false;           // REX.W
    bool mem = false;         // ModRM addresses memory
    int reg = NONE;           // ModRM.reg (+REX.R), or the register of a B8+r mov
    int rm = NONE;            // ModRM.rm register when mod == 3
    int base = NONE;          // memory base register (RIP for rip-relative), NONE if no base
    int32_t disp = 0;
    uint64_t imm = 0;
    bool hasImm = false;
    uint8_t ext() const { return (uint8_t)(reg & 7); }  // /digit of group opcodes
};

const uint8_t* P(uint32_t rva) { return (const uint8_t*)scan::Ptr(rva); }

Ins Decode(uint32_t rva) {
    hde64s h;
    Ins i;
    i.rva = rva;
    i.len = hde64_disasm(P(rva), &h);
    if (h.flags & F_ERROR || i.len == 0) {
        i.len = i.len ? i.len : 1;
        return i;
    }
    i.op = h.opcode;
    i.op2 = h.opcode == 0x0F ? h.opcode2 : 0;
    i.w = h.rex_w;
    if (h.flags & F_IMM8) i.imm = (uint64_t)(int64_t)(int8_t)h.imm.imm8, i.hasImm = true;
    if (h.flags & F_IMM16) i.imm = h.imm.imm16, i.hasImm = true;
    if (h.flags & F_IMM32) i.imm = (uint64_t)(int64_t)(int32_t)h.imm.imm32, i.hasImm = true;
    if (h.flags & F_IMM64) i.imm = h.imm.imm64, i.hasImm = true;
    if ((h.opcode & 0xF8) == 0xB8 && !(h.flags & F_MODRM)) i.reg = (h.opcode & 7) | (h.rex_b << 3);
    if (h.flags & F_MODRM) {
        i.reg = h.modrm_reg | (h.rex_r << 3);
        if (h.modrm_mod == 3) {
            i.rm = h.modrm_rm | (h.rex_b << 3);
        } else {
            i.mem = true;
            if (h.modrm_rm == 4) i.base = (h.sib_base == 5 && h.modrm_mod == 0) ? NONE : (h.sib_base | (h.rex_b << 3));
            else if (h.modrm_rm == 5 && h.modrm_mod == 0) i.base = RIP;
            else i.base = h.modrm_rm | (h.rex_b << 3);
            if (h.flags & F_DISP8) i.disp = (int8_t)h.disp.disp8;
            if (h.flags & F_DISP32) i.disp = (int32_t)h.disp.disp32;
        }
    }
    return i;
}

uint32_t CallTarget(const Ins& i) { return i.op == 0xE8 ? (uint32_t)((int64_t)i.rva + i.len + (int32_t)i.imm) : 0; }
uint32_t RipTarget(const Ins& i) { return i.base == RIP ? (uint32_t)((int64_t)i.rva + i.len + i.disp) : 0; }

std::vector<Ins> DecodeRange(uint32_t a, uint32_t b) {
    std::vector<Ins> out;
    for (uint32_t p = a; p < b;) {
        Ins i = Decode(p);
        out.push_back(i);
        p += i.len;
    }
    return out;
}

std::vector<Ins> FunctionInsns(uint32_t root) {
    auto ch = scan::FunctionChunks(root);
    std::sort(ch.begin(), ch.end());
    std::vector<Ins> out;
    for (auto& c : ch) {
        auto v = DecodeRange(c.first, c.second);
        out.insert(out.end(), v.begin(), v.end());
    }
    return out;
}

std::vector<Ins> DecodeN(uint32_t a, int n) {
    std::vector<Ins> out;
    for (uint32_t p = a; (int)out.size() < n;) {
        Ins i = Decode(p);
        out.push_back(i);
        p += i.len;
    }
    return out;
}

// ---- anchors -----------------------------------------------------------------------------------------------------
struct Fail : std::runtime_error {
    using std::runtime_error::runtime_error;
};
void Need(bool ok, const char* what) {
    if (!ok) throw Fail(what);
}

std::vector<uint32_t> StringXrefs(const char* s) {
    uint32_t r = scan::FindCString(s);
    return r ? scan::FindLeaXrefs(r) : std::vector<uint32_t>{};
}
std::vector<uint32_t> XrefRoots(const char* s) {
    std::set<uint32_t> roots;
    for (uint32_t x : StringXrefs(s))
        if (uint32_t r = scan::FunctionRoot(x)) roots.insert(r);
    return {roots.begin(), roots.end()};
}
bool BytesContain(uint32_t rva, size_t n, const uint8_t* needle, size_t m) {
    const uint8_t* p = P(rva);
    for (size_t k = 0; k + m <= n; k++)
        if (memcmp(p + k, needle, m) == 0) return true;
    return false;
}

std::string Hex(uint64_t v) {
    char b[24];
    snprintf(b, sizeof b, "0x%llx", (unsigned long long)v);
    return b;
}

struct Builder {
    std::map<std::string, uint32_t> functions;
    std::map<std::string, uint32_t> callsites;
    std::map<std::string, std::string> layout;
    void L(const char* k, uint64_t v) { layout[k] = Hex(v); }
};

// ---- recipes (mirror of tools/derive-prototype, validated on builds 1004637 and 1267041) -------------------------
void Run(Builder& o) {
    // logger: most common first call after the log format strings; sink: its callee testing the text length
    std::map<uint32_t, int> votes;
    for (const char* a : {"[online] Added peer %k (%llu)", "[online] Removed peer %k", "[server] Remove Player '%k'",
                          "[online] Server connected to Steam successfully", "-------------- Session ----------------",
                          "[session] Player removed. Player handle: %k"})
        for (uint32_t x : StringXrefs(a))
            for (auto& i : DecodeN(x, 24))
                if (uint32_t t = CallTarget(i)) {
                    votes[t]++;
                    break;
                }
    uint32_t logger = 0;
    int best = 0;
    for (auto& v : votes)
        if (v.second > best) logger = v.first, best = v.second;
    Need(best >= 3, "logger");
    static const uint8_t kLen0[] = {0x49, 0x83, 0x78, 0x08, 0x00};  // cmp qword [r8+8], 0
    std::set<uint32_t> sinks;
    for (auto& i : FunctionInsns(logger))
        if (uint32_t t = CallTarget(i))
            if (scan::InText(t) && BytesContain(t, 16, kLen0, sizeof kLen0)) sinks.insert(t);
    Need(sinks.size() == 1, "logSink");
    o.functions["logSink"] = *sinks.begin();

    // session tick: unique caller of the stats dump
    auto st = XrefRoots("-------------- Session ----------------");
    Need(st.size() == 1, "session stats");
    auto stc = scan::FindDirectCalls(st[0]);
    Need(stc.size() == 1, "sessionTick");
    o.functions["sessionTick"] = scan::FunctionRoot(stc[0]);

    // console control handler passed to SetConsoleCtrlHandler
    uint32_t slot = scan::FindImportSlot("KERNEL32.dll", "SetConsoleCtrlHandler");
    std::set<uint32_t> ctrl;
    for (uint32_t site : slot ? scan::FindIndirectCalls(slot) : std::vector<uint32_t>{})
        for (uint32_t back = 7; back <= 0x14; back++)
            if (scan::MatchAt(site - back, "48 8D 0D")) {
                int32_t d;
                memcpy(&d, P(site - back + 3), 4);
                uint32_t h = (uint32_t)((int64_t)site - back + 7 + d);
                if (scan::InText(h) && scan::MatchAt(h, "48 83 EC 28")) ctrl.insert(h);
            }
    Need(ctrl.size() == 1, "ctrlHandler");
    o.functions["ctrlHandler"] = *ctrl.begin();

    // moderation: unban handler and its single caller
    auto ah = XrefRoots("[server] Account %s/%s was unbanned.");
    Need(ah.size() == 1, "accountAction");
    uint32_t handler = ah[0];
    o.functions["accountAction"] = handler;
    std::set<uint32_t> mc;
    for (uint32_t c : scan::FindDirectCalls(handler)) mc.insert(scan::FunctionRoot(c));
    Need(mc.size() == 1, "moderationSys");
    uint32_t modSys = *mc.begin();
    o.functions["moderationSys"] = modSys;
    auto head = DecodeN(handler, 24);
    const Ins* ev = nullptr;
    for (auto& i : head)
        if (i.op == 0x80 && i.ext() == 7 && i.mem && i.base == RDX && i.imm == 2) {
            ev = &i;
            break;
        }
    if (ev) {
        o.layout["accountActionAbi"] = "event";
        o.L("accountEventType", (uint32_t)ev->disp);
        const Ins* add = nullptr;
        for (auto& i : head)
            if ((i.op == 0x83 || i.op == 0x81) && i.ext() == 0 && i.rm == RDX && i.w) {
                add = &i;
                break;
            }
        Need(add, "accountEventAccount");
        o.L("accountEventAccount", add->imm);
    } else {
        o.layout["accountActionAbi"] = "args";
    }
    for (auto& i : DecodeN(modSys, 40))
        if (i.op == 0x83 && i.ext() == 7 && i.mem && i.base != RIP && i.w && i.imm == 0) {
            o.L("serverReadyB", (uint32_t)i.disp);
            break;
        }

    // updatePlayers: prints the login permissions; slot table from its imul/add
    auto up = XrefRoots("[server] Player '%k' logged in with Permissions:\n");
    Need(up.size() == 1, "updatePlayers");
    o.functions["updatePlayers"] = up[0];
    auto ui = FunctionInsns(up[0]);
    int slotreg = NONE;
    for (size_t k = 0; k < ui.size() && slotreg == NONE; k++) {
        auto& i = ui[k];
        if (i.op != 0x69 || i.imm <= 0x100 || i.mem) continue;
        for (size_t j = k + 1; j < ui.size() && j <= k + 3; j++)
            if (ui[j].op == 0x81 && ui[j].ext() == 0 && ui[j].rm == i.reg) {
                o.L("slotStride", i.imm);
                o.L("slotBase", ui[j].imm);
                slotreg = i.reg;
                break;
            }
    }
    Need(slotreg != NONE, "slot stride/base");
    auto sx = StringXrefs("[server] Player '%k' logged in with Permissions:\n");
    auto spx = StringXrefs("[server] Could not spawn Player Entity");
    Need(sx.size() == 1 && spx.size() == 1, "login/spawn anchors");
    uint32_t loginX = sx[0], spawnX = spx[0];
    int64_t bestD = 1 << 30;
    int32_t name = -1;
    for (auto& i : ui)
        if (i.op == 0x8D && i.mem && i.base == slotreg) {
            int64_t d = llabs((int64_t)i.rva - loginX);
            if (d <= 0x40 && d < bestD) bestD = d, name = i.disp;
        }
    Need(name >= 0, "slotName");
    o.L("slotName", (uint32_t)name);
    int32_t eid = -1;
    for (auto& i : ui)
        if (i.rva >= spawnX - 0x40 && i.rva < spawnX && (i.op == 0x83 || i.op == 0x81) && i.ext() == 7 && i.mem && i.base == slotreg)
            eid = i.disp;
    Need(eid >= 0, "slotEid");
    o.L("slotEid", (uint32_t)eid);
    std::map<int32_t, int> state;
    for (auto& i : ui)
        if (i.op == 0xC6 && i.mem && i.base == slotreg && (i.imm & 0xff) == 6) state[i.disp]++;
    Need(!state.empty(), "slotState");
    o.L("slotState", (uint32_t)std::max_element(state.begin(), state.end(), [](auto& a, auto& b) { return a.second < b.second; })->first);
    int32_t perms = -1;
    for (auto& i : ui)
        if (i.rva > loginX && i.rva < loginX + 0x200 && (i.op == 0xF6 || i.op == 0x84) && i.mem && i.base == slotreg) {
            perms = i.disp;
            break;
        }
    Need(perms >= 0, "slotPerms");
    o.L("slotPerms", (uint32_t)perms);

    // component read + world offset
    uint32_t readFn = 0;
    int readHits = 0;
    for (uint32_t f : scan::FindPattern("48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8D BA ?? ?? ?? 00 44 89 44 24 40 48 8B F1 "
                                        "48 8D 54 24 40 48 8B 0F 41 8B D9 48 81 C1 88 00 00 00 E8 ?? ?? ?? ?? 4C 8B C0 44 8B CB 48 8B D7 48 8B CE E8",
                                        4)) {
        uint32_t core = scan::CallDest(f + 0x41);
        if (core && scan::MatchAt(core, "48 89 5C 24 08 48 89 74 24 10 44 89 4C 24 20 57 48 83 EC 20 48 8D BA 48 01 00 00")) {
            readFn = f;
            readHits++;
        }
    }
    Need(readHits == 1, "getComponentRead");
    o.functions["getComponentRead"] = readFn;
    for (size_t k = 1; k < ui.size(); k++)
        if (CallTarget(ui[k]) == readFn) {
            auto& p = ui[k - 1];
            if (p.op == 0x8B && p.w && p.reg == RDX && p.mem && p.base != RIP) {
                o.L("worldOff", (uint32_t)p.disp);
                break;
            }
        }
    Need(o.layout.count("worldOff"), "worldOff");
    std::vector<uint32_t> adds;
    for (uint32_t a : scan::FindPattern("48 83 EC 38 41 0F 10 01 48 81 C1 ?? ?? ?? 00 4C 8D 4C 24 20", 8))
        for (uint32_t c : scan::FindDirectCalls(a))
            if (scan::FunctionRoot(c) == up[0]) {
                adds.push_back(a);
                break;
            }
    Need(adds.size() == 1, "addComponent");
    o.functions["addComponent"] = adds[0];

    // give items
    uint32_t inv = profile::JobFunction("inventory_actions");
    Need(inv, "inventoryActions");
    o.functions["inventoryActions"] = inv;
    auto ii = FunctionInsns(inv);
    for (auto& i : ii)
        if (uint32_t t = CallTarget(i))
            if (scan::InText(t) && scan::MatchAt(t, "8D 41 FF 4C 8B C2 3D FE 03 00 00")) {
                o.functions["isAdminEntity"] = t;
                o.callsites["inventoryAdminGateRet"] = i.rva + i.len;
                break;
            }
    Need(o.functions.count("isAdminEntity"), "isAdminEntity");
    for (size_t k = 0; k + 1 < ii.size(); k++) {
        auto& a = ii[k];
        auto& b = ii[k + 1];
        if (a.op == 0x8B && !a.w && a.reg == RCX && a.mem && a.base != RIP && b.op == 0x39 && b.mem && b.base == RAX && b.reg == RCX) {
            o.L("piCreateVer", (uint32_t)a.disp);
            o.L("scCreateVer", (uint32_t)b.disp);
            break;
        }
    }
    Need(o.layout.count("piCreateVer"), "create-action versions");

    // chat: the queueMessage call in the incoming-message handler
    auto pr = XrefRoots("[server] Received invalid player message on channel '%s'.");
    Need(pr.size() == 1, "processIncoming");
    auto pi = FunctionInsns(pr[0]);
    static const uint8_t k400[] = {0x00, 0x04, 0x00, 0x00};
    std::vector<const Ins*> qm;
    for (auto& i : pi)
        if (uint32_t t = CallTarget(i))
            if (scan::InText(t) && BytesContain(t, 0x30, k400, sizeof k400)) qm.push_back(&i);
    Need(qm.size() == 1, "chatAddMessage");
    uint32_t addMsg = CallTarget(*qm[0]);
    o.functions["chatAddMessage"] = addMsg;
    o.callsites["chatReceiveRet"] = qm[0]->rva + qm[0]->len;
    for (auto& i : pi)
        if (i.rva > qm[0]->rva - 0x60 && i.rva < qm[0]->rva && i.op == 0x8B && i.w && i.reg == RDX && i.mem && i.base != RIP)
            o.L("chatOff", (uint32_t)i.disp);  // last one before the call
    Need(o.layout.count("chatOff"), "chatOff");
    // sender session lookup (1.0+): the call right before the 'unknown Source' log
    auto cx = StringXrefs("Received chat message from unknown Source. Handle '%k'");
    const Ins* look = nullptr;
    if (cx.size() == 1)
        for (auto& i : pi)
            if (i.rva > cx[0] - 0x40 && i.rva < cx[0] && CallTarget(i)) look = &i;
    if (look) {
        size_t li = look - pi.data();
        for (size_t k = li; k-- > 0 && k + 40 > li;)
            if (pi[k].op == 0x8B && pi[k].w && pi[k].reg == RDX && pi[k].mem && pi[k].base != RIP) {
                o.L("sessionMgr", (uint32_t)pi[k].disp);
                break;
            }
        auto sl = DecodeN(CallTarget(*look), 40);
        std::vector<uint64_t> imuls;
        for (auto& i : sl)
            if (i.op == 0x69) imuls.push_back(i.imm);
        Need(imuls.size() >= 2, "session strides");
        o.L("sessionVersionStride", imuls[0]);
        o.L("sessionRecordStride", imuls[1]);
        for (auto& i : sl) {
            if (i.op == 0x39 && i.mem && i.reg == R8) {
                o.L("sessionRecordHandle", (uint32_t)i.disp);
                o.L("sessionRecordAccount", (uint32_t)i.disp + 0x20);  // sender info copied from the record: account at +0x20
                break;
            }
            if (i.op == 0x8B && !i.w && i.reg == RAX && i.mem && i.base == RDX) o.L("sessionVersion", (uint32_t)i.disp);
        }
        for (auto& i : sl)
            if ((i.op == 0x83 || i.op == 0x81) && i.ext() == 0 && i.rm != NONE && i.imm < 0x100) {
                o.L("sessionBase", i.imm);
                break;
            }
        Need(o.layout.count("sessionRecordHandle"), "session record handle");
    }
    auto ai = FunctionInsns(addMsg);
    int optChecks = 0;
    for (auto& i : ai)
        if (i.op == 0x38 && i.mem && i.base == RAX && i.disp == 8) optChecks++;
    o.L("chatArg8IsValue", optChecks == 1 ? 1 : 0);
    int creg = NONE;
    for (size_t k = 0; k < ai.size() && k < 16 && creg == NONE; k++) {
        auto& i = ai[k];
        if (i.op == 0x8B && i.w && i.rm == RDX) creg = i.reg;
        if (i.op == 0x89 && i.w && !i.mem && i.reg == RDX) creg = i.rm;
    }
    Need(creg != NONE, "chat system register");
    for (auto& i : ai)
        if (i.op == 0xFF && i.ext() == 0 && i.w && i.mem && i.base == creg) {
            o.L("chatCount", (uint32_t)i.disp);
            o.L("chatTail", (uint32_t)i.disp - 8);
            break;
        }
    Need(o.layout.count("chatCount"), "chatCount");
    for (size_t k = 0; k < ai.size(); k++) {
        auto& i = ai[k];
        if (!(i.op == 0x8D && i.mem && i.reg == RAX && i.base == creg)) continue;
        const Ins *c = nullptr, *a = nullptr;
        for (size_t j = k + 1; j < ai.size() && j <= k + 12; j++) {
            if (!c && ai[j].op == 0x38 && ai[j].mem && ai[j].base == RAX) c = &ai[j];
            if (!a && ai[j].op == 0x01 && ai[j].mem && ai[j].base == RAX && ai[j].reg == R8) a = &ai[j];
        }
        if (c && a) {
            o.L("chatMachineActive", (uint32_t)(i.disp + c->disp));
            o.L("chatMachinePending", (uint32_t)(i.disp + a->disp));
        }
        break;
    }
    Need(o.layout.count("chatMachineActive"), "chat machine table");
    for (size_t k = 1; k < ai.size(); k++) {
        auto& p = ai[k - 1];
        auto& i = ai[k];
        if ((i.op == 0x83 || i.op == 0x81) && i.ext() == 0 && i.rm == R8 && i.w && p.op == 0x8B && p.reg == R8 && p.mem && p.disp == 8) {
            o.L("chatRecSizeAdd", i.imm);
            break;
        }
    }
    Need(o.layout.count("chatRecSizeAdd"), "chatRecSizeAdd");

    // combat: job, context getter, event views
    uint32_t cb = profile::JobFunction("combat_experience_source");
    Need(cb, "combatXpSource");
    o.functions["combatXpSource"] = cb;
    auto ci = FunctionInsns(cb);
    size_t fc = 0;
    while (fc < ci.size() && !CallTarget(ci[fc])) fc++;
    Need(fc < ci.size(), "combatContext");
    o.functions["combatContext"] = CallTarget(ci[fc]);
    int32_t ctxBase = INT32_MIN;
    for (size_t k = 0; k < fc; k++) {
        if ((ci[k].op & 0xF8) == 0xB8 && ci[k].reg == R8 && ci[k].hasImm) o.L("combatCtxSize", ci[k].imm);
        if (ci[k].op == 0x8D && ci[k].mem && ci[k].reg == RDX && ci[k].base == RBP) ctxBase = ci[k].disp;
    }
    Need(o.layout.count("combatCtxSize") && ctxBase != INT32_MIN, "combat context");
    std::vector<int32_t> views;
    for (size_t k = 0; k < ci.size(); k++) {
        auto& i = ci[k];
        if (!(i.op == 0x8B && i.w && i.mem && i.base == RBP)) continue;
        for (size_t j = k + 1; j < ci.size() && j <= k + 3; j++)
            if (ci[j].op == 0x0F && ci[j].op2 == 0xB7 && ci[j].mem && ci[j].base == i.reg) {
                views.push_back(i.disp - ctxBase);
                o.L("viewFirstPage", (uint32_t)ci[j].disp);
                break;
            }
    }
    o.L("eventViewPaged", views.empty() ? 0 : 1);
    if (!views.empty()) {
        Need(views.size() >= 2, "event views");
        o.L("combatCtxHits", (uint32_t)views[0]);
        o.L("combatCtxDied", (uint32_t)views[1]);
        std::vector<int32_t> strides;
        for (size_t k = 1; k < ci.size(); k++)
            if (ci[k].op == 0x81 && ci[k].ext() == 7 && ci[k].rm == RCX && ci[k].imm == 0x7f8 && ci[k - 1].op == 0x8D && ci[k - 1].mem)
                strides.push_back(ci[k - 1].disp);
        Need(strides.size() >= 2, "event strides");
        o.L("hitStride", (uint32_t)strides[0]);
        o.L("diedStride", (uint32_t)strides[1]);
    }
    auto tg = scan::FindPattern("48 89 5C 24 08 57 48 83 EC 20 48 8B 01 48 8B DA 48 8D 54 24 40 44 89 44 24 40 48 8B 78 30 48 8D 8F 48 01 00 00 E8", 3);
    Need(tg.size() == 1, "entityTemplateGuid");
    o.functions["entityTemplateGuid"] = tg[0];

    // character name (optional): the lookup the ban code runs before 'Unable to find character name'
    auto ux = StringXrefs("[server] Unable to find character name for '%s'");
    if (ux.size() == 1) {
        auto hi = FunctionInsns(handler);
        const Ins* cn = nullptr;
        for (auto& i : hi)
            if (i.rva > ux[0] - 0x60 && i.rva < ux[0] && CallTarget(i)) cn = &i;
        if (cn) {
            size_t k = cn - hi.data();
            int32_t table = -1;
            for (size_t j = k >= 4 ? k - 4 : 0; j < k; j++)
                if (hi[j].op == 0x8B && hi[j].w && hi[j].reg == RDX && hi[j].mem && hi[j].base != RIP) table = hi[j].disp;
            size_t g = k;
            while (g > 0 && CallTarget(hi[g - 1]) != readFn) g--;
            uint32_t hash = 0;
            if (g > 0)
                for (size_t j = g >= 5 ? g - 5 : 0; j < g - 1; j++)
                    if (hi[j].op == 0x8B && !hi[j].w && hi[j].reg == R8 && hi[j].base == RIP) memcpy(&hash, P(RipTarget(hi[j])), 4);
            if (table >= 0 && hash) {
                o.functions["characterName"] = CallTarget(*cn);
                o.L("characterTable", (uint32_t)table);
                o.L("characterComponent", hash);
            }
        }
    }
}

std::string VerifyBytes(uint32_t rva) {
    std::string s;
    char b[4];
    for (int k = 0; k < 16; k++) {
        snprintf(b, sizeof b, "%s%02X", k ? " " : "", P(rva)[k]);
        s += b;
    }
    return s;
}

JsonValue Str(const std::string& s) {
    JsonValue v;
    v.type = JsonValue::String;
    v.str = s;
    return v;
}
JsonValue Obj() {
    JsonValue v;
    v.type = JsonValue::Object;
    return v;
}

}  // namespace

Result Derive(const JsonValue* templateLayout) {
    Result r;
    Builder b;
    try {
        Run(b);
    } catch (const Fail& f) {
        r.error = f.what();
        return r;
    }
    r.functions = Obj();
    for (auto& f : b.functions) {
        JsonValue e = Obj();
        e.obj.push_back({"rva", Str(Hex(f.second))});
        e.obj.push_back({"verify", Str(VerifyBytes(f.second))});
        r.functions.obj.push_back({f.first, e});
    }
    r.callsites = Obj();
    for (auto& c : b.callsites) r.callsites.obj.push_back({c.first, Str(Hex(c.second))});
    r.layout = Obj();
    if (templateLayout && templateLayout->type == JsonValue::Object)
        for (auto& kv : templateLayout->obj)
            if (!b.layout.count(kv.first)) r.layout.obj.push_back(kv);
    for (auto& l : b.layout) r.layout.obj.push_back({l.first, Str(l.second)});
    r.derivedValues = (int)(b.functions.size() + b.callsites.size() + b.layout.size());
    r.ok = true;
    return r;
}

}  // namespace derive
