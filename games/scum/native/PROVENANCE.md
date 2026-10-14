# Provenance of third-party and ported code in TakaroSCUM

| Component | Source | Version / commit | Licence | Local changes |
| --- | --- | --- | --- | --- |
| `native/src/transport*.{h,cpp}` | gettakaro/connectors `games/enshrouded/mod/src/native/` (our own code) | origin/main 2f55ab11 (2026-10-13) | MIT | log function, JSON helpers (`jsonmini.h`), user agent `Takaro-SCUM/<version>` |
| `native/src/ue4ss_abi.hpp` | hand-written to match RE-UE4SS `UE4SS/include/Mod/CppUserModBase.hpp` and `deps/first/LuaMadeSimple/include/LuaMadeSimple/LuaMadeSimple.hpp` | 91d90727c24ad513838f4e96975d9a51e0126293 | MIT | declarations only; no UE4SS code copied, no Re-UE4SS/UEPseudo |
| `native/ue4ss/UE4SS.def` | export table of `UE4SS.dll` from release `experimental-latest` asset `UE4SS_v3.0.1-1171-g91d90727.zip` | v3.0.1-1171-g91d90727 | MIT | subset of 17 exports used by main.dll |
| `mod/TakaroSCUM/Scripts/takaro/json.lua` | rxi/json.lua | dbf4b2dd2eb7c23be2773c89eb059dadd6436f94 (sha256 of the original file 0eaccda57fabc0330736de25f45cf589821a42b5e0fe02e4e3125f7dc0bf2b7e) | MIT | `json.null`, `json.object()`, sorted object keys |

The import check `native/check-imports.py <main.dll> <UE4SS.dll>` must pass against the pinned UE4SS.dll before a
release: every UE4SS symbol main.dll imports must be exported by it.

Release builds (`scripts/build-release.sh`) compile main.dll with clang-cl/lld-link 19 against the MSVC CRT 14.44.17.14
and Windows SDK 10.0.26100 (fetched by xwin 0.10.0, tree digest pinned in the catalog target) and bundle the UE4SS zip
above unmodified except `UE4SS-settings.ini` (hooks, `native/ue4ss/UE4SS-settings.required.txt`) and `Mods/mods.txt`.
