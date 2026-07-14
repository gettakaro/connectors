# Valheim server-chat validation — 2026-07-14

## Result

`sendMessage` is live-supported when the connected player runs the owned Takaro
Valheim Companion. The server routes an authenticated `server-chat` envelope only
to a peer that negotiated the supported companion protocol, and the client inserts
the message into Valheim's normal chat history. No HUD overlay fallback is used.

The live run also proved that `opts.senderNameOverride` is carried per request. An
explicit value of `con` reached the client as `con`; the connector's `Takaro`
fallback was observed only on a request that did not contain the override.

## Candidate identity

- Connector source commit: `82546ddd49c6`
- Plugin and companion version: `2.0.1`
- Companion protocol: `1`
- Takaro game-server id: `4dadfdf6-18a3-41f1-ae2c-b94200dea9ab`
- Plugin archive SHA-256:
  `c468c88a3f62cd1d5d863c9e59882e930bca07314b238943edd79fac6bae7a1b`
- Companion archive SHA-256:
  `8939d57b7bb39783c4219eb3fd60c8b72d221ff612d287d60545c77824ca3ec9`
- Deployed server DLL SHA-256:
  `a7770141749c07680e9cc8f2ef1f2f1a0a1bea30525df2b925036d2bcd00e2a7`
- Deployed client DLL SHA-256:
  `7bd3fb79154f73cf699ba1cdf2d14bb67d613a5d2434d70bd29cba09a72efcb6`

The release directories were deployed with checksum-verifying `rsync`. Both server
and client `BepInEx/cache/chainloader_typeloader.dat` files were removed before
startup so deterministic archive timestamps could not preserve stale type-loader
metadata.

## Live proof

1. The dedicated server loaded Takaro Valheim `2.0.1` and registered the companion
   RPC.
2. The client loaded Takaro Valheim Companion `2.0.1` and negotiated protocol `1`.
3. A normal Takaro `sendMessage` request routed to one compatible peer. The client
   logged `rendered a server message from Takaro in chat`; visual testing confirmed
   the message was in chat rather than the previous HUD overlay.
4. A controlled Takaro MCP request sent marker
   `VALHEIM_SENDER_OVERRIDE_PROOF_1784009159` with:

   ```json
   {
     "opts": {
       "senderNameOverride": "con"
     }
   }
   ```

5. Takaro returned a successful empty action response (`meta: {}`, `data: {}`).
6. The server logged `server message routed to 1 peer(s); skipped 0 peer(s) without
   compatible companion chat`.
7. The client logged `rendered a server message from con in chat`.

These signals prove the complete Takaro API -> Generic connector -> dedicated
server plugin -> negotiated client companion -> normal Valheim chat path for the
exact candidate.

## Reproducibility gate

After recording the live proof, the branch passed the full `Takaro.Valheim.sln`
test suite (418 passed, 0 failed) and `scripts/build-release.sh 2.0.1`. The release
package behavior check passed for both process roles, and the rebuilt archives
reproduced the plugin and companion SHA-256 values listed above exactly.

## Boundary discovered during validation

The first ordinary Takaro call omitted `opts.senderNameOverride`, so the connector
correctly displayed its `Takaro` fallback even though Takaro's per-server
`serverChatName` setting was `con`. A direct request with the override proved the
Valheim connector was not dropping or hardcoding the sender.

The missing settings propagation is a Takaro-core Generic-connector boundary issue,
not a Valheim protocol issue. Takaro core PR
[`gettakaro/takaro#2934`](https://github.com/gettakaro/takaro/pull/2934) loads the
configured `serverChatName` and forwards it when the caller does not provide an
explicit override. Explicit overrides continue to win.

## Remaining limitations

- Every receiving player needs the Takaro-owned client companion. A server-only
  implementation cannot add a normal Valheim chat entry on an unmodded client.
- Missing, unnegotiated, or incompatible companions produce the immediate
  `companion_server_chat_unavailable` payload error; the server does not silently
  switch back to an overlay.
- This proof promotes only `sendMessage`. Other capability classifications retain
  their existing evidence boundaries.
