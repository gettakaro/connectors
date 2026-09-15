#!/usr/bin/env node
// Validates context/games/enshrouded/module-proof-2026-09-13.json against Takaro MCP (the oracle):
// every referenced event id must exist with the expected eventName, gameserver and module, and the
// referenced modules must still be installed on the gameserver (unless the proof marks them uninstalled).
// Discord checks (when the proof has a "discord" block): the guild is known to Takaro (and takaroEnabled),
// the channel exists by name + id suffix, chatBridge is wired to that channel, and per event:
//   discordPost  -> function logs contain "POST /discord/channels/<channel>/message 200 OK"
//   gameDelivery -> function logs contain "POST /gameserver/<id>/message 200 OK"
//   noLogs       -> function ran successfully without any API call (e.g. bot-authored message ignored)
//   hookName     -> meta.hook.name matches
// Ids in the proof are suffix-only for Discord; full ids are resolved live. Read-only: nothing is posted.
//
// Usage: node context/games/enshrouded/source/scripts/validate-module-proof.mjs [proof.json]
// Env: TAKARO_MCP_URL (default http://127.0.0.1:3000/mcp)

import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));
const proofPath = process.argv[2] || path.resolve(here, "../../../module-proof-2026-09-13.json");
const mcpUrl = process.env.TAKARO_MCP_URL || "http://127.0.0.1:3000/mcp";

const ok = [];
const failures = [];
const record = (cond, pass, fail) => (cond ? ok.push(pass) : failures.push(fail));

async function rpc(session, method, params) {
  const headers = { "content-type": "application/json", accept: "application/json, text/event-stream" };
  if (session) headers["mcp-session-id"] = session;
  const res = await fetch(mcpUrl, { method: "POST", headers, body: JSON.stringify({ jsonrpc: "2.0", id: Date.now(), method, params }) });
  if (!res.ok) throw new Error(`${method} HTTP ${res.status}`);
  const text = await res.text();
  const raw = text.startsWith("event:") || text.startsWith("data:")
    ? text.split("\n").filter((l) => l.startsWith("data:")).map((l) => l.slice(5)).join("\n")
    : text;
  return { body: raw ? JSON.parse(raw) : {}, session: res.headers.get("mcp-session-id") };
}

async function tool(session, name, args) {
  const { body } = await rpc(session, "tools/call", { name, arguments: args });
  if (body.error) throw new Error(`${name}: ${JSON.stringify(body.error)}`);
  const t = body.result?.content?.find((c) => c.type === "text")?.text;
  return typeof t === "string" ? JSON.parse(t) : body.result;
}

async function main() {
  const proof = JSON.parse(fs.readFileSync(proofPath, "utf8"));
  const gameServerId = proof.gameServer.id;
  const init = await rpc(null, "initialize", { protocolVersion: "2025-03-26", capabilities: {}, clientInfo: { name: "enshrouded-module-proof", version: "1" } });
  const session = init.session;
  if (!session) throw new Error("MCP initialize returned no mcp-session-id");
  await rpc(session, "notifications/initialized", undefined).catch(() => {});

  const installed = await tool(session, "moduleinstallationsGetInstalledModules", { filters: { gameserverId: [gameServerId] }, limit: 100 });
  const installedIds = new Set((installed?.data ?? []).map((i) => i.moduleId));
  for (const m of proof.modules) {
    if (m.uninstalledAfterProof) continue;
    record(installedIds.has(m.id), `module installed: ${m.name}`, `module not installed on gameserver: ${m.name} (${m.id})`);
  }

  let discordChannelId = null;
  if (proof.discord) {
    const d = proof.discord;
    const guilds = (await tool(session, "discordSearch", { limit: 100 }))?.data ?? [];
    const guild = guilds.find((g) => String(g.discordId).endsWith(d.guildIdSuffix));
    record(!!guild, `discord guild ...${d.guildIdSuffix} known to Takaro`, `discord guild ...${d.guildIdSuffix} not in discordSearch (bot not invited / domain not linked)`);
    if (guild && d.requireTakaroEnabled) record(guild.takaroEnabled === true, `discord guild ...${d.guildIdSuffix} takaroEnabled`, `discord guild ...${d.guildIdSuffix} has takaroEnabled=false`);
    if (guild) {
      const channels = (await tool(session, "discordGetChannels", { id: guild.discordId }))?.data ?? [];
      const ch = channels.find((c) => String(c.id).endsWith(d.channelIdSuffix) && c.name === d.channelName);
      record(!!ch, `discord channel #${d.channelName} ...${d.channelIdSuffix} exists`, `discord channel #${d.channelName} ...${d.channelIdSuffix} not found`);
      discordChannelId = ch ? String(ch.id) : null;
    }
    if (d.chatBridgeModuleId && discordChannelId) {
      const inst = (installed?.data ?? []).find((i) => i.moduleId === d.chatBridgeModuleId);
      const wired = inst?.systemConfig?.hooks?.DiscordToGame?.discordChannelId;
      record(wired === discordChannelId, `chatBridge wired to #${d.channelName}`, `chatBridge DiscordToGame channel ${wired ? "..." + String(wired).slice(-4) : "unset"} != ...${d.channelIdSuffix}`);
    }
  }

  for (const step of proof.proofSteps) {
    for (const ev of step.events) {
      let got;
      try {
        got = (await tool(session, "eventGetOne", { id: ev.eventId }))?.data;
      } catch (err) {
        failures.push(`${step.path}: eventGetOne ${ev.eventId} failed: ${err.message}`);
        continue;
      }
      const good = got && got.eventName === ev.eventName && got.gameserverId === gameServerId && (!ev.moduleId || got.moduleId === ev.moduleId);
      record(good, `${step.path}: ${ev.eventName} ${ev.eventId}`, `${step.path}: event ${ev.eventId} mismatch (got ${got?.eventName}/${got?.gameserverId}/${got?.moduleId})`);
      if (good && ev.msgIncludes) {
        const msg = got.meta?.msg ?? "";
        record(String(msg).includes(ev.msgIncludes), `${step.path}: msg contains "${ev.msgIncludes}"`, `${step.path}: msg "${msg}" lacks "${ev.msgIncludes}"`);
      }
      if (!good) continue;
      const result = got.meta?.result;
      const logs = (result?.logs ?? []).map((l) => String(l.msg ?? ""));
      if (ev.hookName) {
        record(got.meta?.hook?.name === ev.hookName, `${step.path}: hook ${ev.hookName}`, `${step.path}: hook name ${got.meta?.hook?.name} != ${ev.hookName}`);
      }
      if (ev.discordPost || ev.gameDelivery || ev.noLogs) {
        record(result?.success === true, `${step.path}: ${ev.eventId} success`, `${step.path}: ${ev.eventId} result.success=${result?.success}`);
      }
      if (ev.discordPost) {
        const want = discordChannelId ? `POST /discord/channels/${discordChannelId}/message 200 OK` : null;
        record(!!want && logs.some((l) => l.includes(want)), `${step.path}: Discord API 200 to #${proof.discord?.channelName}`, `${step.path}: no "POST /discord/channels/...${proof.discord?.channelIdSuffix}/message 200 OK" in logs`);
      }
      if (ev.gameDelivery) {
        const want = `POST /gameserver/${gameServerId}/message 200 OK`;
        record(logs.some((l) => l.includes(want)), `${step.path}: gameserver message 200`, `${step.path}: no "${want}" in logs`);
      }
      if (ev.noLogs) {
        record(logs.length === 0, `${step.path}: no API calls (message ignored)`, `${step.path}: expected no logs, got ${logs.length}`);
      }
    }
  }

  for (const line of ok) console.log(`OK: ${line}`);
  for (const line of failures) console.error(`FAIL: ${line}`);
  console.log(failures.length ? `\nEnshrouded module proof FAILED (${failures.length})` : `\nEnshrouded module proof passed (${ok.length} checks).`);
  process.exit(failures.length ? 1 : 0);
}

main().catch((err) => {
  console.error(`FAIL: ${err instanceof Error ? err.message : String(err)}`);
  process.exit(1);
});
