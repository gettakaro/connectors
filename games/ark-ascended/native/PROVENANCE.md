# Provenance of third-party and ported code in TakaroAsa

| Component | Source | Version / commit | Licence | Local changes |
| --- | --- | --- | --- | --- |
| `native/src/transport*.{h,cpp}`, `jsonmini.h` | `games/enshrouded/mod/src/native/` and `games/scum/native/src/` in this repository (our own code) | | MIT | log function, user agent |
| AsaApi headers (build only, never committed) | https://github.com/ArkServerApi/AsaApi | the commit pinned in each catalog target | MIT | none |
| `AsaApi.lib` (build only) and the bundled loader, `AsaApi.dll` and its dependencies | https://github.com/ArkServerApi/AsaApi the release pinned in each catalog target | sha256 in the catalog target | MIT | none; `Lib/` and the Permissions developer folder are left out of the release zip |
| fmt headers (build only, compiled into the DLL) | https://github.com/fmtlib/fmt release `11.1.4` | sha256 in the catalog target | MIT | none |

Each catalog target pins its own AsaApi. Game 97 moved to Unreal Engine 5.8 and needs AsaApi 2.11 or later: its target
builds against the commit that release was built from. The older target uses the 2.03 binaries with newer headers, which
carry updates for game versions that had no AsaApi release yet. `AsaApi.lib` and `AsaApi.dll` always come from the same
release, so what links is what loads.