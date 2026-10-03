// The native connector's coverage registry: one record per Takaro action and event, with the
// semantics of the bridge's registry (bridge/src/takaro/coverage.ts). capabilities.json next to
// this file is the published copy; tests/drift_test.py asserts the two are equal.
//
// `status` is evidence (live-supported only with live MCP plus client evidence). `implementation`
// drives runtime behaviour: "native" actions run, "pending" ones answer a structured error that
// names the action, so Takaro always gets an answer and never a silent success.
#pragma once

#include <string>

namespace conan {

struct Coverage {
    const char* name;
    const char* status;          // live-supported | schema-fallback | unsupported | pending
    const char* implementation;  // native | pending
    const char* shape;           // responseShape / payloadShape
    const char* verification;
    const char* reason;
};

const Coverage* ActionCoverage(const std::string& action);  // nullptr: not a Takaro action
const Coverage* EventCoverage(const std::string& type);
std::string RegistryJson();  // same shape as capabilities.json minus the comment and legend

}  // namespace conan
