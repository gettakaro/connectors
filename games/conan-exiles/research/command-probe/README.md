# Command research probe (#205)

Research only, not a product feature. Findings: [docs/research/native-commands/conan-exiles.md](../../../../docs/research/native-commands/conan-exiles.md).

The probe is part of the native connector (`native/core/conan/command_probe.{h,cpp}`) and does
nothing unless the server process has `TAKARO_CONAN_COMMAND_PROBE` set to a directory. `stub.py`
stands in for Takaro in that directory:

```bash
python3 stub.py catalog <dir> --revision r1 --prefix .tk   # publish a catalog (re-run to change it)
python3 stub.py run <dir> --admins <steam64>,...          # answer invocations until stopped
```

Start the server with `TAKARO_CONAN_COMMAND_PROBE=<dir>` (Windows: `set` it in the start script).
The native log shows `command-probe: RESEARCH PROBE ENABLED`. Counters are under `commandProbe` in
`events-health.json`. Stop the stub to test the offline path.
