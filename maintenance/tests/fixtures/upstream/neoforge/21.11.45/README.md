# NeoForge 21.11.45 stand-ins

`neoforge-21.11.45-installer.jar` and `neoforge-21.11.45-universal.jar` are ~400-byte zips,
not the real 4 MB / 3.8 MB jars. The tests re-pin a catalog copy to these files' own sha256
values and serve them, plus their `.sha256` sidecars, from an in-process HTTP server, so the
sidecar-first acquisition path runs without shipping upstream bytes.
