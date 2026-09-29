#pragma once

#include <functional>
#include <string>

namespace NativeBridge {
// Direct native Takaro connection is the default. TAKARO_NATIVE_GATE=1 retains
// the isolated experimental gate for transport regression tests only.
// ActionHandler is a test override and runs on the action worker, never the
// socket or game thread. It receives normalized owned JSON arguments.
using ActionHandler = std::function<std::string(const std::string&, const std::string&)>;
void SetActionHandler(ActionHandler handler);
bool Start();
void Stop();
std::string HealthJson();
#ifdef TAKARO_BRIDGE_TEST
void TestPauseCompletions(bool pause);
void TestNoteLocationWindow(const std::string& id);
std::string TestGateAction(const std::string& action, const std::string& args);
#endif
} // namespace NativeBridge
