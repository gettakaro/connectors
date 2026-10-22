# Command probe (research for #202)

A research probe, not a feature. It answers whether the Enshrouded server can recognise a Takaro
player command in chat, keep it out of the broadcast, bind it to the sender's SteamID64 and reply
privately. Findings: [`docs/research/native-commands/enshrouded.md`](../../../../docs/research/native-commands/enshrouded.md).

It is never part of a release:

- Only `TAKARO_COMMAND_PROBE=1 ./mod/build.sh` compiles it, into `mod/build-probe/`. A normal build
  does not see these files and produces a byte-identical `dbghelp.dll`.
- The probe build arms only on game build `1273043` (the build whose `addMessage` ABI was
  recovered) with the 1.0 chat and session layout. That needs the `1273043` game data: build on
  top of #582, or add that build's section to `<server>\takaro\enshrouded-gamedata.json`.
- It acts only while `<server>\takaro\command-probe.json` exists. Delete the file to turn it off.

| File | What |
| --- | --- |
| `probe_core.{h,cpp}` | Pure decision logic: catalog parsing and collision preflight, claim, refuse or invoke |
| `probe_glue.inc` | Game side, included by `mod/src/world.cpp` under `#ifdef TAKARO_COMMAND_PROBE`: sender checks, suppression, private reply, catalog and log thread |
| `command-probe.example.json` | A stub catalog |
| `tests/run.sh` | Host test of the decision core (pinned gcc image) |

The probe writes one JSON line per player chat to `<server>\takaro\connector-state\command-probe.jsonl`:
the exact bytes (hex), every sender check, the decision, the stub receipt and the reply route. It
contains SteamIDs; keep it out of public places.

Catalog fields: `revision`, `prefix` (default `@`), `nonce` (the magic trigger, required),
`issuedAtMs` and `maxAgeSeconds` (staleness, both required; 1 s to 7 days), `available` (`false` = Takaro unreachable),
`failDuringSend`, `commands[]` (`name`, `aliases`, `enabled`, `permission`) and
`grants` (`{"<steamId>": ["<permission>"]}`).
