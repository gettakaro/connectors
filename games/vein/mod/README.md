# libtakaro-vein

This Linux `LD_PRELOAD` library runs inside `VeinServer-Linux-Test` (Steam app 2131400). It owns the game hooks, Takaro protocol, verified TLS/WebSocket transport, actions, event outbox and persistent state. It requires no sidecar or Node runtime. Players use the vanilla VEIN client. See [the developer guide](../DEVELOPMENT.md), [installation guide](../INSTALL.md) and [optional diagnostic HTTP API](docs/API.md).

Build with `./build.sh --tests` in the pinned Debian Bookworm toolchain. `./build.sh --native --tests` uses an already-provisioned local toolchain. The build embeds pinned PIC/static libwebsockets, OpenSSL, nlohmann/json and PCRE2; [third_party/README.md](third_party/README.md) records versions, hashes and licenses. `dist/libtakaro-vein.so` and its checksum are generated artifacts.

Install the library outside Steam's tree and apply `LD_PRELOAD` to the game binary only. The connector reads `TAKARO_REGISTRATION_TOKEN`, `TAKARO_IDENTITY_TOKEN`, `TAKARO_WS_URL` and persistent state settings in the **game process**. `TAKARO_PLUGIN_TOKEN` is optional and protects loopback diagnostics at port 18890; it is not a bridge secret. If unset, HTTP diagnostics are disabled.

The game thread owns UObject access and never performs socket, JSON, disk or regex work. Background workers exchange owned bounded messages. See [docs/gamethread-policy.md](docs/gamethread-policy.md) for timeout ownership rules. A failed symbol or hook degrades only its capability and is reported in health rather than crashing the server.
