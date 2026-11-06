# Provenance of third-party and ported code in TakaroAsa

| Component | Source | Version / commit | Licence | Local changes |
| --- | --- | --- | --- | --- |
| `native/src/transport*.{h,cpp}`, `jsonmini.h` | `games/enshrouded/mod/src/native/` and `games/scum/native/src/` in this repository (our own code) | | MIT | log function, user agent |
| AsaApi headers (build only, never committed) | https://github.com/ArkServerApi/AsaApi | commit `77be34d11feca2790b54fcd5a3d48678588d52bb` | MIT | none |
| `AsaApi.lib` (build only) and the bundled loader, `AsaApi.dll` and its dependencies | https://github.com/ArkServerApi/AsaApi release `2.03`, `AsaApi_2.03.zip` | sha256 in the catalog target | MIT | none; `Lib/` and the Permissions developer folder are left out of the release zip |
| fmt headers (build only, compiled into the DLL) | https://github.com/fmtlib/fmt release `11.1.4` | sha256 in the catalog target | MIT | none |

The headers are newer than the 2.03 binaries: they carry header updates for later game versions that have no AsaApi
release yet. `AsaApi.lib` and `AsaApi.dll` still come from the same release, so what links is what loads.
