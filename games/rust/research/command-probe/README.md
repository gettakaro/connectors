# Native command probe (research, issue #197)

Research code only. Nothing here is part of the connector, the release artifact or any
deploy script, and the probe does nothing until `StubUrl` is set in its config. The findings
are in [docs/research/native-commands/rust.md](../../../../docs/research/native-commands/rust.md).

| File | What it is |
| --- | --- |
| `TakaroCommandProbe.cs` | Carbon/Oxide plugin. Polls the stub catalog, registers free direct roots plus the `/takaro <name>` fallback, routes every invocation through one callback that takes the caller from `BasePlayer`, replies to the caller only, never queues. RCON `takaroprobe.status <name> ...` prints who the framework says owns each root. |
| `TakaroCollisionFixture.cs` | A "foreign" plugin that owns `/probedup`, and a sink that logs `OnPlayerChat` and `OnPlayerCommand`. |
| `stub/stub.mjs` | Local stand-in for Takaro: `GET /catalog`, `POST /invoke` (writes `receipts.jsonl`), `POST /control` (modes `ok`, `deny`, `drop`, `slow`; catalog replacement). Not a Takaro protocol. |

## Running it

```bash
# 1. Stub on the host, listening on the Docker network gateway the server container can reach
node games/rust/research/command-probe/stub/stub.mjs <dataDir> <docker-gateway-ip>

# 2. Probe config, then the plugin (Carbon: carbon/configs + carbon/plugins,
#    Oxide: oxide/config + oxide/plugins)
echo '{"StubUrl":"http://<docker-gateway-ip>:28099","PollSeconds":2.0,"InvokeTimeoutSeconds":5.0,"ForceRegisterConflicts":false}' \
  > carbon/configs/TakaroCommandProbe.json
cp TakaroCommandProbe.cs TakaroCollisionFixture.cs carbon/plugins/

# 3. Drive it
curl -XPOST <docker-gateway-ip>:28099/control -d '{"catalog":{"revision":2,"commands":[{"name":"probe"}]}}'
curl -XPOST <docker-gateway-ip>:28099/control -d '{"mode":"slow","delayMs":8000}'
# in game: /probe a b   /takaro probe a b   /probedeny   /probedup   /takaro probedup
```

`ForceRegisterConflicts: true` skips the foreign-owner preflight, to observe what each
framework does with a duplicate name. Remove both plugins and their configs when done.
