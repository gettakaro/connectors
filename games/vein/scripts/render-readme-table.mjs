#!/usr/bin/env node
// Regenerates the "What works, what doesn't" table of games/vein/README.md from a
// capabilities.json (the campaign's evidence matrix).
//
//   node scripts/render-readme-table.mjs <capabilities.json> [--write <README.md>]
//
// Without --write the table is printed to stdout. With --write the markdown table inside
// the README's "## What works, what doesn't" section (everything up to the next "###"
// heading) is replaced in place - the README carries no generator markers.
//
// The row set is fixed (49 rows, identical to the Dragonwilds connector README) so the
// two connectors stay comparable. Only the symbol comes from capabilities.json; the note
// is authored here, optionally per status.
//
// Symbols: proven -> OK; partial, plugin-proven and any "awaiting-*" status -> CAVEAT;
//          blocked, pending and anything unknown -> NO.

import { readFileSync, writeFileSync } from 'node:fs';

const OK = '✅';      // white heavy check mark
const CAVEAT = '⚠️'; // warning sign
const NO = '❌';      // cross mark

const HEADING = "## What works, what doesn't";

/** @type {Array<{label:string, keys?:string[], fixed?:string, note:string, notes?:Record<string,string>}>} */
const ROWS = [
  // ---- actions (17 capabilities, 18 rows: sendMessage covers broadcast + whisper) ----
  { label: 'Connection & heartbeat', keys: ['actions.testReachability'],
    note: 'The server shows as reachable while the sidecar is up.' },
  { label: 'Player list', keys: ['actions.getPlayers'],
    note: 'Name, ping and spawned state; `gameId` is the SteamID64.' },
  { label: 'Single player lookup', keys: ['actions.getPlayer'],
    note: 'Same data for one player; offline players answer with a last-known record.' },
  { label: 'Player location', keys: ['actions.getPlayerLocation'],
    note: 'The live position, following walking and teleports.' },
  { label: 'Player inventory', keys: ['actions.getPlayerInventory'],
    note: 'Matches the in-game bag; empty while the player is dead.',
    notes: { partial: 'What the player is carrying, read by reflection from the inventory component. Matches the in-game bag, but it was not re-checked against Takaro after every item type.' } },
  { label: 'Give an item', keys: ['actions.giveItem'],
    note: 'The item appears in the bag without a relog.',
    notes: { failed: 'Does not work yet. The game’s own give-item path is wired up but the item does not reach the player, so Takaro’s give and the shop’s delivery both fail.' } },
  { label: 'Item catalogue', keys: ['actions.listItems'],
    note: 'Every loaded item class. Readable names need `TAKARO_ITEM_NAMES=1`.',
    notes: { partial: 'Every item class the server has loaded. Names are the internal class names unless `TAKARO_ITEM_NAMES=1` is set, which asks the game for the readable name at start-up.' } },
  { label: 'Entity catalogue', keys: ['actions.listEntities'],
    note: 'Creature types sync, but Takaro never deletes rows from older builds.',
    notes: { partial: 'Creature types sync, but Takaro never deletes rows from older builds.' } },
  { label: 'Locations / points of interest', keys: ['actions.listLocations'],
    note: 'Served by the connector, but Takaro never asks for them.',
    notes: { partial: 'Served by the connector, but Takaro never asks for them.' } },
  { label: 'Run a console command', keys: ['actions.executeConsoleCommand'],
    note: 'The connector\'s own set: `help`, `players`, `say`, `give`, `tp`, `ban`, `save`, …' },
  { label: 'Broadcast a message', keys: ['actions.sendMessage'],
    note: 'Everyone sees it, as a chat line prefixed with the server name.' },
  { label: 'Whisper a player', keys: ['actions.sendMessage'],
    note: 'Reaches the one player, as an on-screen notification.' },
  { label: 'Teleport a player', keys: ['actions.teleportPlayer'],
    note: 'The player is moved to the requested position.',
    notes: { failed: 'Does not work yet. The request is accepted but the player does not move, so the teleports module and Takaro’s teleport both fail.' } },
  { label: 'Kick a player', keys: ['actions.kickPlayer'],
    note: 'The player is dropped and can rejoin afterwards.',
    notes: { failed: 'Does not work yet. The request reports success but the player stays connected. Use a ban followed by an unban to remove someone.' } },
  { label: 'Ban a player (timed and permanent)', keys: ['actions.banPlayer'],
    note: 'Works offline too; the connector lifts timed bans at expiry.' },
  { label: 'Unban a player', keys: ['actions.unbanPlayer'],
    note: 'The player can rejoin at once.' },
  { label: 'Ban list', keys: ['actions.listBans'],
    note: 'The game\'s bans plus the connector\'s, with reason and expiry.' },
  { label: 'Shut the server down', keys: ['actions.shutdown'],
    note: 'Saves the world first, then quits cleanly.' },

  // ---- events (6) ----
  { label: 'Player joined event', keys: ['events.player-connected'],
    note: 'Arrives on every join, with the SteamID64.' },
  { label: 'Player left event', keys: ['events.player-disconnected'],
    note: 'Arrives on a clean quit and after a crash.' },
  { label: 'Player chat event', keys: ['events.chat-message'],
    note: 'Real player chat reaches Takaro; the connector\'s own messages are not echoed.' },
  { label: 'Player death event', keys: ['events.player-death'],
    note: 'Position and cause included; falls and drowning have no attacker.',
    notes: { failed: 'Not working yet — player deaths are not reported to Takaro.' } },
  { label: 'Entity kill event', keys: ['events.entity-killed'],
    note: 'Creature, player and held item. AI-on-AI kills are not reported.',
    notes: { failed: 'Not working yet — creature kills are not reported to Takaro.' } },
  { label: 'Log events', keys: ['events.log'],
    note: 'Forwarded with secrets redacted, but Takaro does not store log events.',
    notes: { partial: 'Forwarded with secrets redacted, but Takaro does not store log events.',
             failed: 'The connector forwards server log lines (redacted), but Takaro does not store log lines as events, so they cannot be searched or used in modules.' } },

  // ---- map: protocol version 1 Dimension, picture uploaded by the operator ----
  { label: 'Map info', fixed: OK, note: 'The connector declares the 12 km map area to Takaro, so player dots land in the right place.' },
  { label: 'Map tiles', fixed: CAVEAT, note: 'The server files carry no map picture. Upload the in-game map once in the Takaro dashboard ([INSTALL.md](INSTALL.md#world-map)).' },

  // ---- modules (7 capabilities, 5 rows) ----
  { label: 'Modules: chat commands', keys: ['modules.command'],
    note: 'In-game commands reach the module and answer in chat.',
    notes: { failed: 'Not verified in game yet — it needs a human to type a command at the keyboard.' } },
  { label: 'Modules: hooks', keys: ['modules.hook'],
    note: 'Chat and join hooks fire and run their code.' },
  { label: 'Modules: cronjobs', keys: ['modules.cronjob'],
    note: 'Scheduled module runs fire and can message the server.',
    notes: { partial: 'Scheduled module runs fire on their own schedule and run their code; the in-game side effect was not checked for every run.' } },
  { label: 'Modules: teleports (`@settp`, `@tp`, …)', keys: ['modules.teleports'],
    note: 'The teleports module\'s in-game commands move the player.',
    notes: { failed: 'Blocked by the teleport action above — the commands are accepted but the player does not move.' } },
  { label: 'Modules: server messages / onboarding', keys: ['modules.serverMessages', 'modules.playerOnboarding'],
    note: 'Timed messages and the welcome message are delivered in game.',
    notes: { partial: 'The welcome message on join is delivered in game; timed server messages fire on schedule and were checked on the wire rather than for every in-game line.' } },

  // ---- shop + economy (7) ----
  { label: 'Shop: buy in game', keys: ['shop.buyInGame'],
    note: 'Buying with the in-game chat command works.',
    notes: { failed: 'Not verified in game yet — it needs a human to run the in-game shop commands, and item delivery depends on the give action above.' } },
  { label: 'Shop: order in Takaro and claim in game', keys: ['shop.orderViaMcp'],
    note: 'The items are delivered to the player.',
    notes: { partial: 'The order is paid and claimed and the currency moves, but the items do not actually arrive while the give action is broken.' } },
  { label: 'Shop: bundle of several items', keys: ['shop.multiItemBundle'],
    note: 'One claim delivers every item in the listing.',
    notes: { partial: 'One claim does issue a delivery for every item in the listing; the items themselves do not arrive while the give action is broken.' } },
  { label: 'Shop: order while offline, claim later', keys: ['shop.orderWhileOffline'],
    note: 'Refused while offline, succeeds after rejoining.',
    notes: { partial: 'The claim is correctly refused while the player is offline and succeeds after the player rejoins. Not yet followed as one single order from offline all the way to the items counted in the world.' } },
  { label: 'Shop: not enough currency', keys: ['shop.insufficientFunds'],
    note: 'The purchase is refused and the balance is unchanged.' },
  { label: 'Economy: currency', keys: ['shop.economyEnabled'],
    note: 'Balances are set, read and debited by Takaro.' },
  { label: 'Economy: balance / top list in game', keys: ['shop.balance'],
    note: 'The in-game economy commands answer in chat.',
    notes: { failed: 'Not verified in game yet — it needs a human to type the balance command.' } },

  // ---- Discord (6 capabilities, 5 rows; noEcho shares a row with chatBridgeNoEcho) ----
  { label: 'Discord: game chat → Discord', keys: ['discord.gameToDiscord'],
    note: 'In-game chat is relayed to the linked channel.',
    notes: { failed: 'Not verified with a real in-game chat line yet; the bridge itself accepts and posts player-authored messages.' } },
  { label: 'Discord: Discord → game chat', keys: ['discord.discordToGame'],
    note: 'Works, but only a real human post can confirm it — bots are ignored.',
    notes: { partial: 'Works, but only a real human post can confirm it — bots are ignored.' } },
  { label: 'Discord: module hook / cronjob posts', keys: ['discord.customHookToDiscord', 'discord.customCronToDiscord'],
    note: 'Hooks and cronjobs post to Discord and edit their messages.' },
  { label: 'Discord: join/leave notices', keys: ['discord.joinLeaveNotices'],
    note: 'Posted to Discord by the chat-bridge module.' },
  { label: 'Discord: no echo of server messages', keys: ['discord.noEcho', 'modules.chatBridgeNoEcho'],
    note: 'Stock `chatBridge` re-posts server messages; the `chatBridgeNoEcho` fork does not.' },

  // ---- resilience (9 capabilities, 6 rows) ----
  { label: 'Events while the Takaro connection is down', keys: ['resilience.outageReconnect'],
    note: 'Kept and delivered in order once the connection is back.',
    notes: { failed: 'Designed for and implemented (queued events, ordered delivery after reconnect) but not yet exercised against a real outage.' } },
  { label: 'Reconnects after a server or container restart',
    keys: ['resilience.containerRestartReconnect', 'resilience.serverRestartOnlineReconcile', 'resilience.pluginRestartDetection'],
    note: 'Re-identifies on its own; keep the sidecar\'s restart policy on.',
    notes: { failed: 'Implemented (restart detection, online-set reconciliation, a fresh plugin boot id resets the cursor) but not yet exercised end to end.' } },
  { label: 'No duplicate events after a connector restart', keys: ['resilience.sidecarRestartNoReplay'],
    note: 'The event cursor is persisted, so nothing is replayed.',
    notes: { failed: 'The event cursor is persisted so a sidecar restart should replay nothing; not yet verified against Takaro’s event totals.' } },
  { label: 'Survives a network drop to Takaro', keys: ['resilience.wsDropReconnect'],
    note: 'The WebSocket reconnects by itself and re-identifies.',
    notes: { failed: 'The WebSocket is built to reconnect by itself with a backoff of 2 to 60 seconds; not yet verified against a forced socket drop.' } },
  { label: 'Timed bans expire on their own', keys: ['resilience.timedBanLiftedByConnector'],
    note: 'Lifted at expiry, including across a restart.',
    notes: { failed: 'Implemented — the connector lifts a timed ban at expiry because Takaro sends no unban — but not yet verified across a restart.' } },
  { label: 'Keeps running after a game update breaks a feature', keys: ['resilience.symbolSelfCheckDegrade'],
    note: 'The broken feature reports `degraded`; everything else keeps working.',
    notes: { failed: 'The plugin self-checks at load and marks a feature it can no longer find as `degraded` in `/health` while the server keeps running. Built and unit-tested, not yet exercised with a deliberately broken build on the live rig.' } },
];

const CLASS = {
  proven: 'proven',
  partial: 'partial',
  'plugin-proven': 'partial',
  blocked: 'failed',
  pending: 'failed',
};
// A status of the form "awaiting-<someone>" means one human action is outstanding,
// not that the feature failed.
const classify = (status) => CLASS[status] ?? (String(status).startsWith('awaiting-') ? 'partial' : 'failed');
const SYMBOL = { proven: OK, partial: CAVEAT, failed: NO };
const RANK = { proven: 0, partial: 1, failed: 2 };

function lookup(caps, dotted) {
  const [section, ...rest] = dotted.split('.');
  const key = rest.join('.');
  const row = caps?.[section]?.[key];
  if (!row) throw new Error(`capabilities.json has no ${dotted}`);
  return row.status;
}

function render(caps) {
  const lines = ['| What | | Notes |', '|---|---|---|'];
  for (const row of ROWS) {
    let cls;
    if (row.fixed) {
      cls = row.fixed === OK ? 'proven' : row.fixed === CAVEAT ? 'partial' : 'failed';
    } else {
      cls = row.keys
        .map((k) => classify(lookup(caps, k)))
        .reduce((a, b) => (RANK[b] > RANK[a] ? b : a), 'proven');
    }
    const note = row.notes?.[cls] ?? row.note;
    lines.push(`| ${row.label} | ${SYMBOL[cls]} | ${note} |`);
  }
  return lines.join('\n');
}

const args = process.argv.slice(2);
const capsPath = args[0];
if (!capsPath) {
  console.error('usage: render-readme-table.mjs <capabilities.json> [--write <README.md>]');
  process.exit(2);
}
const caps = JSON.parse(readFileSync(capsPath, 'utf8'));
const table = render(caps);

const writeIdx = args.indexOf('--write');
if (writeIdx === -1) {
  console.log(table);
} else {
  const readmePath = args[writeIdx + 1];
  if (!readmePath) { console.error('--write needs a README path'); process.exit(2); }
  const readme = readFileSync(readmePath, 'utf8');
  const lines = readme.split('\n');
  const start = lines.findIndex((l) => l.trim() === HEADING);
  if (start === -1) { console.error(`heading "${HEADING}" not found in ${readmePath}`); process.exit(1); }
  let end = lines.findIndex((l, i) => i > start && l.startsWith('### '));
  if (end === -1) end = lines.length;
  const first = lines.findIndex((l, i) => i > start && i < end && l.startsWith('|'));
  if (first === -1) { console.error(`no table in the "${HEADING}" section of ${readmePath}`); process.exit(1); }
  let last = first;
  while (last + 1 < end && lines[last + 1].startsWith('|')) last += 1;
  lines.splice(first, last - first + 1, table);
  writeFileSync(readmePath, lines.join('\n'));
  console.error(`rewrote ${ROWS.length} rows in ${readmePath}`);
}
