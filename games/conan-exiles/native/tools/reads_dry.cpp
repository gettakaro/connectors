// reads_dry: runs the worker half of the read actions (conan/reads.*) against a LIVE Conan
// server from outside the process, read-only, with process_vm_readv on its pid. No game-thread
// call is made, so the startup self-checks stay "pending" (and ping is left out of the player
// DTO). Dev tool for re-pinning and diagnostics; never shipped.
//
//   reads_dry <pid> [--objobjects 0xc35a580] [--nameblocks 0xc2a5d40] [--player <steam64>]
//             [--record <file>] [--full]
//
// --record writes every byte range read (address, length, bytes) to <file>, a memory image the
// host tests can replay. --full prints the complete list outputs instead of samples.
#include "common.h"
#include "conan/reads.h"
#include "takaro/json_util.h"
#include "ue/mem.h"

#include <sys/uio.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace {

class RemoteMem : public UE::Mem {
public:
    RemoteMem(pid_t pid, FILE* rec) : pid_(pid), rec_(rec) {}
    bool Read(uintptr_t addr, void* out, size_t n) const override {
        if (n == 0) return true;
        if (addr < 0x10000 || addr + n > 0x800000000000ULL) return false;
        Count();
        iovec l{out, n}, r{(void*)addr, n};
        bool ok = process_vm_readv(pid_, &l, 1, &r, 1, 0) == (ssize_t)n;
        if (ok) Record(addr, out, n);
        return ok;
    }

private:
    void Record(uintptr_t addr, const void* data, size_t n) const {
        if (!rec_) return;
        std::lock_guard<std::mutex> g(mu_);
        uint64_t a = addr, len = n;
        fwrite(&a, 8, 1, rec_);
        fwrite(&len, 8, 1, rec_);
        fwrite(data, 1, n, rec_);
    }
    pid_t pid_;
    FILE* rec_;
    mutable std::mutex mu_;
};

std::string Sample(const JsonValue& v, size_t n) {
    if (v.type != JsonValue::Array) return takaro::JsonDump(v);
    std::string o = "[" + std::to_string(v.arr.size()) + " rows] ";
    for (size_t i = 0; i < v.arr.size() && i < n; i++) o += takaro::JsonDump(v.arr[i]) + "\n    ";
    return o;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: reads_dry <pid> [--player steam64] [--record file] [--full]\n");
        return 2;
    }
    pid_t pid = atoi(argv[1]);
    uintptr_t objObjects = 0xc35a580, nameBlocks = 0xc2a5d40;  // build 25639945 (non-PIE)
    std::string player, record;
    bool full = false;
    std::vector<std::string> find;
    for (int i = 2; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--objobjects" && i + 1 < argc) objObjects = strtoull(argv[++i], nullptr, 0);
        else if (a == "--nameblocks" && i + 1 < argc) nameBlocks = strtoull(argv[++i], nullptr, 0);
        else if (a == "--player" && i + 1 < argc) player = argv[++i];
        else if (a == "--record" && i + 1 < argc) record = argv[++i];
        else if (a == "--full") full = true;
        else if (a == "--find" && i + 1 < argc) find.push_back(argv[++i]);
    }
    FILE* rec = record.empty() ? nullptr : fopen(record.c_str(), "wb");
    RemoteMem mem(pid, rec);
    SetNativeLogPath("/dev/stderr");
    if (!find.empty()) {
        UE::Reflection r(mem, objObjects, nameBlocks);
        printf("scan: %lld objects\n", (long long)r.Scan(find));
        for (auto& f : find)
            for (uintptr_t ob : r.Named(f))
                printf("%s: %#lx class=%s alive=%d path=%s\n", f.c_str(), (unsigned long)ob, r.ClassName(ob).c_str(),
                       r.Alive(ob), r.Path(ob).c_str());
        return 0;
    }
    conan::ReadOptions o;
    o.mem = &mem;
    o.objObjects = objObjects;
    o.nameBlocks = nameBlocks;
    o.warmupThread = false;
    o.listWaitMs = 10;
    conan::ReadService rs(o);
    uint64_t t0 = NowNs();
    std::string status;
    rs.WarmupStep(status);
    FlushNativeLogs();
    printf("warm-up (%.0f ms): %s\n", (NowNs() - t0) / 1e6, status.c_str());
    JsonValue args;
    JsonParse("{}", args);
    for (const char* action : {"listItems", "listEntities", "listLocations", "getPlayers"}) {
        t0 = NowNs();
        auto r = rs.Execute(action, args);
        printf("%s (%.1f ms): %s\n", action, (NowNs() - t0) / 1e6,
               r.ok ? (full ? takaro::JsonDump(r.payload) : Sample(r.payload, 8)).c_str() : ("ERROR " + r.error).c_str());
    }
    if (!player.empty()) {
        JsonValue pa;
        JsonParse("{\"gameId\":\"" + player + "\"}", pa);
        for (const char* action : {"getPlayer", "getPlayerLocation", "getPlayerInventory"}) {
            t0 = NowNs();
            auto r = rs.Execute(action, pa);
            printf("%s (%.1f ms): %s\n", action, (NowNs() - t0) / 1e6,
                   r.ok ? takaro::JsonDump(r.payload).c_str() : ("ERROR " + r.error).c_str());
        }
    }
    FlushNativeLogs();
    printf("health: %s\n", rs.HealthJson().c_str());
    if (rec) fclose(rec);
    return 0;
}
