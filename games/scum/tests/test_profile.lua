-- Game Profile: game + connector identity in identify, the game.profile capability (name, game build, icon revision
-- and size) and getGameIcon (the declared icon's bytes as base64), the sha256/base64 helpers and the icon checks.
local t = require("lib.t")
local F = require("lib.fakes")
local json = require("takaro.json")
local sha256 = require("takaro.sha256")
local profile = require("takaro.profile")
local App = require("takaro.app")

local function read(path)
  local f = assert(io.open(path, "rb"))
  local s = f:read("a")
  f:close()
  return s
end

local ICON = read("assets/icon-256.png")

local function names(manifest)
  local out = {}
  for _, c in ipairs(manifest) do out[c.name] = c end
  return out
end

local function b64decode(s)
  local A = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"
  local out = {}
  s = s:gsub("=", "")
  local bits, n = 0, 0
  for i = 1, #s do
    bits = (bits << 6) | (A:find(s:sub(i, i), 1, true) - 1)
    n = n + 6
    if n >= 8 then
      n = n - 8
      out[#out + 1] = string.char((bits >> n) & 255)
    end
  end
  return table.concat(out)
end

local function app_v1(opts)
  opts = opts or {}
  opts.files = opts.files or { ["/mod/icon-256.png"] = ICON }
  local a, native, tk, g, fs = F.app(opts)
  native.open()
  tk.pump()
  native.deliver({ type = "identifyResponse", payload = { gameServerId = "gs-1", protocolVersion = 1 } })
  tk.pump()
  return a, native, tk, g, fs
end

t.test("sha256 matches the FIPS 180-2 vectors (one block, two blocks, a million bytes)", function()
  t.eq(sha256.hex(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855")
  t.eq(sha256.hex("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")
  t.eq(sha256.hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
    "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1")
  t.eq(sha256.hex(string.rep("a", 1000000)), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0")
end)

t.test("base64 matches RFC 4648 and round-trips the icon", function()
  local cases = { [""] = "", f = "Zg==", fo = "Zm8=", foo = "Zm9v", foob = "Zm9vYg==", fooba = "Zm9vYmE=",
    foobar = "Zm9vYmFy" }
  for k, v in pairs(cases) do t.eq(sha256.base64(k), v, k) end
  t.eq(b64decode(sha256.base64(ICON)), ICON)
end)

t.test("the shipped icon is a 256x256 PNG within 128 KiB; revision = first 12 hex of its sha256", function()
  local icon, why = profile.load_icon(F.fs({ ["/mod/icon-256.png"] = ICON }), "/mod")
  t.ok(icon, why)
  t.eq(icon.size, #ICON)
  t.ok(icon.size <= 131072, "at most 128 KiB")
  t.eq(icon.revision, sha256.hex(ICON):sub(1, 12))
  t.ok(icon.revision:match("^%x+$") and #icon.revision == 12)
end)

t.test("an icon Takaro would reject is not declared", function()
  local function load(bytes) return profile.load_icon(F.fs({ ["/m/icon-256.png"] = bytes }), "/m") end
  t.eq((load(nil)), nil, "missing")
  t.eq((load("GIF89a" .. string.rep("\0", 40))), nil, "not a PNG")
  local wrongSize = ICON:sub(1, 16) .. string.pack(">I4>I4", 128, 128) .. ICON:sub(25)
  local icon, why = load(wrongSize)
  t.eq(icon, nil)
  t.ok(why:find("128x128", 1, true), why)
  t.eq((load(ICON .. string.rep("\0", 131073 - #ICON))), nil, "over 128 KiB")
end)

t.test("identify sends game scum and connector takaro-scum with the release version (= version.txt)", function()
  local _, native, tk = F.app({ files = { ["/mod/icon-256.png"] = ICON } })
  native.open()
  tk.pump()
  local id = native.frames("identify")[1].msg.payload
  t.eq(id.game, "scum")
  t.deep(id.connector, { name = "takaro-scum", version = App.VERSION })
  t.eq(App.VERSION, (read("version.txt"):gsub("%s+$", "")), "connector version is the release-please version")
  t.ok(App.VERSION:match("^%d+%.%d+%.%d+$") or App.VERSION:match("^%d+%.%d+%.%d+[-+][%w.+-]+$"), "strict semver")
  t.ok(id.game:match("^[a-z0-9][a-z0-9-]*$") and id.connector.name:match("^[a-z0-9][a-z0-9-]*$"))
end)

t.test("identify sends game and connector on protocol 0 too, without a manifest", function()
  local _, native, tk = F.app({ extra = "TAKARO_PROTOCOL=0", files = { ["/mod/icon-256.png"] = ICON } })
  native.open()
  tk.pump()
  local id = native.frames("identify")[1].msg.payload
  t.eq(id.game, "scum")
  t.eq(id.connector.name, "takaro-scum")
  t.eq(id.capabilities, nil)
end)

t.test("the manifest declares game.profile: name SCUM and the icon's revision and size", function()
  local _, native = app_v1()
  local caps = names(native.frames("identify")[1].msg.payload.capabilities)
  local p = caps["game.profile"]
  t.ok(p, "game.profile declared")
  t.deep(p.parameters, { name = "SCUM", icon = { revision = sha256.hex(ICON):sub(1, 12), size = #ICON } })
end)

t.test("getGameIcon answers the declared revision and the whole PNG as base64", function()
  local _, native, tk = app_v1()
  local p = names(native.frames("identify")[1].msg.payload.capabilities)["game.profile"].parameters
  local rid = tk.request("getGameIcon", "{}")
  tk.pump()
  local payload, s = tk.response(rid)
  t.eq(s.msg.error, nil)
  t.eq(payload.revision, p.icon.revision)
  local bytes = b64decode(payload.data)
  t.eq(#bytes, p.icon.size)
  t.eq(bytes, ICON)
end)

t.test("without an icon file: no icon declared, getGameIcon answers game_error", function()
  local _, native, tk = app_v1({ files = {} })
  local p = names(native.frames("identify")[1].msg.payload.capabilities)["game.profile"]
  t.deep(p.parameters, { name = "SCUM" })
  local rid = tk.request("getGameIcon", "{}")
  tk.pump()
  local _, s = tk.response(rid)
  t.eq(s.msg.error.code, "game_error")
end)

t.test("getGameIcon is not a protocol 0 action", function()
  local _, _, tk = F.app({ files = { ["/mod/icon-256.png"] = ICON } })
  tk.connect()
  local rid = tk.request("getGameIcon", "{}")
  tk.pump()
  local payload = tk.response(rid)
  t.eq(payload.success, false)
  t.ok(payload.errorMessage:find("unknown action", 1, true))
end)

t.test("the game build is declared once the engine has it: capabilities replacement with version", function()
  local g = F.game()
  local value = nil
  g.game_version = function() return value and { ok = true, value = value } or { ok = false, reason = "not yet" } end
  local _, native, tk = app_v1({ game = g })
  local p = names(native.frames("identify")[1].msg.payload.capabilities)["game.profile"].parameters
  t.eq(p.version, nil, "unknown at identify")
  value = "1.3.4.2.161892+0"
  native.clock = native.clock + 10000
  tk.pump()
  local caps = native.frames("capabilities")
  t.eq(#caps, 1)
  local m = names(caps[1].msg.payload.capabilities)
  t.eq(m["game.profile"].parameters.version, "1.3.4.2.161892")
  t.eq(m["game.profile"].parameters.icon.revision, p.icon.revision)
  t.ok(m.map and m["players.list"], "the full manifest is sent again")
  native.clock = native.clock + 10000
  tk.pump()
  t.eq(#native.frames("capabilities"), 1, "read once")
end)

t.test("a game build known before identify goes into the identify manifest", function()
  local g = F.game()
  g.game_version = function() return { ok = true, value = "1.3.4.2.161892+0" } end
  local _, native = app_v1({ game = g })
  local p = names(native.frames("identify")[1].msg.payload.capabilities)["game.profile"].parameters
  t.eq(p.version, "1.3.4.2.161892")
  t.eq(#native.frames("capabilities"), 0, "nothing to replace")
end)

t.test("game build cleaning: net suffix dropped; too long or non-printable is left out", function()
  t.eq(profile.clean_version("1.3.4.2.161892+0"), "1.3.4.2.161892")
  t.eq(profile.clean_version(" 1.3.4.2 "), "1.3.4.2")
  t.eq(profile.clean_version("1.3.4.2.161892+0\r\n"), "1.3.4.2.161892")
  t.eq(profile.clean_version(string.rep("1", 33)), nil)
  t.eq(profile.clean_version("1.3\n"), "1.3")
  t.eq(profile.clean_version("1.\0013"), nil)
  t.eq(profile.clean_version(""), nil)
  t.eq(profile.clean_version(nil), nil)
end)

t.test("the manifest stays within Takaro's game.profile limits", function()
  local _, native = app_v1()
  local p = names(native.frames("identify")[1].msg.payload.capabilities)["game.profile"].parameters
  t.ok(#p.name >= 1 and #p.name <= 64)
  t.ok(p.icon.size >= 1 and p.icon.size <= 131072)
  t.ok(#p.icon.revision <= 128 and not p.icon.revision:find(" "))
  t.ok(json.encode(p))
end)

t.run("profile")
