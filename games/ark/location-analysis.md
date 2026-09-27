# Exact-build loaded spawn-point locations

This is an offline binding candidate for Linux `ShooterGameServer` build
21241282, SHA-256
`7e7ded49e0c658e74199801d79630bd33da407d3e468e039837bf61a9de7c520`.
It has not been tested on the running game or accepted by Takaro.

[Takaro's Generic protocol](https://docs.takaro.io/advanced/generic-connector-protocol/)
defines a `listLocations` WebSocket request with `args: []` and a response
array of `{position,name,code,radius}` or rectangular extents. It is an
actual connector action even though the currently available Takaro MCP
tools expose no direct `gameserverListLocations` endpoint. The inspected
built-in teleport module stores its own locations as variables and does
not dispatch this action. Read-only inspection of the 29 module definitions
available in the controlled Takaro domain found no code reference to
`listLocations` or `ControllerListLocations`; the built-in teleports module
is limited to 7 Days to Die, Rust, and Minecraft. The available MCP
game-server operations include `GetMapInfo` but no list-locations operation;
`GetMapInfo` on the controlled ARK server returned HTTP 400 "gameserver
responded with bad data" and is not evidence of this action. The official
WebSocket contract proves what the sidecar must answer if requested, but a
real module/SDK or backend controller consumer remains to be shown before
claiming a Takaro-mediated `listLocations` acceptance test.

The exact binary registers `UGameplayStatics::GetAllActorsOfClass` with an
ASCII name at `0x455EF44`, registration call at `0x2D70B35`, and exec thunk
at `0x2F260F0`. The thunk reads the three parameters and directly calls
`0x263B340` at `0x2F261CD–0x2F261DA`; its ABI is
`void(UObject* worldContext, UClass* actorClass, TArray<AActor*>* out)`.
The native implementation clears the output count, resolves the world from
the caller's context, checks the class hierarchy, iterates actors, skips
pending-kill objects, and appends matches. The installed APlayerStart class
getter is `0x2E42C80`. Address `0x2E42CA0` lands inside a later call
instruction and must never be used as a function entry. Its reflected
`PlayerStartTag` FName is at `+0x494`
and `SpawnPointRegion` int32 at `+0x49C`, per property registration
`0x2E42CF1–0x2E42DFF`. The `GetActorBounds` ASCII registration
at `0x2D79E60` points to exec thunk `0x2F4BBC0`, which calls native
`0x26E9360` with ABI `void(AActor*, FVector* origin, FVector* boxExtent)`.
The native function calls actor vtable `+0x720` for component bounds and
computes the real center and half-extents from Min/Max at
`0x26E938E–0x26E93CB`. `UObject::GetPathName`
wrapper `0x1293230` gives the loaded actor's object path for a distinct
code; it sets its underlying stop-at-object argument to null itself.

`mod/src/location_bindings.hpp` takes a freshly validated world pointer on
the game thread, enumerates at most 1024 loaded APlayerStart actors, checks
class, object size, path uniqueness, and finite positive native bounds. It
returns their native tag/path, native region index, actual bounds center as
`position`, and actual rectangular sizes (twice each half-extent). This is
the native actor collision footprint, not an invented named spawn-region
center or area. It is a catalog of **loaded spawn actors only**, not
all possible map POIs, spawn areas, or unloaded sublevels. The native
`GetAllActorsOfClass` call itself traverses the world's actor collection in
one game-thread call; the helper's cap bounds output processing, not that
engine enumeration. Integration must validate map population and Takaro's
acceptance of the actual rectangular bounds before exposing `listLocations`; it must
not return an empty success while the map or enumeration is unverified.

Focused synthetic helper verification:

```sh
docker run --rm --network none -v /path/to/connectors:/work:ro gcc:14 bash -lc \
  'g++ -std=c++20 -O2 -Wall -Wextra -Werror -pthread /work/games/ark/mod/tests/location_bindings_test.cpp -o /tmp/location-test && /tmp/location-test'
```

Result: exit 0. This verifies helper bounds, native-string ownership,
duplicate rejection, finite coordinates, and game-thread guard; it is not
a substitute for a live loaded-map snapshot.
