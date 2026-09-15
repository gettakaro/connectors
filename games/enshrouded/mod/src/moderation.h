// Pure (Windows-free, host-testable) helpers for the account-action handler's admin protection.
//
// handleAccountAction (0x683510 @1024233), Kick/Ban branch, per matching player slot (rdi = server + i*stride):
//   test byte [rdi+0x1cb],1 ; jne <return>      <- slot permission mask bit 0 = CanKickBan: target is left alone
//   cmp  sil,1              ; jne <kick tail>
//   ...
//   mov  edx,[rdi+0x1c4]    ; mov rcx,[r13+10h] ; call kickEnqueue
// So the game silently ignores kick and ban for players whose user group has canKickBan (e.g. "Admins").
// The plugin reads the three layout values from the handler's code and, for such a target, clears bit 0 for the
// duration of the synchronous handler call on the moderation thread, then restores it.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace moderation {

struct ProtectLayout {
    uint32_t protOff = 0;     // slot flags byte (0x1cb), bit 0 = CanKickBan
    uint32_t mhOff = 0;       // slot machine handle (0x1c4)
    uint32_t slotStride = 0;  // 0x2bb38
    bool ok() const { return protOff && mhOff && slotStride; }
};

inline bool MatchBytes(const uint8_t* p, const char* pat) {
    // pattern "F6 87 ?? ?? 00 00"
    for (size_t i = 0; *pat;) {
        while (*pat == ' ') pat++;
        if (!*pat) break;
        if (pat[0] != '?') {
            auto hex = [](char c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; };
            if (p[i] != (uint8_t)(hex(pat[0]) << 4 | hex(pat[1]))) return false;
        }
        pat += 2;
        i++;
    }
    return true;
}

// Scans the handler body [code, code+len) for the three anchors. Every anchor must match exactly once.
inline ProtectLayout ParseHandler(const uint8_t* code, size_t len) {
    ProtectLayout L;
    int nProt = 0, nMh = 0, nStride = 0;
    for (size_t i = 0; i + 17 <= len; i++) {
        const uint8_t* p = code + i;
        if (MatchBytes(p, "F6 87 ?? ?? 00 00 01 0F 85 ?? ?? ?? ?? 40 80 FE 01")) {
            memcpy(&L.protOff, p + 2, 4);
            nProt++;
        }
        if (MatchBytes(p, "8B 97 ?? ?? 00 00 49 8B 4D 10 E8")) {
            memcpy(&L.mhOff, p + 2, 4);
            nMh++;
        }
        if (MatchBytes(p, "48 81 C7 ?? ?? ?? ?? 48 83 FB 10")) {
            memcpy(&L.slotStride, p + 3, 4);
            nStride++;
        }
    }
    if (nProt != 1 || nMh != 1 || nStride != 1) return ProtectLayout{};
    return L;
}

// Index of the player slot (0..15) whose machine handle is `mh`, or -1. `server` points at the Server object.
inline int FindSlotByMachine(const uint8_t* server, const ProtectLayout& L, uint32_t mh) {
    if (!mh || !L.ok()) return -1;
    for (int i = 0; i < 16; i++) {
        uint32_t v;
        memcpy(&v, server + (size_t)i * L.slotStride + L.mhOff, 4);
        if (v == mh) return i;
    }
    return -1;
}

inline uint8_t* ProtByte(uint8_t* server, const ProtectLayout& L, int slot) {
    return server + (size_t)slot * L.slotStride + L.protOff;
}

}  // namespace moderation
