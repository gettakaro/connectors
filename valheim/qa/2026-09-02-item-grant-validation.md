# Valheim item-grant live validation — 2026-09-02

## Scope

Live verification of the protocol-2 `item-grant` inventory delivery introduced by
`feat(valheim)!: deliver giveItem into the player's inventory`. Before this run the
feature had unit tests and a real-assembly build, but had never executed in a running
game: the previous handoff recorded it explicitly as "built, NOT live-tested".

This run answers one question — does a Takaro `giveItem` land in the player's actual
Valheim inventory, and does the accounting reconcile when it cannot? — plus the mode
and version behaviour the breaking protocol bump makes operators depend on.

The governing rule of `docs/connector-testing/CHECKLIST.md` applies: a connector is
accepted on what it does *through Takaro* and in the *game client*, not on what its own
code or logs claim. Every verdict below cites a Takaro-visible result, an in-game
screenshot, or an exact log line. `SKIP` means not verified and is never reported as a
pass.

Screenshots referenced as `scratchpad/shots/<name>.png` live in the session scratch
directory and are **not committed**; they are cited here for traceability only, and every
verdict is additionally supported by log text or a Takaro-read value that stands on its
own.

## Candidate identity

- Branch: `fix/valheim-live-validation`, feature commit `89a64d6`
- Built and deployed version: **`2.0.0-dev.28a4566`** (server plugin and companion, built
  together by `./scripts/build-release.sh 2.0.0-dev.28a4566 dist`)
- Protocol: `{ "minimum": 2, "current": 2, "maximum": 2 }` on both halves
- Deploy timestamp (server restart): **2026-09-02T13:47:52Z**
- Deployed server DLL SHA-256:
  - `TakaroValheim.dll` `a3313372a52a3d663b98beff79b1ee97d1a8903587b5dfaf1ff02fa73c5718b8`
  - `Takaro.Valheim.Core.dll` `7a2eba78c1d760b29cc81f376d5f56f7d7af2649e11b6b2ca7cce6c88708cd90`
  - `Takaro.Valheim.Companion.Protocol.dll` `6d57169e1e65de6e359b78ed622bec5c27d8370ba54ef2f927ea1e51512d59ae`
- Deployed companion DLL SHA-256:
  - `Takaro.Valheim.Companion.dll` `9255af5534c7266f7858917f177ab058993afba013f6bbfdd78e0ea7994b612c`
  - `Takaro.Valheim.Companion.Protocol.dll` `6d57169e1e65de6e359b78ed622bec5c27d8370ba54ef2f927ea1e51512d59ae`

Role separation was verified from the archives themselves: the companion ZIP contains no
`TakaroValheim.dll` and no `Takaro.Valheim.Core.dll`, and the plugin ZIP contains no
`Takaro.Valheim.Companion.dll`.

The previous `2.0.1` (protocol 1) deploy was backed up whole before this one, to
`/home/hendrik/valheim-takaro-plugin-backups/20260902T134732Z`. That backup doubles as
the v1 companion fixture used by GRANT-9 below.

Note on version reporting: BepInEx logs the **assembly** version, so the server line reads
`Loading [Takaro Valheim 2.0.0]` with no prerelease suffix. `manifest.json` and the
companion's own startup line carry the full `2.0.0-dev.28a4566` and are the source of
truth.

## Environment

- Takaro game server: `82f53af5-bb18-4c15-98bf-6fb956d433e1` ("Takaro Dev Valheim"),
  domain `shiny-bats-bake`
- Player: `Hehe` / `Steam_76561198000735875`
- Server config: `companionMode = required` for GRANT-1..8 and GRANT-9; temporarily
  `optional` for GRANT-8
- Dedicated server log: `/home/hendrik/valheim-dedicated-server/valheim-server.out`
- Graphical client log:
  `~/.local/share/Steam/steamapps/common/Valheim/BepInEx/LogOutput.log`
- `BepInEx/cache/chainloader_typeloader.dat` was deleted on **both** roles before every
  restart in this run

Protocol-2 negotiation was confirmed on both sides before any grant was issued:

```text
client  Takaro Valheim Companion 2.0.0-dev.28a4566 started with protocol 2.
client  Takaro Valheim Companion negotiated protocol 2 with the connected server.
server  Takaro Valheim companion hello sent to peer -728583769.
server  Takaro Valheim server message routed to 1 peer(s); skipped 0 peer(s) without
        compatible companion chat.
```

Baseline before the first grant: `{"online":true,"currency":201,"inventory":["RawMeat x1
q1","LeatherScraps x1 q1"]}` — no Wood — with
`gameserverTestReachabilityForId` → `{"connectable": true}` and server pid `613087`.

## Verdicts

### Inventory delivery

| ID | Item | Verdict | Evidence |
| --- | --- | --- | --- |
| ACT-11 | `giveItem` lands in the player's inventory in game | PASS | `give Wood 5` → server `routed giveItem to the companion for Hehe (Steam_76561198000735875): item=Wood, amount=5, quality=1.`; client `applied an item grant: 5x item_wood to inventory, 0 dropped.`; in-game chat `Takaro: Received 5x item_wood.`; Takaro read back `Wood x5 q1`. No `dropped … for Hehe` world-drop line anywhere in the server log. `scratchpad/shots/c1-inv-open.png` shows Wood 5/50 in the bag, and the Stone Axe recipe listing Wood 5 in white (satisfied) independently confirms it |
| GRANT-1 | Grant merges into an existing stack | PASS | `give Wood 40` then `give Wood 5`: Wood progressed `5 → 45 → 50` as a single Takaro entry; `scratchpad/shots/c2-inv-open2.png` shows exactly ONE Wood stack reading `50/50` and no second Wood slot, with the other 29 slots empty |
| DATA-1 | No double counting | PASS | Every grant moved the Takaro total by exactly the amount that the client reported as delivered, never by the requested amount when they differed. The overflow case is the sharp one: `give Wood 20` against a full bag reported `5x to inventory, 15 dropped` and Takaro's Wood total moved `1495 → 1500`, exactly +5 — not +20, and not +25 |
| GRANT-2 | Unknown item code is rejected before any grant | PASS | `give NotAnItem 1` → HTTP 400 `item_not_found: Valheim item 'NotAnItem' was not found.`; server logged `response frame written: action=giveItem, success=False.` with **no** `routed giveItem to the companion` line; the client log had no new lines at all; inventory unchanged |
| GRANT-3 | Overflow splits — what fits goes in, only the shortfall drops | PASS | Bag filled to 32/32 (29 stacks of Wood 50/50, one at 45/50, plus RawMeat and LeatherScraps — `scratchpad/shots/c4-inv-full.png`), then `give Wood 20` → client `applied an item grant: 5x item_wood to inventory, 15 dropped.`; chat `Takaro: Received 5x item_wood; inventory full, 15 dropped at your feet.`; the 45 stack topped up to 50/50, slot count stayed 32, weight moved 2992 → 3002 (+10 = 5 wood × 2.0), and the dropped logs are visible on the ground in `scratchpad/shots/c4-ground4.png` |
| GRANT-4 | Full bag drops everything with a player-visible notice | PASS | `give Wood 1` against 32/32 → client `applied an item grant: 0x item_wood to inventory, 1 dropped.`; chat `Takaro: Your inventory was full - 1x item_wood dropped at your feet.`; Takaro Wood total stayed 1500 across four polls over 24 s. Still the **companion** route — the server logged `routed giveItem to the companion`, not `dropped 1x Wood for Hehe` |
| GRANT-5 | No double notice from the old server-side drop path | PASS | For every companion-route grant in this run, no `Takaro: Dropped …` HUD text appeared alongside the companion notice — confirmed visually in each screenshot and by `grep -i drop` over the **entire** dedicated-server log for the phase, which returns zero matches. The server-side world drop never ran while a v2 companion was present |

### Concurrency

| ID | Item | Verdict | Evidence |
| --- | --- | --- | --- |
| GRANT-6 | Fire-and-forget grant does not deadlock the main thread | PASS | 30 concurrent `giveItem` calls completed in **1686 ms** (14:02:28 → 14:02:30 UTC), all 30 returning `{"meta":{},"data":{}}` with zero errors. Server logged 30 `routed giveItem to the companion` and 30 `response frame written: action=giveItem` with `success=False` count 0. Client logged 30 grants whose delivered + dropped sums to exactly 30. A 90-second poller running `gameserverGetPlayers` + `gameserverTestReachabilityForId` every second scored **30/30 cycles OK, 0 timeouts, no gap**, with the samples bracketing the burst showing no stall (`14:02:28.399 getPlayers=OK 644ms reach=OK 1072ms` → `14:02:31.130 getPlayers=OK 619ms reach=OK 493ms`). Server pid `613087` unchanged before and after |

This is the regression guard for the load-bearing constraint in the feature commit: the
server must never await the companion's reply, because `Drain()` blocks the Unity main
thread. Thirty simultaneous grants under continuous Takaro polling is the shape that
would expose a reintroduced round trip, and nothing stalled.

### Shop path

| ID | Item | Verdict | Evidence |
| --- | --- | --- | --- |
| ECON-7 | Shop claim delivers in game (full-bag route) | PASS | `@shop 1 1 buy` typed in-game against listing `bd384176-6f3d-40a1-939f-facdd9cd30ff` ("Acceptance Wood Bundle", 100 coins, 5× Wood). Takaro recorded a new order `8712a425-1b6d-46fc-9362-e73f51b7deaa` with status `COMPLETED`; currency went **201 → 101**, exactly −100. The server routed the delivery through the companion: `routed giveItem to the companion for Hehe (Steam_76561198000735875): item=Wood, amount=5, quality=1.` The bag was full, so the client reported `0x item_wood to inventory, 5 dropped.` and the player saw the chain in chat (`scratchpad/shots/c7-result.png`) |
| GRANT-7 | Shop claim lands **in the bag** (non-full inventory) | PASS | Closed after the operator freed two slots by hand (30/32 used, `shots/shop3-chat.png`). `@shop 1 1 buy` typed in-game at 14:35:43Z → server `request received: action=giveItem` then `routed giveItem to the companion for Hehe (Steam_76561198000735875): item=Wood, amount=5, quality=1.`; client `applied an item grant: 5x item_wood to inventory, 0 dropped.`; no `dropped … for Hehe` line; order `6363de0f-0bf5-425b-b293-8070cc2cb4c7` `COMPLETED`; currency 101 → 1; Takaro inventory 30 → 31 stacks, Wood 1400 → 1405 (+5 exactly) |

The full shop chain, as the player saw it:

```text
Hehe:   @shop 1 1 buy
Takaro: You have purchased Acceptance Wood Bundle for 100 Takaro coins.
Takaro: Your inventory was full - 5x item_wood dropped at your feet.
Takaro: You have received items from a shop order.
Takaro: 5x item_wood
```

### Modes and version compatibility

| ID | Item | Verdict | Evidence |
| --- | --- | --- | --- |
| GRANT-8 | No companion under `optional` → unchanged server-side world drop | PASS | With `companionMode = optional` and the companion folder moved aside (vanilla client), `give Wood 5` logged `Takaro Valheim dropped 5x Wood for Hehe (Steam_76561198000735875) at x=81.72208, y=35.97805, z=-0.9973294.` and **no** `routed giveItem to the companion` line. The client Takaro log had zero new lines. No enforcement line existed in the server log after 40 s (`grep -a -i "required companion enforcement\|kicked RPC"` → no matches), the player stayed connected and listed by `gameserverGetPlayers`, and `scratchpad/shots/c5-worlddrop.png` shows the wood log on the ground at the character's feet |
| GRANT-9 | v1 companion vs v2 server under `required` — server survives, client is told | PASS (outcome) | The `2.0.1` / protocol-1 companion fixture was installed against the protocol-2 server. Enforcement fired ~30 s after spawn: `required companion enforcement scheduled for peer -642826249: reason=MissingCompanion, expected=2, actual=missing.` followed by `sent the built-in kicked RPC to peer -642826249 after the companion explanation grace period.` The client returned to the main menu with Valheim's own modal reading "You have been kicked from the server." (`scratchpad/shots/c6-kicked.png`). Server pid `653081` was **identical** before and after, `gameserverGetPlayers` answered (`[]`) and `gameserverTestReachabilityForId` still returned `{"connectable": true}`. The diagnostic string is weaker than documented — see the findings section |

Restoring the v2 companion under `required` returned the session to a clean state:
`negotiated protocol 2 with the connected server`, no enforcement scheduled for the new
peer, and `gameserverGetPlayers` listing `Hehe` again.

## Findings recorded rather than fixed

### 1. An incompatible companion is reported as *missing*, not as a version mismatch

The feature commit's body claims that keeping `MinimumVersion` at 1 would be worse because
a v1 companion "would fail to parse a v2 hello, answer nothing, and be kicked under
required mode with no diagnostic at all. **Refusing to negotiate produces an actionable
version-mismatch message instead.**"

That last claim did not hold. Raising the minimum does not produce a version-mismatch
message for a protocol-1 companion, because the rejection happens before any version can
be exchanged. The v1 companion loaded and started normally —

```text
client  Loading [Takaro Valheim Companion 2.0.1]
client  Takaro Valheim Companion initialized for protocol 1.
client  Takaro Valheim Companion 2.0.1 started with protocol 1.
client  Takaro Valheim Companion RPC registered.
```

— and then never answered the server's hello, because a protocol-1 companion cannot parse
a protocol-2 hello carrying the unknown `ItemGrant` capability bit. From the server's side
that peer is indistinguishable from a peer with no companion at all, so the enforcement
line reads:

```text
required companion enforcement scheduled for peer -642826249:
  reason=MissingCompanion, expected=2, actual=missing.
```

Not `actual=1`. The mismatch is detected by **timeout**, not by a negotiated version-range
rejection. On the client side the situation is worse: grepping the whole client log for
`error|warn|exception|fail|unsupported|protocol` returns only the companion's own two
"protocol 1" startup lines. It logs no incompatibility, no negotiation failure, nothing.

So in practice: an operator debugging an out-of-date companion sees a *missing-companion*
enforcement and must know to distrust it, and the player sees the generic "You have been
kicked from the server." dialog with no cause. The outcome is still correct — an
incompatible client is kicked and the dedicated server survives — but the diagnostics
promised by the upgrade rationale are not there. A version-range report can only work for
a companion new enough to parse the server hello; from protocol 2 onward that is possible,
and for protocol 1 it never will be. `COMPANION.md` has been qualified accordingly.

### 2. Chat notices use the Takaro item code, not the Valheim display name

Every player-visible notice names the item as `item_wood`:

```text
Takaro: Received 5x item_wood.
Takaro: Received 5x item_wood; inventory full, 15 dropped at your feet.
Takaro: Your inventory was full - 1x item_wood dropped at your feet.
```

This is the Takaro item-name convention rather than Valheim's display name `Wood`, and it
matches the display-name derivation in `Core/Inventory.cs` and `ValheimServerAdapter.cs`,
as well as the shop's own `5x item_wood` confirmation line. It is therefore *consistent*
rather than a one-off bug, and a player reading their chat sees the same token the shop
showed them. Recorded as cosmetic and deliberately not changed in this run: renaming it
touches the shared naming convention, not the grant path.

### 3. Takaro inventory snapshots lag the grant by 8–20 seconds

Grants were visible in the game client and the client log immediately, but Takaro's
inventory read lagged: ~8 s for the first grant to appear, and ~20 s for the `45 → 50`
merge. Anything verifying `giveItem` through `getPlayerInventory` must poll rather than
read once, or it will record a false negative. This is Takaro-side snapshot cadence, not a
connector delay.

### 4. The delivery outcome is not visible through Takaro

Every successful `giveItem` returned the empty success payload:

```json
{ "meta": {}, "data": {} }
```

Takaro does not surface a `delivery` field, so the caller cannot tell from the API whether
the items went into the bag or onto the ground. The split is only observable in the server
log (`routed giveItem to the companion` versus `dropped Nx … at x=…`) and in the client
log (`Nx to inventory, M dropped`). Anyone building on this — a shop refund policy, for
instance — should know the API response carries no delivery information.

### 5. The in-bag shop case needed a human hand on the inventory grid

`GRANT-7` was initially a SKIP because the automated session could not bring the test bag
below full; it was closed later the same day once the operator dropped two stacks by
hand. Freeing a slot requires physically manipulating the inventory grid, and Valheim's
UI cursor cannot be driven by synthetic pointer input:

- `xdotool mousemove` to a computed slot position, with and without a preceding
  `windowactivate --sync`, moved the X pointer into the Valheim window
  (`getmouselocation` confirmed `x:477 y:371 … window:148897800`) but produced **no** slot
  highlight and no tooltip.
- Ctrl+click (the in-game `Drop  L-Ctrl + Mouse-1` binding), plain click-pick, and
  mousedown/mouseup at the slot all left the bag at 32 items.
- `mousemove_relative` in small steps *did* move Valheim's own crosshair once, proving
  Valheim tracks a private cursor from relative deltas that is decoupled from the X pointer
  position — but further relative bursts moved it only partway and then it stopped
  responding, and one large single burst did not move it at all.

This is the same class of problem that previously forced `key.sh`/`chat.sh` to drop
`--window`. `/die` was excluded by instruction, since it drops the whole inventory. With no
steerable cursor there is no drag, ctrl-drop, or click-pick that can reach a slot, so the
run stopped rather than reporting an inferred pass.

### Automation notes worth keeping

These cost real time this session and are not obvious:

- **`xdotool --window` is ignored by Valheim.** Targeted key events do not reach it; only
  focused, window-less sends work — hence `key.sh`/`chat.sh` without `--window`.
- **`Tab` opens the inventory but does not close it.** The on-screen hint is
  `Close Menu  Escape`. Worse, `Escape` closes the inventory *and* opens the game menu, so
  a second `Escape` is needed to get back to the world.
- **The first key after window activation is consumed.** When the Valheim window is not
  already focused, the activation swallows the first synthetic key, which makes an
  open/close pair look like it did nothing.
- **The chat overlay auto-fades within a few seconds**, so a chat screenshot must be taken
  immediately after the action that produced the message, not after checking logs.
- **Pointer warps do not move the in-game UI cursor** (finding 5 above).

## Result

The protocol-2 `item-grant` inventory delivery is **live-proven** on the deployed
`2.0.0-dev.28a4566` artifact. A Takaro `giveItem` now lands in the player's real Valheim
inventory: it merges into an existing stack rather than opening a new slot, it splits
correctly against a full bag by inserting what fits and dropping only the shortfall, and
in every case the amount Takaro records is exactly the amount that actually arrived — no
double counting, and no units invented or lost. The old server-side world drop never ran
while a v2 companion was present, and no duplicate drop notice was ever shown.

The fire-and-forget design holds under load: thirty concurrent grants completed in 1686 ms
while a one-second Takaro poller ran uninterrupted at 30/30 cycles, with the dedicated
server's pid unchanged. That is the regression guard for the main-thread constraint the
feature depends on, and it passed.

The compatibility story is correct in outcome and weaker in diagnostics than documented.
A vanilla client under `optional` still gets the unchanged world drop and is left alone; a
protocol-1 companion under `required` is kicked with Valheim's own dialog while the
dedicated server survives untouched. But that kick is logged as
`reason=MissingCompanion, expected=2, actual=missing` — an out-of-date companion is
indistinguishable from an absent one — and the client logs nothing at all. The upgrade
rationale's promise of an actionable version-mismatch message does not apply to a
protocol-1 companion.

The last gap was closed by hand: with two slots freed on the inventory grid, an in-game
`@shop 1 1 buy` produced order `6363de0f-…` `COMPLETED`, the server routed the delivery to
the companion, the client inserted `5x item_wood to inventory, 0 dropped`, and Takaro's
inventory read moved 1400 → 1405 Wood (`GRANT-7`). Every row in this ledger is now
observed, none inferred.
