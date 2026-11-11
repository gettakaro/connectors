#include "starrupture_chat_hooks.h"
#include "modloader_process_events.h"

#include <chrono>
#include <iostream>
#include <stdexcept>

namespace
{
int g_registered = 0;
int g_unregistered = 0;
int g_dispatched = 0;
void Observe(void*, void*, void*) { ++g_dispatched; }
void Register(TakaroRawProcessEventCallback) { ++g_registered; }
void Unregister(TakaroRawProcessEventCallback) { ++g_unregistered; }

void Expect(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}
}

int main()
{
    try
    {
        using namespace std::chrono_literals;
        const auto start = TakaroChatDedupe::Clock::time_point{};
        TakaroChatDedupe dedupe(250ms);

        Expect(dedupe.ShouldForward("player-a", "hello", start), "first chat message was suppressed");
        Expect(!dedupe.ShouldForward("player-a", "hello", start + 100ms), "duplicate chat message was forwarded");
        Expect(dedupe.ShouldForward("player-b", "hello", start + 100ms), "different player chat was suppressed");
        Expect(dedupe.ShouldForward("player-a", "different", start + 100ms), "different chat text was suppressed");
        Expect(dedupe.ShouldForward("player-a", "hello", start + 251ms), "expired duplicate was suppressed");
        Expect(!dedupe.ShouldForward("", "hello", start + 300ms), "chat without identity was forwarded");
        Expect(!dedupe.ShouldForward("player-a", "", start + 300ms), "empty chat was forwarded");
        dedupe.Reset();
        Expect(dedupe.ShouldForward("player-a", "hello", start + 301ms), "reset did not clear chat dedupe");

        Expect(ShouldCaptureTakaroAcceptedChat(true, "hello"), "accepted player chat was rejected");
        Expect(!ShouldCaptureTakaroAcceptedChat(false, "hello"), "server-originated chat was captured");
        Expect(!ShouldCaptureTakaroAcceptedChat(true, ""), "empty accepted chat was captured");

        TakaroProcessEventBus bus;
        Expect(bus.Subscribe(Observe, Register), "first ProcessEvent subscription failed");
        Expect(bus.Subscribe(Observe, Register), "duplicate ProcessEvent subscription was not idempotent");
        Expect(g_registered == 1, "ProcessEvent host was registered more than once");
        bus.Dispatch(nullptr, nullptr, nullptr);
        Expect(g_dispatched == 1, "ProcessEvent subscription was not dispatched exactly once");
        bus.Unsubscribe(Observe, Unregister);
        Expect(g_unregistered == 1 && bus.SubscriberCount() == 0, "ProcessEvent host was not unregistered");

        std::cout << "starrupture_chat_hooks_test: passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "starrupture_chat_hooks_test: " << error.what() << "\n";
        return 1;
    }
}
