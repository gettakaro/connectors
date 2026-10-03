#!/usr/bin/env python3
"""Lists the stage 2 candidate classes, UFunctions and properties found in a probe reflection
dump, as Markdown. Usage: candidates.py <reflection.json[.gz]> > candidates.md"""
import gzip
import json
import re
import sys

# (section, kind, target). kind: fn = exact "Owner.Function"; fnre = regex on function names
# (optional owner regex after '@'); cls = class/struct with all its functions; prop = regex on
# property names (optional owner regex after '@'); enum = enum name.
CANDIDATES = [
    ("Console and cheats", "fn", "PlayerController.ConsoleCommand"),
    ("Console and cheats", "fn", "PlayerController.ServerExec"),
    ("Console and cheats", "fn", "PlayerController.ServerExecRPC"),
    ("Console and cheats", "fn", "KismetSystemLibrary.ExecuteConsoleCommand"),
    ("Console and cheats", "fn", "PlayerController.EnableCheats"),
    ("Console and cheats", "fn", "ConanPlayerController.CheatSpawnItem"),
    ("Console and cheats", "fn", "ConanCheatManager.ServerExec"),
    ("Console and cheats", "fn", "ConanCheatManager.SpawnItem"),
    ("Console and cheats", "fn", "ConanCheatManager.TeleportPlayer"),
    ("Console and cheats", "fn", "ConanCheatManager.TeleportPlayerExact"),
    ("Console and cheats", "fn", "GameItemSpawner.SpawnItem"),
    ("Console and cheats", "fnre", "Admin|Cheat@^(Conan|PlayerController|BasePlayerChar|Admin)"),
    ("Console and cheats", "prop", "CheatManager|CheatClass|Admin@^(PlayerController|ConanPlayerController|BasePlayerChar_C|ConanCharacter)$"),
    ("Console and cheats", "cls", "ConanCheatManager"),
    ("Console and cheats", "cls", "BP_ConanCheatManager_C"),
    ("Moderation", "fnre", "Kick"),
    ("Moderation", "fnre", "Ban(Player|Info)|Unban|Blacklist|Whitelist"),
    ("Moderation", "cls", "ServerBlacklist"),
    ("Moderation", "cls", "BlacklistedUser"),
    ("Moderation", "cls", "ConanGameSession"),
    ("Moderation", "cls", "GameSession"),
    ("Moderation", "enum", "EBanType"),
    ("Moderation", "enum", "EBanInfoReasons"),
    ("Shutdown", "fnre", "Shutdown|Quit|ExitGame|RequestExit|ServerRestart|Countdown|Maintenance"),
    ("Inventory and catalogue", "cls", "ItemInventory"),
    ("Inventory and catalogue", "cls", "ItemTemplateTableRow"),
    ("Inventory and catalogue", "cls", "ItemTableRow"),
    ("Inventory and catalogue", "fn", "KismetTextLibrary.Conv_TextToString"),
    ("Inventory and catalogue", "fn", "DataTableFunctionLibrary.GetDataTableRowNames"),
    ("Inventory and catalogue", "fnre", "GetLocalizedName|GetItemName|GetDisplayName|GetItemTemplate"),
    ("Inventory and catalogue", "prop", "Inventory@^(ConanCharacter|BasePlayerChar_C|ConanPlayerController)$"),
    ("Location and teleport", "fn", "Actor.K2_GetActorLocation"),
    ("Location and teleport", "fn", "Actor.K2_SetActorLocation"),
    ("Location and teleport", "fn", "Actor.K2_TeleportTo"),
    ("Location and teleport", "fnre", "Teleport@^(Conan|PlayerController|Actor|BasePlayerChar)"),
    ("Events", "fn", "GameModeBase.K2_PostLogin"),
    ("Events", "fn", "GameModeBase.K2_OnLogout"),
    ("Events", "fn", "BaseGameMode_C.K2_PostLogin"),
    ("Events", "fn", "BaseGameMode_C.K2_OnLogout"),
    ("Events", "fnre", "PlayerLogout|PostLogin|OnLogin|OnLogout"),
    ("Events", "fnre", "OnNPCKilled|WasKilled|OnKilled|KilledBy|OnDeath|NewOnDeath|HandleKilledByPlayer|KillCharacter$"),
    ("Events", "fnre", "SendChatMessage|ReceiveChatMessage"),
    ("Ping and IP", "prop", "Ping|NetworkAddress|IPAddr|RemoteAddr|UserIDFromURLOptions|MasterAccountId|UniqueID$@^(PlayerState|ConanPlayerState|PlayerController|ConanPlayerController|NetConnection|IpConnection)$"),
    ("Ping and IP", "fnre", "Ping|NetworkAddress|GetIP@^(PlayerState|ConanPlayerState|PlayerController|ConanPlayerController|KismetSystemLibrary)$"),
]


def load(path):
    opener = gzip.open if path.endswith(".gz") else open
    with opener(path, "rt", encoding="utf-8") as f:
        return json.load(f)


def params(f):
    out = []
    for p in f["params"]:
        t = p["type"]
        extra = p.get("struct") or p.get("class") or p.get("enum") or (p.get("inner") or {}).get("type") or ""
        out.append(f"{p['name']}: {t}{'<' + extra + '>' if extra else ''} @{p['offset']}"
                   + (f" [{p['param']}]" if p.get("param") else ""))
    return "; ".join(out) or "none"


def fn_row(owner, f):
    native = "yes" if "Native" in f["flagNames"] else "no (Blueprint)"
    return (f"| `{owner}.{f['name']}` | {native} | {f['flagNames']} | {f['parmsSize']} | {params(f)} |")


def main():
    d = load(sys.argv[1])
    structs = d["structs"]
    by_name = {}
    for s in structs:
        by_name.setdefault(s["name"], s)
    enums = {e["name"]: e for e in d["enums"]}
    meta = d["meta"]
    print(f"Dump: build {meta['serverBuild']}, build-id {meta['buildId']}, {meta['generatedAt']}; "
          f"{meta['structs']} structs, {meta['functions']} UFunctions, {meta['enums']} enums, "
          f"{meta['objectSlots']} object slots.\n")
    section = None
    fn_header = "| Function | Native | Flags | ParmsSize | Params (name: type @offset) |\n|---|---|---|---|---|"
    for sec, kind, target in CANDIDATES:
        if sec != section:
            section = sec
            print(f"\n## {sec}\n")
        if kind == "fn":
            owner, name = target.split(".", 1)
            s = by_name.get(owner)
            f = next((x for x in (s or {}).get("functions", []) if x["name"] == name), None)
            if f:
                print(f"**{target}**: present as a reflected UFunction.\n\n{fn_header}\n{fn_row(owner, f)}\n")
            else:
                why = "class not found" if not s else "class found, no UFunction with this name"
                print(f"**{target}**: NOT reflected ({why}).\n")
        elif kind == "fnre":
            pat, _, own = target.partition("@")
            rows = [fn_row(s["name"], f) for s in structs for f in s["functions"]
                    if re.search(pat, f["name"], re.I) and (not own or re.search(own, s["name"]))
                    and not f["name"].startswith("ExecuteUbergraph")]
            print(f"**Functions matching `{pat}`{' on `' + own + '`' if own else ''}**: {len(rows)}"
                  + (" (first 40)" if len(rows) > 40 else "") + "\n")
            if rows:
                print(fn_header + "\n" + "\n".join(rows[:40]) + "\n")
        elif kind == "cls":
            s = by_name.get(target)
            if not s:
                print(f"**{target}**: NOT found.\n")
                continue
            print(f"**{target}** ({s['kind']}, `{s['path']}`): super chain {' > '.join(s['superChain']) or '-'}; "
                  f"size {s['size']}; live instances {s.get('instances', 0)}; "
                  f"{len(s['properties'])} properties, {len(s['functions'])} functions.\n")
            if s["properties"]:
                print("| Property | Type | Offset | Size |\n|---|---|---|---|")
                for p in s["properties"][:60]:
                    extra = p.get("struct") or p.get("class") or p.get("enum") or (p.get("inner") or {}).get("type") or ""
                    print(f"| {p['name']} | {p['type']}{'<' + extra + '>' if extra else ''} | {p['offset']} | {p['size']} |")
                print()
            if s["functions"]:
                print(fn_header)
                for f in s["functions"]:
                    print(fn_row(target, f))
                print()
        elif kind == "prop":
            pat, _, own = target.partition("@")
            rows = []
            for s in structs:
                if own and not re.search(own, s["name"]):
                    continue
                for p in s["properties"]:
                    if re.search(pat, p["name"], re.I):
                        extra = p.get("struct") or p.get("class") or p.get("enum") or ""
                        rows.append(f"| `{s['name']}.{p['name']}` | {p['type']}{'<' + extra + '>' if extra else ''} | {p['offset']} | {p['size']} |")
            print(f"**Properties matching `{pat}` on `{own or '*'}`**: {len(rows)}\n")
            if rows:
                print("| Property | Type | Offset | Size |\n|---|---|---|---|\n" + "\n".join(rows[:40]) + "\n")
        elif kind == "enum":
            e = enums.get(target)
            if not e:
                print(f"**{target}**: NOT found.\n")
            else:
                vals = ", ".join(f"{n.split('::')[-1]}={v}" for n, v in e["values"])
                print(f"**{target}** (`{e['path']}`): {vals}\n")
    print("\n## Singletons and data tables\n")
    for k, v in d["singletons"].items():
        live = ", ".join(f"`{c}` x{n}" for c, n in v["liveClasses"][:6]) or "none"
        print(f"- **{k}**: live classes {live}; first instance "
              f"{('`' + v['instances'][0]['path'] + '`') if v['instances'] else 'none'}")
    items = [t for t in d["dataTables"] if re.search(r"ItemTable|ItemNameToTemplate", t["path"])]
    print(f"- **Data tables**: {len(d['dataTables'])} live; item tables: "
          + "; ".join(f"`{t['path']}` ({t['rowStruct']})" for t in items))


if __name__ == "__main__":
    main()
