# Reflection probe (dev tool, not shipped)

`libtakaro-conan-probe.so` is an `LD_PRELOAD` library for the Conan Exiles Linux dedicated server, build
25639945 only (GNU build-id `3a05a6ef…`). It is never part of a release. It does two things:

- writes a full UE reflection dump to JSON: every class, script struct and enum, with their properties,
  UFunctions, parameter layouts, live instance counts, CDOs and data tables;
- serves a REPL over HTTP on `127.0.0.1:7979` inside the server's network namespace. The REPL can find
  objects, read properties, call a UFunction on the game thread and trace ProcessEvent by function name.
  Every request needs the `X-Probe-Token` header.

It coexists with the stage 1 library. List the probe **first** in `LD_PRELOAD`: glibc runs preload
constructors last to first, so stage 1 patches ProcessEvent and the probe then chains its detour in front.

## Build

```bash
tools/probe/build.sh   # builds in the pinned Buster image; output is tools/probe/build/libtakaro-conan-probe.so
```

## Use

- `tools/probe/probe.sh METHOD PATH [JSON]` sends a request through `docker exec` into the server container.
  The token is read inside the container from `<Saved>/TakaroProbe/token`, so it never appears on a command line.
- Endpoints: `/health`, `/names`, `/types`, `/functions`, `/class`, `/find`, `/obj`, `/read`,
  `POST /call`, `/trace` (POST, GET and DELETE) and `/dump` (POST and GET).
- `candidates.py <dump.json.gz>` prints the stage 2 candidate table as Markdown.

## Thread rules

- Every read made off the game thread uses `process_vm_readv` on the server's own pid, so a stale pointer
  returns an error instead of crashing the server.
- Only a `/call` job runs on the game thread. Calls are queued and drained from the ProcessEvent detour,
  at most 4 jobs or 500 µs per drain.
- With no trace active, a trace costs one relaxed atomic load per ProcessEvent.
- Matched trace calls are decoded on the calling thread, at the trace's rate limit. File I/O happens on a
  writer thread.
