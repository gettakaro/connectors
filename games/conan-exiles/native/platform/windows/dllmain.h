// Windows entry point. The DLL is a winmm.dll proxy placed next to ConanSandboxServer-Win64-Shipping.exe
// (a static import, not a KnownDLL, loaded before the engine starts; dbghelp is only a delay import on
// Conan). It forwards the three winmm functions the server imports to the system copy and, from
// DllMain(DLL_PROCESS_ATTACH), records the game (main) thread and starts StartConnector() on a worker
// thread, so nothing runs under the loader lock.
#pragma once

namespace winplat {

// Runs once on the worker thread: config (env, then ConanSandbox\Saved\Config\Takaro\takaro.json),
// pins::Resolve("windows", PeIdentity(), ...), MinHook on ProcessEvent when the build is verified,
// then the bridge with WinHttpTransport. Never throws; logs and stays inert on any failure.
void StartConnector();

}  // namespace winplat
