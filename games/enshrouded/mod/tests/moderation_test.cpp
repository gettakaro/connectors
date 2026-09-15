// Regression test for 2026-09-14: kick/ban of a canKickBan (Admins-group) player was silently ignored by the game.
// Checks the handler anchor parsing and the slot lookup/bypass helpers. With ENSHROUDED_EXE set, the parser is
// also run against the pinned server binary's real handler body (first 0x1000 bytes from 0x683510 @1024233, as the plugin scans).
#include "moderation.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <vector>
using namespace moderation;

static void put(std::vector<uint8_t>& v, size_t at, std::initializer_list<uint8_t> b) {
    size_t i = at;
    for (uint8_t x : b) v[i++] = x;
}

int main() {
    // Handler body shaped like build 1024233 (only the anchor instructions matter, rest is filler).
    std::vector<uint8_t> code(0x800, 0xCC);
    put(code, 0x3b8, {0x48, 0x81, 0xC7, 0x38, 0xBB, 0x02, 0x00, 0x48, 0x83, 0xFB, 0x10});             // add rdi,0x2bb38; cmp rbx,10h
    put(code, 0x3ca, {0xF6, 0x87, 0xCB, 0x01, 0x00, 0x00, 0x01, 0x0F, 0x85, 0x01, 0x04, 0x00, 0x00,  // test byte [rdi+1cbh],1; jne
                      0x40, 0x80, 0xFE, 0x01});                                                        // cmp sil,1
    put(code, 0x7c9, {0x8B, 0x97, 0xC4, 0x01, 0x00, 0x00, 0x49, 0x8B, 0x4D, 0x10, 0xE8});             // mov edx,[rdi+1c4h]; mov rcx,[r13+10h]; call
    ProtectLayout L = ParseHandler(code.data(), code.size());
    assert(L.ok() && L.protOff == 0x1cb && L.mhOff == 0x1c4 && L.slotStride == 0x2bb38);

    // Duplicate anchor -> refuse (self-check must fail closed).
    auto dup = code;
    put(dup, 0x10, {0x8B, 0x97, 0xC4, 0x01, 0x00, 0x00, 0x49, 0x8B, 0x4D, 0x10, 0xE8});
    assert(!ParseHandler(dup.data(), dup.size()).ok());
    // Missing skip test (game changed) -> refuse.
    auto miss = code;
    miss[0x3ca] = 0x90;
    assert(!ParseHandler(miss.data(), miss.size()).ok());

    // Fake server: slot 0 = Friends (mask 0x3e), slot 2 = Admins (mask 0x3f).
    std::vector<uint8_t> server(16 * L.slotStride + 0x400, 0);
    auto setSlot = [&](int i, uint32_t mh, uint8_t mask) {
        memcpy(&server[(size_t)i * L.slotStride + L.mhOff], &mh, 4);
        server[(size_t)i * L.slotStride + L.protOff] = mask;
    };
    setSlot(0, 257, 0x3e);
    setSlot(2, 385, 0x3f);
    assert(FindSlotByMachine(server.data(), L, 257) == 0);
    assert(FindSlotByMachine(server.data(), L, 385) == 2);
    assert(FindSlotByMachine(server.data(), L, 999) == -1);
    assert(FindSlotByMachine(server.data(), L, 0) == -1);
    assert((*ProtByte(server.data(), L, 0) & 1) == 0);  // Friends: game kicks normally
    assert((*ProtByte(server.data(), L, 2) & 1) == 1);  // Admins: game would skip -> plugin bypass needed

    if (const char* exe = getenv("ENSHROUDED_EXE")) {
        FILE* f = fopen(exe, "rb");
        assert(f);
        std::vector<uint8_t> img;
        uint8_t buf[65536];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0) img.insert(img.end(), buf, buf + n);
        fclose(f);
        // map RVA 0x683510 to a file offset via the section table
        uint32_t pe;
        memcpy(&pe, &img[0x3c], 4);
        uint16_t ns, so;
        memcpy(&ns, &img[pe + 6], 2);
        memcpy(&so, &img[pe + 20], 2);
        size_t fo = 0;
        for (int i = 0; i < ns; i++) {
            size_t s = pe + 24 + so + (size_t)i * 40;
            uint32_t vs, va, raw;
            memcpy(&vs, &img[s + 8], 4);
            memcpy(&va, &img[s + 12], 4);
            memcpy(&raw, &img[s + 20], 4);
            if (va <= 0x683510 && 0x683510 < va + vs) fo = 0x683510 - va + raw;
        }
        assert(fo);
        ProtectLayout R = ParseHandler(&img[fo], 0x1000);
        std::printf("real handler: protOff=0x%x mhOff=0x%x stride=0x%x\n", R.protOff, R.mhOff, R.slotStride);
        assert(R.ok() && R.protOff == 0x1cb && R.mhOff == 0x1c4 && R.slotStride == 0x2bb38);
    }
    std::cout << "moderation_test OK\n";
    return 0;
}
