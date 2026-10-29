# Zomboid command probe (#201 research)

Research scaffolding for [docs/research/native-commands/zomboid.md](../../../../docs/research/native-commands/zomboid.md).
It is not the Takaro command protocol and nothing here is shipped behaviour.

- `mod/agent/.../research/CommandProbe.java`: catalog snapshot, name rules, argument
  parsing, private outcomes and receipts. No game classes; unit tested in `CommandProbeTest`.
- `mod/agent/.../research/CommandProbeRuntime.java`: the game side. It peeks the command string
  in `GameServer.receiveReceiveCommand`, polls the stub catalog and answers with
  `ChatServer.sendMessageToServerChat(UdpConnection, String)` on the main thread.
- `mod/agent/.../hooks/ReceiveCommandAdvice.java`: the hook. `HookInstaller` binds it only when
  `TAKARO_COMMAND_PROBE_URL` is set; without it the agent's transformation of `GameServer` is
  unchanged and no probe thread starts.
- `stub.py`: the stand-in backend (`GET /catalog`, `POST /invoke`, `POST /control` with modes
  `ok`, `deny`, `slow`, `drop`, `offline`).

Run it on a disposable dev server only:

```bash
python3 games/zomboid/research/command-probe/stub.py --bind <docker bridge ip> --port 8790 --receipts stub-receipts.jsonl
# start the server with TAKARO_COMMAND_PROBE_URL=http://<docker bridge ip>:8790 in its environment
curl -s -X POST http://<docker bridge ip>:8790/control -d '{"mode":"deny"}'
```

The agent writes `command-probe-receipts.jsonl` next to `TakaroConfig.txt`. Receipts carry the
caller's SteamID; keep them out of tracked files.
