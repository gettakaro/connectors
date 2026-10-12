Takaro Conan Exiles Connector @VERSION@ (@PLATFORM@)

The native connector: @BINARY@ runs inside the Conan Exiles dedicated server and connects it
to Takaro by itself. No sidecar, no RCON, no mod; players install nothing.

Built for Conan Exiles Dedicated Server build @REVISION@ (see takaro-target.json). On any
other build it stays connected but refuses every action and says why in
ConanSandbox/Saved/Logs/TakaroConanNative.log.

Install, upgrade and rollback: INSTALL.md.
Configuration: ConanSandbox/Saved/Config/Takaro/takaro.json, created on the first start. Paste
your registration token into it; saving it while the server runs is enough. An upgrade never
touches it.
What is linked in, and the licenses: THIRD-PARTY.md and licenses/.
Checksums of every file in this folder: SHA256SUMS.

Never share takaro.json: it holds your Takaro registration token.
