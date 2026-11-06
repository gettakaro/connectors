#pragma once

#include <array>
#include <atomic>
#include <cstddef>

using TakaroRawProcessEventCallback = void (*)(void* object, void* function, void* parameters);
using TakaroProcessEventRegistrar = void (*)(TakaroRawProcessEventCallback callback);

void TakaroProcessEventBusDispatch(void* object, void* function, void* parameters);

// ProcessEvent also runs off the game thread, so Dispatch can race Subscribe.
// Fixed atomic slots mean a dispatch never walks storage that is being replaced.
class TakaroProcessEventBus
{
public:
    bool Subscribe(
        TakaroRawProcessEventCallback callback,
        TakaroProcessEventRegistrar registerWithHost);
    void Unsubscribe(
        TakaroRawProcessEventCallback callback,
        TakaroProcessEventRegistrar unregisterFromHost);
    void Dispatch(void* object, void* function, void* parameters) const;
    size_t SubscriberCount() const;

private:
    static constexpr size_t MaxSubscribers = 4;
    std::array<std::atomic<TakaroRawProcessEventCallback>, MaxSubscribers> callbacks_{};
};

bool SubscribeTakaroProcessEvent(TakaroRawProcessEventCallback callback);
void UnsubscribeTakaroProcessEvent(TakaroRawProcessEventCallback callback);
