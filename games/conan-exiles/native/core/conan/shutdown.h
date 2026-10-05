// shutdown (lane L2b): Conan has no reflected countdown shutdown (spike S2), so the connector
// mimics the admin shutdown: chat announcements on a countdown, then the engine's own graceful
// exit (UGameEngine::HandleExitCommand -> FUnixPlatformMisc::RequestExit(0)), reached through the
// reflected KismetSystemLibrary.ExecuteConsoleCommand("exit"). No signature pin is needed.
#pragma once

#include <string>
#include <vector>

namespace conan {

// Seconds before the exit at which a warning is sent, for a countdown of `total` seconds:
// always `total` itself, then the usual marks below it (5 min, 4, 3, 2, 1 min, 30 s, 10 s,
// 5..1 s). Descending, no duplicates. Empty for total <= 0.
std::vector<int> CountdownMarks(int total);
// "The server shuts down in 2 minutes." / "... in 30 seconds." / "... in 1 second."
std::string CountdownText(int secondsLeft);

}  // namespace conan
