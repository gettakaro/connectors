// #202 command probe, game-side glue. Included into mod/src/world.cpp (inside its anonymous namespace, right before
// AddMsgDetour) only when the DLL is built with TAKARO_COMMAND_PROBE=1; release builds never see this file.
//
// Active only while <server>\takaro\command-probe.json exists and the server is the one game build whose
// ChatSystem::addMessage ABI was recovered (docs/research/native-commands/enshrouded.md). Everything on the server
// thread is memory reads plus queueing; file I/O happens on the probe's own thread.
// Its headers (probe_core.h, <fstream>, <mutex>, <sstream>, <thread>) are included at the top of world.cpp.
namespace cprobe {

const char kAbiBuild[] = "1273043";  // addMessage + processIncomingMessages disassembled on this build only

std::mutex g_mu;
std::shared_ptr<const probe::Catalog> g_cat;  // guarded by g_mu
std::deque<std::string> g_out;                // JSON lines for the writer thread, guarded by g_mu
uint64_t g_seq = 0;                           // server thread only
std::atomic<bool> g_started{false}, g_armed{false};

std::string TakaroPath(const char* name) { return PluginBaseDir() + "\\takaro\\" + name; }

uint64_t WallMs() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return (u.QuadPart - 116444736000000000ULL) / 10000;
}

void Emit(const std::string& line) {
    std::lock_guard<std::mutex> g(g_mu);
    if (g_out.size() < 1000) g_out.push_back(line);
}

void Writer() {
    std::string last;
    bool present = false;
    for (;;) {
        std::ifstream f(TakaroPath("command-probe.json"), std::ios::binary);
        if (!f) {
            if (present) {
                std::lock_guard<std::mutex> g(g_mu);
                g_cat.reset();
                PluginLog("command-probe: catalog removed, probe off");
            }
            present = false;
            last.clear();
        } else {
            std::stringstream ss;
            ss << f.rdbuf();
            std::string text = ss.str();
            if (!present || text != last) {
                present = true;
                last = text;
                probe::Catalog c;
                std::string err;
                std::lock_guard<std::mutex> g(g_mu);
                if (probe::ParseCatalog(text, c, err)) {
                    g_cat = std::make_shared<const probe::Catalog>(std::move(c));
                    PluginLog("command-probe: catalog revision '%s' loaded (%zu commands, available=%d)",
                              g_cat->revision.c_str(), g_cat->commands.size(), (int)g_cat->available);
                } else if (g_cat) {
                    // keep every claimed name private: the old claims stay, but nothing is accepted
                    auto keep = std::make_shared<probe::Catalog>(*g_cat);
                    keep->available = false;
                    g_cat = keep;
                    PluginLog("command-probe: catalog rejected (%s); previous claims kept, all refused", err.c_str());
                } else {
                    PluginLog("command-probe: catalog rejected (%s); probe off", err.c_str());
                }
            }
        }
        std::deque<std::string> q;
        {
            std::lock_guard<std::mutex> g(g_mu);
            q.swap(g_out);
        }
        if (!q.empty()) {
            std::ofstream o(TakaroPath("connector-state\\command-probe.jsonl"), std::ios::app | std::ios::binary);
            for (auto& l : q) o << l << "\n";
        }
        Sleep(500);
    }
}

void Start() {
    if (GameBuild() != kAbiBuild) {
        PluginLog("command-probe: not armed, game build %s is not the verified build %s", GameBuild().c_str(), kAbiBuild);
        return;
    }
    if (!L.chatArg8Value || !L.sessVerStride) {
        PluginLog("command-probe: not armed, gamedata lacks the 1.0 chat/session layout");
        return;
    }
    if (g_started.exchange(true)) return;
    g_armed = true;
    std::thread(Writer).detach();
    PluginLog("command-probe: armed for game build %s (waiting for takaro\\command-probe.json)", kAbiBuild);
}

// Sender of a player chat message, from the game's own tables only (persona names are never used).
probe::Sender ResolveSender(uint64_t server, uint32_t handle, uint64_t gameAccount, std::string& facts) {
    probe::Sender s;
    char b[200];
    snprintf(b, sizeof b, "\"handle\":\"0x%08x\",\"handleIndex\":%u,\"handleHigh\":\"0x%x\"", handle, handle & 0x3f,
             handle >> 6);
    facts = b;
    auto fail = [&](const char* why) {
        s.reason = why;
        return s;
    };
    if (!server) return fail("no-server");
    if (!handle) return fail("no-handle");
    if ((handle & 0x3f) >= 16) return fail("out-of-range");  // the server has 16 player slots
    int hit = -1, hits = 0;
    for (int i = 0; i < 16; i++)
        if (*(uint32_t*)(Slot(server, i) + L.chatHandleSlotField) == handle) {
            hit = i;
            hits++;
        }
    snprintf(b, sizeof b, ",\"slot\":%d,\"slotMatches\":%d", hit, hits);
    facts += b;
    if (hits == 0) return fail("stale-handle");
    if (hits > 1) return fail("ambiguous-slot");
    uint64_t slot = Slot(server, hit);
    uint8_t state = *(uint8_t*)(slot + kSlotState);
    uint32_t eid = *(uint32_t*)(slot + kSlotEid);
    int machine = (int)(*(uint32_t*)(slot + kSlotMachine) & 0x7f);
    // the game's own session record for this handle (same lookup the receive path does before addMessage)
    uint64_t mgr = *(uint64_t*)(server + L.sessMgr), acc = 0;
    if (mgr && Readable(mgr, L.sessBase)) {
        uint32_t ver = *(uint32_t*)(mgr + L.sessVer);
        uint64_t rec = mgr + L.sessBase + (uint64_t)ver * L.sessVerStride + (uint64_t)(handle & 0x3f) * L.sessRecStride;
        if (ver <= 1 && Readable(rec, L.sessRecAccount + 8) && *(uint32_t*)(rec + L.sessRecHandle) == handle)
            acc = *(uint64_t*)(rec + L.sessRecAccount);
    }
    snprintf(b, sizeof b, ",\"state\":%u,\"entity\":%u,\"machine\":%d,\"sessionAccount\":%s,\"accountMatchesGame\":%s",
             state, eid, machine, acc ? "true" : "false", acc && acc == gameAccount ? "true" : "false");
    facts += b;
    if (state != 3) return fail("not-logged-in");
    if (!eid) return fail("no-entity");
    if (!acc) return fail("no-session");
    if (acc != gameAccount) return fail("session-mismatch");
    s.machine = machine;
    int found = 0;
    for (auto& sm : PluginState::Get().OnlineMachines())
        if (sm.second == std::to_string(machine)) {
            s.steamId = sm.first;
            found++;
        }
    if (found == 0) return fail("no-steamid");
    if (found > 1) {
        s.steamId.clear();
        return fail("ambiguous-steamid");
    }
    s.valid = true;
    return s;
}

// Queues a notice for one machine only, on the server thread (mirrors WorldSendMessage's whisper path).
std::string PrivateReply(uint64_t server, int machine, uint32_t handle, uint64_t account, uint8_t type,
                         const std::string& text) {
    uint64_t chat = *(uint64_t*)(server + L.chatOff);
    if (machine < 0 || machine >= (int)L.chatMachines || !chat || !Readable(chat, 0x1e0)) return "no route";
    if (!*(uint8_t*)(chat + L.chatActive + (uint64_t)machine * L.chatMStride)) return "machine not active";
    uint64_t before = *(uint64_t*)(chat + L.chatCount);
    StringView sv{text.data(), text.size()};
    Opt64 o2{};
    uint8_t e[16] = {0};
    g_origAddMsg(e, chat, type, &sv, 0, 0, handle, account, &o2);
    uint64_t rec = *(uint64_t*)(chat + L.chatTail);
    if (e[0] || *(uint64_t*)(chat + L.chatCount) != before + 1 || !rec) return "not queued";
    uint32_t size = *(uint32_t*)(rec + L.chatRecLen) + L.chatRecSizeAdd;
    if (size & 3) size = size - (size & 3) + 4;
    int excluded = 0;
    for (int k = 0; k < (int)L.chatMachines && k < 32; k++) {
        if (k == machine || !*(uint8_t*)(chat + L.chatActive + (uint64_t)k * L.chatMStride)) continue;
        uint32_t* mask = (uint32_t*)(rec + L.chatRecMask);
        if (*mask & (1u << k)) continue;
        *mask |= (1u << k);
        *(uint32_t*)(chat + L.chatPending + (uint64_t)k * L.chatMStride) -= size;
        excluded++;
    }
    return "queued, excluded " + std::to_string(excluded);
}

// Called from AddMsgDetour for the player-chat call site only, on the server thread. True = suppress the message.
bool OnPlayerChat(uint8_t type, const StringView* text, uint32_t handle, uint64_t gameAccount) {
    if (!g_armed) return false;
    std::shared_ptr<const probe::Catalog> cat;
    {
        std::lock_guard<std::mutex> g(g_mu);
        cat = g_cat;
    }
    if (!cat) return false;
    uint64_t server = (uint64_t)g_server;
    std::string msg(text->ptr, (size_t)text->len), facts;
    probe::Sender sender = ResolveSender(server, handle, gameAccount, facts);
    probe::Decision d = probe::Decide(msg, cat.get(), WallMs(), sender);
    static const char* kKinds[] = {"pass", "invoke", "reject"};
    std::string id = BootId().substr(0, 8) + "-" + std::to_string(++g_seq);
    std::string line = "{\"at\":" + JsonStr(IsoNowUtc()) + ",\"id\":" + JsonStr(id) + ",\"catalog\":" +
                       JsonStr(cat->revision) + ",\"type\":" + std::to_string(type) + ",\"len\":" +
                       std::to_string(msg.size()) + ",\"hex\":\"" + probe::Hex(msg) + "\"," + facts +
                       ",\"senderValid\":" + (sender.valid ? "true" : "false") + ",\"senderReason\":" +
                       JsonStr(sender.reason) + ",\"steamId\":" + JsonStr(sender.steamId) + ",\"decision\":\"" +
                       kKinds[(int)d.kind] + "\",\"trigger\":" + JsonStr(d.trigger) + ",\"command\":" +
                       JsonStr(d.command) + ",\"args\":" + JsonStr(d.args) + ",\"reason\":" + JsonStr(d.reason);
    if (d.kind == probe::Kind::Pass) {
        Emit(line + "}");
        return false;
    }
    bool failed = d.kind == probe::Kind::Invoke && cat->failDuringSend;
    std::string receipt = d.kind == probe::Kind::Invoke ? (failed ? "failed" : "submitted") : "rejected";
    std::string reply = probe::ReplyText(d, id, failed);
    std::string route = sender.machine >= 0 ? PrivateReply(server, sender.machine, handle, gameAccount, 4, reply)
                                            : std::string("no verified sender, no reply");
    Emit(line + ",\"suppressed\":true,\"receipt\":{\"status\":\"" + receipt +
         "\",\"stub\":true},\"reply\":" + JsonStr(reply) + ",\"replyRoute\":" + JsonStr(route) + "}");
    PluginLog("command-probe: %s %s '%s' -> %s (%s)", id.c_str(), kKinds[(int)d.kind], d.trigger.c_str(),
              receipt.c_str(), route.c_str());
    return true;
}

}  // namespace cprobe
