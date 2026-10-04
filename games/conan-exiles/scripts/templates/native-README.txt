Takaro Conan Exiles Connector @VERSION@ (@PLATFORM@)

The native connector: @BINARY@ runs inside the Conan Exiles dedicated server and connects it
to Takaro by itself. No sidecar, no RCON, no mod; players install nothing.

Built for Conan Exiles Dedicated Server build @REVISION@ (see takaro-target.json). On any
other build it stays connected but refuses every action and says why in
ConanSandbox/Saved/Logs/TakaroConanNative.log.

Install, upgrade, moving over from the old bridge and rollback: INSTALL.md.
Configuration: takaro.json.example (copy it to ConanSandbox/Saved/Config/Takaro/takaro.json).
What is linked in, and the licenses: THIRD-PARTY.md and licenses/.
Checksums of every file in this folder: SHA256SUMS.

Never share takaro.json: it holds your Takaro registration token.
