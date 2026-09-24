# Exact-build rejection evidence

Target: `linux-21241282`, exact `ShooterGameServer` SHA-256
`7e7ded49e0c658e74199801d79630bd33da407d3e468e039837bf61a9de7c520`.
The tested staged pre-final library SHA-256 was
`bbbcb42423e774aab377a62df4fd0a352f030749604509d1bbfbf7642a081478`.
These checks used disposable fixtures only; the installed server and live
runtime were untouched. Re-run the maintenance hook on the final built
artifact before claiming final release verification.

The shipped `launch.sh` was copied beside the shipped library into a temporary
directory. A copy of `/bin/true` named `ShooterGameServer` had SHA-256
`b381cc8c55ec524b9fa92897f0882e855357468b4a763519cec47e05994d61ff`.
With `ARK_NATIVE_TOKEN=isolated-fixture`, launching that temporary server root
returned exit 7, empty stdout, and exactly:

```text
Unknown ARK executable; native connector refused
```

The stronger direct-preload test used a copy of `/bin/sleep`, named
`ShooterGameServer` and padded to the expected 86,269,536 bytes. Its SHA-256
was `7ad57c2fdb3478bdf84a68e41fbed11146365b623d3d29515495f9aaf8268e08`.
Inside a disposable `--network none` container, `LD_PRELOAD` of the shipped
library returned safely from its constructor, the fixture slept and exited 0,
and loopback port 18891 was closed:

```text
native_listener=absent
fixture_exit=0
```

The native constructor calls `exact_build()` before `preflight_bindings()`;
the wrong hash therefore prevented the absolute-address prologue reads. The
test demonstrates this for an executable with both the expected basename and
size. The maintenance `negative-wrong-target` hook now repeats the launcher
and direct-preload checks using its pinned runtime image and installed release
artifact, saves each raw log, and passes only when both reject safely.

Distinct wrong-prologue behavior cannot be exercised by mutating this exact
binary: changing a byte also changes its SHA-256 and stops at the earlier hash
guard. Focused binding tests independently reject wrong prologues/targets in
`console_bindings_test.cpp`, `engine_exec_bindings_test.cpp`, and
`death_bindings_test.cpp`. This is a limitation of the negative live fixture,
not a claimed wrong-prologue boot pass.
