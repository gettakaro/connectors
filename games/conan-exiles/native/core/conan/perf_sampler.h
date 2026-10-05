// Every 10 s: reads the engine frame counter (KismetSystemLibrary.GetFrameCount, one small
// game-thread job; its cost is counted like any other job) and records a GtStats sample, so
// health can report game-thread microseconds per engine tick.
#pragma once

namespace conan {

void StartPerfSampler(int periodMs = 10000);
void StopPerfSampler();

}  // namespace conan
