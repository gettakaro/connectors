# Terraria connector: known issues

The details behind the caveats in the [README](README.md)'s "What works, what doesn't" table.

## Players

- **A Terraria server only knows the character name.** Players join by IP and the server gets no
  Steam or other account id, so a player is known to Takaro by the character name. Renaming the
  character makes a new player in Takaro, and the same person is not linked to their profile from
  other games. Use Takaro's `/link` to tie a player to a Takaro user.
- **Names with spaces, dots or other characters** are kept as they are for display; Takaro's
  internal id for them gets a short suffix (for example `terraria:Bob_J__Test-6c2b12d5`).

## Kills

- **The kill weapon is a guess.** Terraria records no damage source when an NPC dies, so the
  weapon is the item the killer held at that moment. Minion, sentry, damage-over-time and
  late-landing projectile kills can name an item that did none of the damage; the weapon is
  `unknown` when nothing usable was held.
- **A kill counts only for a player who damaged the NPC.** NPCs killed by town NPCs, traps or
  lava are not reported. When several players hit the same NPC, the last one to hit it gets the
  kill.

## Items

- **Item names are derived from the game's internal ids**, not from its language file. About 120
  of the 6147 names glue a short word onto the one before (`Bandof Regeneration`,
  `A Horrible Nightfor Alchemy`) and apostrophes are missing (`Aarons Helmet`). Giving and
  looking up still work: the match ignores spaces and punctuation, so `Band of Regeneration` and
  `Aaron's Helmet` find the right item.
- **Giving refuses rather than drops.** An item is only given when it fits in the main
  inventory; coin and ammo slots are not counted. A full inventory refuses the whole delivery,
  so a shop order fails instead of dropping items on the floor.
- **Takaro fills its item list only when it syncs** (when the server is first connected, then
  hourly, or when you trigger the item sync in Takaro). Connect the bridge before you rely on
  item names in the shop.

## Bans

- **Bans hold the character's client id and IP address.** A Takaro ban bans both, so a player
  cannot rejoin under the same client or from the same address. Someone who reinstalls Terraria
  and changes address can come back.
- **Takaro shows a timed ban as permanent after its next ban sync** (a Takaro issue: the end date
  the connector reports is not copied). The ban on the server still ends on time.
- **Removing a ban that was made in the game from Takaro's ban list does not lift it** on the
  server; Takaro only lifts bans it made. Lift it in the game (`/ban del`) instead.
- Without the plugin the bridge falls back to a TShock name ban, which a player who is not
  logged in to a TShock account walks straight through, and timed bans are permanent.

## Other

- **Log lines from the bridge's own REST calls must be filtered out** (`logExcludePatterns`),
  or they flood Takaro's rate limit and real events are dropped. The default filter matches the
  REST user `takaro-rest`.
- **A shutdown request from Takaro reports success even when `enableShutdown=false` refused it.**
- **The world map needs Takaro protocol 1**, which Takaro's staging service offers and
  takaro.io does not yet; on protocol 0 the map stays off.
- **Commands starting with `/` never reach Takaro**: TShock handles them. Use Takaro's default
  `@` command prefix.
