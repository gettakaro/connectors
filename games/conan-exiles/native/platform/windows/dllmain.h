// Windows entry point (stub interface; the Windows lane fills it in). The proxy DLL (Enshrouded's
// dbghelp.dll pattern, or whichever DLL W0 picks) forwards every export to the system copy and,
// from DllMain(DLL_PROCESS_ATTACH) on a worker thread that waits for the loader lock to clear, runs
// the same sequence as platform/linux/main.cpp: config (env, then
// ConanSandbox\Saved\Config\Takaro\takaro.json), pins::Resolve("windows", PeIdentity(), ...),
// MinHook on ProcessEvent when the build is verified, then the bridge with WinHttpTransport.
#pragma once

namespace winplat {

// Called once from DllMain's worker thread. Never throws; logs and stays inert on any failure.
void StartConnector();

}  // namespace winplat
