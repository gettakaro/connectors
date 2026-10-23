// Derives a gamedata section (see profile.h) for the running game build from stable anchors: log strings, the ECS job
// table and instruction shapes inside the functions those lead to. Used when takaro/enshrouded-gamedata.json has no
// section for the build, so a game update keeps working without a new DLL.
#pragma once
#include "common.h"

#include <string>

namespace derive {

struct Result {
    bool ok = false;
    std::string error;        // first required value that could not be found
    JsonValue functions;      // name -> {rva, verify}
    JsonValue callsites;      // name -> "0x..."
    JsonValue layout;         // name -> "0x..." / string
    int derivedValues = 0;
};

// Needs scan::Init()/InitAt() first. `templateLayout` (may be null) supplies the constants no anchor can derive;
// derived values always win over it.
Result Derive(const JsonValue* templateLayout);

}  // namespace derive
