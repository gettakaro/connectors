#!/usr/bin/env node
import { createWriteStream, mkdirSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { createRequire } from "node:module";

const require = createRequire(import.meta.url);
const fakeBanSteamId = "76561198000000000";

function argValue(name, fallback) {
  const index = process.argv.indexOf(name);
  if (index === -1 || index + 1 >= process.argv.length) return fallback;
  return process.argv[index + 1];
}

const projectRoot = resolve(new URL("..", import.meta.url).pathname);
const port = Number(argValue("--port", process.env.TF2_HARNESS_PORT || "23826"));
const timeoutMs = Number(argValue("--timeout-ms", process.env.TF2_HARNESS_TIMEOUT_MS || "90000"));
const logPath = resolve(argValue("--log", `${projectRoot}/_data/logs/tf2-live-harness.jsonl`));
const wsPackage = resolve(process.env.TF2_HARNESS_WS || `${projectRoot}/_data/harness/node_modules/ws`);
const { WebSocketServer } = require(wsPackage);

mkdirSync(dirname(logPath), { recursive: true });
const logStream = createWriteStream(logPath, { flags: "a" });

const requiredResponses = new Map([
  ["add-bot-live", (msg) => msg.payload?.success === true && typeof msg.payload?.rawResult === "string" && msg.success === undefined],
  ["reach-live", (msg) => msg.payload?.connectable === true && msg.payload?.connector === "takaro_tf2" && msg.payload?.map && msg.success === undefined],
  ["players-live", (msg) => Array.isArray(msg.payload) && msg.success === undefined],
  ["cmd-live", (msg) => msg.payload?.success === true && typeof msg.payload?.rawResult === "string" && msg.success === undefined],
  ["cmd-denied", (msg) => msg.payload?.success === false && msg.payload?.errorCode === "command-not-allowed"],
  ["chat-offline-refused", (msg) => msg.payload?.success === false && msg.payload?.errorCode === "player-not-found"],
  ["chat-live", (msg) => msg.payload === null && msg.success === undefined],
  ["items-live", (msg) => isGeneratedTf2SchemaCatalog(msg.payload) && msg.success === undefined],
  ["entities-live", (msg) => Array.isArray(msg.payload) && msg.payload.length > 0 && msg.payload.every(isEntityDto) && msg.success === undefined],
  ["locations-live", (msg) => Array.isArray(msg.payload) && msg.payload.length > 0 && msg.payload.every(isLocationDto) && msg.success === undefined],
  ["mapinfo-live", (msg) => msg.payload?.enabled === false && msg.payload?.mapBlockSize === 0 && msg.payload?.maxZoom === 0 && msg.success === undefined],
  ["maptile-fallback-live", (msg) => typeof msg.payload === "string" && msg.payload.startsWith("iVBOR") && msg.success === undefined],
  ["ban-live", (msg) => msg.payload === null && msg.success === undefined],
  ["listbans-live", (msg) => Array.isArray(msg.payload) && msg.payload.some((ban) => ban?.player?.gameId === fakeBanSteamId) && msg.success === undefined],
  ["unban-live", (msg) => msg.payload === null && msg.success === undefined],
  ["listbans-after-unban-live", (msg) => Array.isArray(msg.payload) && !msg.payload.some((ban) => ban?.player?.gameId === fakeBanSteamId) && msg.success === undefined],
  ["unsupported-live", (msg) => msg.payload?.success === false && msg.payload?.errorCode === "unsupported-action"],
  ["shutdown-disabled", (msg) => msg.payload?.success === false && msg.payload?.errorCode === "shutdown-disabled"]
]);

const optionalPlayerResponses = new Map([
  ["get-player-live", (msg) => msg.payload?.gameId && msg.success === undefined],
  ["chat-targeted-live", (msg) => msg.payload === null && msg.success === undefined],
  ["chat-sender-override-live", (msg) => msg.payload === null && msg.success === undefined],
  ["location-live", (msg) => typeof msg.payload?.x === "number" && msg.success === undefined],
  ["inventory-live", (msg) => Array.isArray(msg.payload) && msg.payload.length > 0 && msg.payload.every(isItemDto) && msg.payload.every((item) => !item.name.startsWith("tf_weapon")) && msg.success === undefined],
  ["teleport-live", (msg) => msg.payload === null && msg.success === undefined],
  ["kill-bot-live", (msg) => msg.payload?.success === true && typeof msg.payload?.rawResult === "string" && msg.success === undefined],
  ["kick-player-live", (msg) => msg.payload === null && msg.success === undefined]
]);

const state = {
  identified: false,
  pong: false,
  responses: new Map(),
  optionalResponses: new Map(),
  playerRequestsIssued: false,
  playerId: "",
  playerConnected: false,
  playerDeath: false,
  playerDisconnected: false,
  logEvent: false,
  killIssued: false,
  kickIssued: false,
  playersSeen: 0,
  completed: false
};

function log(event) {
  const entry = { ts: new Date().toISOString(), ...event };
  logStream.write(`${JSON.stringify(entry)}\n`);
  console.log(JSON.stringify(entry));
}

function messagePayload(msg) {
  return msg.payload && typeof msg.payload === "object" ? msg.payload : msg;
}

function gameEventType(msg) {
  return msg.eventType || msg.payload?.type;
}

function gameEventData(msg) {
  return msg.data || msg.payload?.data || {};
}

function isItemDto(item) {
  return item &&
    typeof item.code === "string" &&
    item.code.length > 0 &&
    typeof item.name === "string" &&
    item.name.length > 0 &&
    (item.amount === undefined || typeof item.amount === "number") &&
    (item.quality === undefined || typeof item.quality === "string");
}

function isGeneratedTf2SchemaCatalog(items) {
  return Array.isArray(items) &&
    items.length >= 300 &&
    items.every(isItemDto) &&
    items.some((item) => item.code === "13" && item.name === "Scattergun" && item.quality === "tf_weapon_scattergun") &&
    items.some((item) => item.code === "18" && item.name === "Rocket Launcher" && item.quality === "tf_weapon_rocketlauncher");
}

function isEntityDto(entity) {
  return entity &&
    typeof entity.code === "string" &&
    entity.code.length > 0 &&
    typeof entity.name === "string" &&
    entity.name.length > 0 &&
    typeof entity.type === "string";
}

function isLocationDto(location) {
  return location &&
    typeof location.code === "string" &&
    location.code.length > 0 &&
    typeof location.name === "string" &&
    location.name.length > 0 &&
    typeof location.position?.x === "number" &&
    typeof location.position?.y === "number" &&
    typeof location.position?.z === "number" &&
    typeof location.radius === "number";
}

function send(ws, msg) {
  log({ direction: "out", msg });
  ws.send(JSON.stringify(msg));
}

function request(ws, requestId, action, args = {}) {
  send(ws, { type: "request", requestId, payload: { action, args } });
}

function issueBaseRequests(ws) {
  request(ws, "add-bot-live", "executeConsoleCommand", {
    command: "tf_bot_add 1 scout red normal TakaroLiveBot noquota"
  });
  request(ws, "reach-live", "testReachability");
  request(ws, "cmd-live", "executeConsoleCommand", { command: "status" });
  request(ws, "cmd-denied", "executeConsoleCommand", { command: "quit" });
  request(ws, "chat-live", "sendMessage", { message: "Takaro TF2 live smoke" });
  request(ws, "items-live", "listItems");
  request(ws, "entities-live", "listEntities");
  request(ws, "locations-live", "listLocations");
  request(ws, "mapinfo-live", "getMapInfo");
  request(ws, "maptile-fallback-live", "getMapTile", { x: 0, y: 0, z: 0 });
  request(ws, "ban-live", "banPlayer", {
    player: { gameId: fakeBanSteamId },
    reason: "Takaro TF2 live smoke fake SteamID cleanup",
    expiresAt: "2031-12-31T23:59:59Z"
  });
  setTimeout(() => request(ws, "listbans-live", "listBans"), 500);
  setTimeout(() => request(ws, "unban-live", "unbanPlayer", { gameId: fakeBanSteamId }), 750);
  setTimeout(() => request(ws, "listbans-after-unban-live", "listBans"), 1000);
  request(ws, "unsupported-live", "giveItem", { itemCode: "rocket_launcher", amount: 1 });
  request(ws, "shutdown-disabled", "shutdown");
  request(ws, "chat-offline-refused", "sendMessage", {
    message: "Takaro TF2 offline recipient",
    opts: { recipient: { gameId: fakeBanSteamId } }
  });
  setTimeout(() => request(ws, "players-live", "getPlayers"), 1000);
}

function issuePlayerRequests(ws, player) {
  const identifier = player.gameId || player.steamId || player.name;
  if (!identifier) return;

  state.playerRequestsIssued = true;
  state.playerId = identifier;
  request(ws, "get-player-live", "getPlayer", { gameId: identifier });
  request(ws, "chat-targeted-live", "sendMessage", {
    message: "Takaro TF2 targeted live smoke",
    opts: { recipient: { gameId: identifier } }
  });
  request(ws, "chat-sender-override-live", "sendMessage", {
    message: "Takaro TF2 sender override live smoke",
    opts: {
      recipient: { gameId: identifier },
      senderNameOverride: "ModuleSmoke"
    }
  });
  request(ws, "location-live", "getPlayerLocation", { gameId: identifier });
  request(ws, "inventory-live", "getPlayerInventory", { gameId: identifier });
  request(ws, "teleport-live", "teleportPlayer", {
    gameId: identifier,
    x: 0,
    y: 0,
    z: 64
  });
}

function maybeComplete(wss) {
  if (state.completed) return;

  const missing = [];
  for (const [requestId, predicate] of requiredResponses) {
    const msg = state.responses.get(requestId);
    if (!msg || !predicate(msg)) missing.push(requestId);
  }

  if (!state.identified) missing.push("identify");
  if (!state.pong) missing.push("pong");
  if (!state.logEvent) missing.push("log-event");
  if (state.playerRequestsIssued) {
    for (const [requestId, predicate] of optionalPlayerResponses) {
      const msg = state.optionalResponses.get(requestId);
      if (!msg || !predicate(msg)) missing.push(requestId);
    }
    if (!state.playerConnected) missing.push("player-connected-event");
    if (!state.playerDeath) missing.push("player-death-event");
    if (!state.playerDisconnected) missing.push("player-disconnected-event");
  }

  if (missing.length) return;

  state.completed = true;
  log({
    event: "pass",
    responses: [...state.responses.keys()],
    optionalResponses: [...state.optionalResponses.keys()],
    playersSeen: state.playersSeen,
    playerConnected: state.playerConnected,
    playerDeath: state.playerDeath,
    playerDisconnected: state.playerDisconnected
  });
  for (const client of wss.clients) {
    client.terminate();
  }
  wss.close(() => {
    logStream.end(() => process.exit(0));
  });
  setTimeout(() => {
    logStream.end(() => process.exit(0));
  }, 250).unref();
}

const wss = new WebSocketServer({ port });
log({ event: "listening", port });

const timeout = setTimeout(() => {
  const missing = [];
  for (const requestId of requiredResponses.keys()) {
    if (!state.responses.has(requestId)) missing.push(requestId);
  }
  if (!state.identified) missing.push("identify");
  if (!state.pong) missing.push("pong");
  if (!state.logEvent) missing.push("log-event");
  if (state.playerRequestsIssued) {
    for (const requestId of optionalPlayerResponses.keys()) {
      if (!state.optionalResponses.has(requestId)) missing.push(requestId);
    }
    if (!state.playerConnected) missing.push("player-connected-event");
    if (!state.playerDeath) missing.push("player-death-event");
    if (!state.playerDisconnected) missing.push("player-disconnected-event");
  }

  log({
    event: "fail",
    reason: "timeout",
    missing,
    responses: [...state.responses.keys()],
    optionalResponses: [...state.optionalResponses.keys()],
    playersSeen: state.playersSeen
  });
  logStream.end();
  process.exit(1);
}, timeoutMs);
timeout.unref();

wss.on("connection", (ws) => {
  log({ event: "connection" });

  ws.on("message", (raw) => {
    let msg;
    try {
      msg = JSON.parse(raw.toString());
    } catch (error) {
      log({ direction: "in", parseError: error.message, raw: raw.toString() });
      return;
    }

    log({ direction: "in", msg });

    if (msg.type === "identify") {
      const payload = messagePayload(msg);
      state.identified = Boolean(
        payload.name &&
          Object.prototype.hasOwnProperty.call(payload, "identityToken") &&
          Object.prototype.hasOwnProperty.call(payload, "registrationToken") &&
          payload.game === undefined &&
          payload.connector === undefined
      );
      send(ws, { type: "connected" });
      send(ws, { type: "identifyResponse", payload: { gameServerId: "tf2-live-harness" } });
      send(ws, { type: "ping", requestId: "ping-live" });
      setTimeout(() => issueBaseRequests(ws), 250);
      return;
    }

    if (msg.type === "pong" && msg.requestId === "ping-live") {
      state.pong = true;
      maybeComplete(wss);
      return;
    }

    if (msg.type === "gameEvent" && gameEventType(msg) === "player-connected") {
      const data = gameEventData(msg);
      state.playerConnected = Boolean(data.player?.gameId && data.player?.name);
      maybeComplete(wss);
      return;
    }

    if (msg.type === "gameEvent" && gameEventType(msg) === "log") {
      const data = gameEventData(msg);
      state.logEvent = typeof data.msg === "string" &&
        !Object.prototype.hasOwnProperty.call(data, "level") &&
        !Object.prototype.hasOwnProperty.call(data, "message") &&
        !Object.prototype.hasOwnProperty.call(data, "source");
      maybeComplete(wss);
      return;
    }

    if (msg.type === "gameEvent" && gameEventType(msg) === "player-death") {
      const data = gameEventData(msg);
      state.playerDeath = Boolean(
        data.player?.gameId &&
          Object.prototype.hasOwnProperty.call(data, "attacker") &&
          typeof data.position?.x === "number"
      );
      if (state.playerId && !state.kickIssued) {
        state.kickIssued = true;
        request(ws, "kick-player-live", "kickPlayer", {
          gameId: state.playerId,
          reason: "Takaro TF2 live smoke cleanup"
        });
      }
      maybeComplete(wss);
      return;
    }

    if (msg.type === "gameEvent" && gameEventType(msg) === "player-disconnected") {
      const data = gameEventData(msg);
      state.playerDisconnected = Boolean(data.player?.gameId && data.player?.name);
      maybeComplete(wss);
      return;
    }

    if (msg.type === "response" && msg.requestId) {
      if (requiredResponses.has(msg.requestId)) {
        state.responses.set(msg.requestId, msg);
      }

      if (optionalPlayerResponses.has(msg.requestId)) {
        state.optionalResponses.set(msg.requestId, msg);
      }

      if (msg.requestId === "players-live" && Array.isArray(msg.payload) && msg.payload.length > 0) {
        state.playersSeen = msg.payload.length;
        issuePlayerRequests(ws, msg.payload[0]);
      }

      if (msg.requestId === "teleport-live" && msg.payload === null && !state.killIssued) {
        state.killIssued = true;
        request(ws, "kill-bot-live", "executeConsoleCommand", { command: "tf_bot_kill all" });
      }

      maybeComplete(wss);
    }
  });
});
