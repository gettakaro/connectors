// Standalone transport smoke (no UE4SS): opens the Takaro WebSocket with the same Bridge + WinHTTP code
// as main.dll, sends an identify with deliberately bogus tokens, and prints what comes back. After the first
// identifyResponse it closes the connection once and expects the transport to reconnect with backoff (WIRE-5).
// It never uses a real token and cannot register a game server.
//   takaro-smoke.exe [wss-url] [seconds]
#include "../src/bridge.h"
#include "../src/log.h"

#include <cstdio>
#include <cstdlib>
#include <string>

int main(int argc, char** argv) {
    std::string url = argc > 1 ? argv[1] : "wss://connect.takaro.io/";
    int seconds = argc > 2 ? atoi(argv[2]) : 20;
    auto& b = native::Bridge::Get();
    if (!b.Connect(url, "")) {
        printf("SMOKE FAIL: bad url\n");
        return 2;
    }
    bool opened = false, closedOnce = false;
    int frames = 0, closes = 0, errors = 0, opens = 0;
    ULONGLONG closedAt = 0;
    uint64_t epoch = 0;
    ULONGLONG end = GetTickCount64() + (ULONGLONG)seconds * 1000;
    while (GetTickCount64() < end) {
        native::InboxItem it;
        if (!b.Poll(it)) {
            Sleep(20);
            continue;
        }
        std::string text = it.text.size() > 400 ? it.text.substr(0, 400) + "..." : it.text;
        printf("notice kind=%s epoch=0x%llx text=%s\n", it.kind.c_str(), (unsigned long long)it.epoch, text.c_str());
        fflush(stdout);
        if (it.kind == "open") {
            opened = true;
            opens++;
            epoch = it.epoch;
            if (closedAt) printf("reconnected %llu ms after the close\n", (unsigned long long)(GetTickCount64() - closedAt));
            const char* identify =
                "{\"type\":\"identify\",\"payload\":{\"identityToken\":\"scum-l2-smoke-bogus-identity\","
                "\"registrationToken\":\"bogus-not-a-real-token\",\"name\":\"scum-l2-transport-smoke\"}}";
            printf("send identify (bogus tokens): %s\n", native::QueueStatusName(b.Send(identify, "control", epoch)));
        } else if (it.kind == "frame") {
            frames++;
            if (!closedOnce && it.text.find("identifyResponse") != std::string::npos) {
                closedOnce = true;
                closedAt = GetTickCount64();
                printf("closing epoch 0x%llx to test reconnect\n", (unsigned long long)it.epoch);
                b.Close(it.epoch, "smoke reconnect test");
            }
        } else if (it.kind == "closed") {
            closes++;
        } else if (it.kind == "error") {
            errors++;
        }
    }
    printf("status %s\n", b.StatusJson().c_str());
    b.Disconnect();
    bool pass = opened && frames > 0 && opens >= 2 && closes >= 1;
    printf("SMOKE %s: opened=%d opens=%d frames=%d closes=%d errors=%d\n", pass ? "PASS" : "FAIL", opened, opens,
           frames, closes, errors);
    return pass ? 0 : 1;
}
