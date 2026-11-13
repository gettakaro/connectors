using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using Eleon;
using Eleon.Modding;
using Newtonsoft.Json.Linq;
using TakaroEmpyrion.Core;
using TakaroEmpyrion.Game;
using TakaroEmpyrion.Protocol;

/// <summary>
/// Contract tests: a fake Takaro (in-memory socket) and a fake Empyrion (ModGameAPI + IGameV2) drive the real
/// protocol client, bridge and adapters. Run: mono contract-tests.exe &lt;capabilities.json&gt;
/// </summary>
public static partial class ContractTests
{
    static int _pass, _fail;

    static void Check(bool ok, string name, string detail = "")
    {
        if (ok) { _pass++; Console.WriteLine("PASS " + name); }
        else { _fail++; Console.WriteLine("FAIL " + name + (detail.Length > 0 ? " :: " + detail : "")); }
    }

    public static int Main(string[] argv)
    {
        string caps = argv.Length > 0 ? argv[0] : "capabilities.json";
        Run("args shapes", ArgsShapes);
        Run("ban duration", BanDuration);
        Run("ecf + csv + catalogue", CatalogParsing);
        Run("coverage drift", () => CoverageDrift(caps));
        Run("wire", Wire);
        Run("map files + frame", MapFrame);
        Run("map tiles", MapTiles);
        Run("map real data", MapRealData);
        Run("wire v1", WireV1);
        Run("reconnect delays", ReconnectDelays);
        Run("reconnect wire", ReconnectWire);
        Run("reconnect slow uplink", ReconnectSlowUplink);
        Run("first join while a playfield starts", FirstJoinWhilePlayfieldStarts);
        Console.WriteLine("\n" + _pass + " passed, " + _fail + " failed");
        Console.Out.Flush();
        // Background protocol threads are still alive; exit hard so Mono's teardown cannot mask the result.
        Environment.Exit(_fail == 0 ? 0 : 1);
        return 0;
    }

    static void Run(string name, Action t)
    {
        try { t(); }
        catch (Exception e) { Check(false, name + " threw", e.ToString()); }
    }

    // ------------------------------------------------------------------ args

    static void ArgsShapes()
    {
        Check(Args.Parse(new JValue("{}")).PlayerGameId() == null, "args '{}' string");
        Check(Args.Parse(new JArray()).Raw.Count == 0, "args [] array");
        Check(Args.Parse(JValue.CreateNull()).Raw.Count == 0, "args null");
        Check(Args.Parse(new JValue("")).Raw.Count == 0, "args empty string");
        Check(Args.Parse(new JValue("{\"gameId\":\"76561198000000001\"}")).PlayerGameId() == "76561198000000001", "flat gameId in JSON string");
        Check(Args.Parse(JObject.Parse("{\"player\":{\"gameId\":\"765\"}}")).PlayerGameId() == "765", "nested player.gameId");
        Check(Args.Parse(JObject.Parse("{\"player\":{\"id\":\"pog\",\"player\":{\"gameId\":\"999\"}}}")).PlayerGameId() == "999", "PlayerOnGameserver row");
        Check(Args.Parse(JObject.Parse("{\"gameId\":12345}")).PlayerGameId() == "12345", "numeric gameId");
        var withNull = Args.Parse(JObject.Parse("{\"dimension\":null,\"x\":\"1.5\",\"y\":2,\"z\":null}"));
        Check(withNull.Str("dimension") == null && withNull.Num("x") == 1.5 && withNull.Num("y") == 2 && withNull.Num("z") == null, "explicit null + numeric strings");
        // Live banPlayer frame, 2026-11-03: expiresAt must survive as a parseable timestamp, not vanish as a Date token.
        var ban = Args.Parse(new JValue("{\"player\":{\"gameId\":\"76561198000735875\"},\"reason\":\"takaro-test timed ban\",\"expiresAt\":\"2026-11-04T09:00:00Z\"}"));
        Check(ban.Str("expiresAt") == "2026-11-04T09:00:00Z", "banPlayer expiresAt kept as string (regression: became permanent)");
        Check(Args.Parse(JObject.Parse("{\"expiresAt\":\"2026-11-04T09:00:00Z\"}")).Str("expiresAt") == "2026-11-04T09:00:00.000Z", "expiresAt from a pre-parsed Date token");
        bool threw = false;
        try { Args.Parse(new JValue("{not json")); } catch (ArgsException) { threw = true; }
        Check(threw, "malformed JSON string -> ArgsException");
    }

    static void BanDuration()
    {
        DateTime now = new DateTime(2026, 1, 1, 0, 0, 0, DateTimeKind.Utc);
        Check(Actions.BanDuration(null, now) == "3650d", "permanent ban -> 3650d");
        string plys = "Players connected (1)\n C-Id:  E-Id, Player Name,    Playfield Name, IP address\n----\n    1:  1040, Tester,          Akua,           203.0.113.15|51576\n\nGlobal online players list:\n  1. id=1040 name=Tester";
        var ips = Actions.ParsePlysIps(plys);
        Check(ips.Count == 1 && ips[1040] == "203.0.113.15", "ip parsed from the console's plys (live format)");
        Check(Actions.BanDuration(now.AddDays(14), now) == "14d", "14 days -> 14d");
        Check(Actions.BanDuration(now.AddHours(5).AddMinutes(1), now) == "5h", "5h01 rounds to 5h (nearest hour)");
        Check(Actions.BanDuration(now.AddHours(24).AddSeconds(25), now) == "1d", "24h00m25s -> 1d (live case: ceil made it 25h)");
        Check(Actions.BanDuration(now.AddMinutes(-5), now) == "1h", "expiry in the past -> 1h minimum");
        // Identity: Steam vs Epic (the game gives Epic players a numeric hash id outside the SteamID64 range).
        JObject steamId = PlayerRegistry.Identity("76561198000735875", "Tester");
        Check((string)steamId["steamId"] == "76561198000735875" && (string)steamId["platformId"] == "steam:76561198000735875", "Steam player -> steamId + steam: platformId");
        JObject epicId = PlayerRegistry.Identity("8969801958249005365", "EpicGuy");
        Check(epicId["steamId"] == null && epicId["epicOnlineServicesId"] == null && (string)epicId["platformId"] == "epic:8969801958249005365"
              && (string)epicId["gameId"] == "8969801958249005365", "Epic player -> epic: platformId, never steamId/EOS id");
        Check(!PlayerRegistry.IsSteamId("10000000000000000") && PlayerRegistry.IsPlayerId("10000000000000000"), "17-digit Epic id is not mistaken for Steam (no length check)");
        Check(PlayerRegistry.IsSteamId("76561197960265729") && !PlayerRegistry.IsSteamId("76561202255233024"), "SteamID64 detection by universe/type bits");
        Check(!PlayerRegistry.IsPlayerId("Tester") && !PlayerRegistry.IsPlayerId("-5") && !PlayerRegistry.IsPlayerId(""), "non-numeric is not a player id");
    }

    // ------------------------------------------------------------------ catalogue

    static void CatalogParsing()
    {
        string ecf = "# comment\n{ Item Id: 1, Name: MeleePlayer\n  Class: Melee\n}\n{ +Item Id: 3, Name: Pistol\n  Category: Weapons\n  { Child 0\n    Class: Ranged\n  }\n}\n";
        List<EcfBlock> blocks = Ecf.Parse(ecf).ToList();
        Check(blocks.Count == 2 && blocks[1].Name == "Pistol" && blocks[1].Kind == "Item", "ecf top-level blocks with nested child");
        Check(blocks[1].Props.ContainsKey("Category") && !blocks[1].Props.ContainsKey("Class"), "child props do not leak into parent");

        var rows = Csv.Rows("KEY,English\nPistol,Projectile Pistol\n\"Quoted\",\"A, b \"\"c\"\"\"\n").ToList();
        Check(rows.Count == 3 && rows[2][1] == "A, b \"c\"", "csv quoting");

        var c = new Catalog();
        c.LoadMapping(new Dictionary<string, int> { { "Pistol", 4099 }, { "IronOre", 4300 }, { "HullBlocks", 380 } });
        c.LoadLocalization("KEY,English\nPistol,Projectile Pistol\nIronOre,[b]Iron[/b] Ore\n");
        Check(c.DisplayName("Pistol") == "Projectile Pistol", "localized display name");
        Check(c.DisplayName("IronOre") == "Iron Ore", "markup stripped");
        Check(c.DisplayName("HullBlocks") == "Hull Blocks", "humanized fallback");
        Check(c.TryId("Projectile Pistol", out int id, out string code) && id == 4099 && code == "Pistol", "giveItem by display name");
        Check(c.TryId("4300", out id, out code) && code == "IronOre", "giveItem by numeric id");
        Check(!c.TryId("NoSuchThing", out id, out code), "unknown item rejected");
        JArray items = c.ItemsJson();
        var names = items.ToDictionary(i => (string)i["code"], i => (string)i["name"]);
        Check(items.Count == 3 && names["Pistol"] == "Projectile Pistol" && names["IronOre"] == "Iron Ore" && names["HullBlocks"] == "Hull Blocks", "items carry human names, never class names");

        string tmp = Path.Combine(Path.GetTempPath(), "takaro-emp-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(Path.Combine(tmp, "Configuration"));
        File.WriteAllText(Path.Combine(tmp, "Configuration", "ItemsConfig.ecf"), "{ Item Id: 2, Name: Flashlight\n}\n{ +Item Id: 3, Name: Pistol\n}\n{ +Item Id: 708, Name: PDA\n}\n");
        File.WriteAllText(Path.Combine(tmp, "Configuration", "BlocksConfig.ecf"), "{ Block Id: 380, Name: HullBlocks\n}\n{ +Block Id: 381, Name: HullWedge, Ref: HullBlocks\n}\n");
        var fb = new Catalog();
        int n = fb.LoadMappingFromEcf(new[] { tmp });
        // Live starter inventory on v1.19.6: 8194 Flashlight, 8900 PDA.
        Check(n == 5 && fb.TryId("Flashlight", out id, out code) && id == 8194 && fb.TryId("PDA", out id, out code) && id == 8900
              && fb.TryId("HullWedge", out id, out code) && id == 381, "ECF fallback: item id = 8192 + config id (measured), blocks as-is");
        Check(fb.CodeOf(8194) == "Flashlight", "inventory ids map back to codes");
        Check(fb.Mismatches(new Dictionary<string, int> { { "Pistol", 8195 }, { "HullBlocks", 999 } }).Count == 1, "fallback ids validated against the runtime mapping");
        File.WriteAllText(Path.Combine(tmp, "Configuration", "BlocksConfig.ecf"), "{ Block Id: 0, Name: Air\n}\n{ Block Id: 380, Name: HullBlocks\n}\n{ Block Id: 382, Name: DebugBlock\n  ShowUser: No\n}\n");
        File.WriteAllText(Path.Combine(tmp, "Configuration", "EClassConfig.ecf"), "{ Entity Name: CV_Alien\n  Faction: Zirax\n}\n{ Entity Name: Turret1\n  Faction: Zirax\n  Class: TurretAuto\n}\n{ Entity Name: ZiraxMinigun\n  Faction: Zirax\n}\n");
        var vis = new Catalog();
        vis.LoadMappingFromEcf(new[] { tmp });
        vis.LoadFiles(new[] { tmp });
        var visCodes = vis.ItemsJson().Select(i => (string)i["code"]).ToList();
        Check(!visCodes.Contains("Air") && !visCodes.Contains("DebugBlock") && visCodes.Contains("HullBlocks") && visCodes.Contains("Pistol"), "hidden (ShowUser: No) and Air excluded from listItems");
        Check(vis.EntitiesJson().Count == 1 && (string)vis.EntitiesJson()[0]["code"] == "ZiraxMinigun", "structures and turrets excluded from listEntities");
        Directory.Delete(tmp, true);

        c.LoadEntities("{ Entity Name: PlyMale\n  Faction: Player\n}\n{ Entity Name: ZiraxMale\n  Faction: Zirax\n}\n{ Entity Name: Rabbit\n  Faction: Prey\n  IsEnemy: false\n}\n");
        JArray ents = c.EntitiesJson();
        Check(ents.Count == 2 && (string)ents[0]["type"] == "hostile" && (string)ents[1]["type"] == "friendly", "entities: player classes excluded, hostility mapped");
        c.LoadEntities("{ Entity Name: Parasaur02\n  Faction: Prey\n  IsEnemy: false\n}\n{ +Entity Name: ParasaurBaby, Ref: Parasaur02\n  MaxHealth: 300\n}\n{ +Entity Name: LoopA, Ref: LoopB\n}\n{ +Entity Name: LoopB, Ref: LoopA\n}\n");
        var refCodes = c.EntitiesJson().Select(e => (string)e["code"]).ToList();
        Check(refCodes.Contains("ParasaurBaby") && refCodes.Contains("Parasaur02") && !refCodes.Contains("LoopA"),
              "entities inherit Faction through Ref: (live: ParasaurBaby was missing from listEntities), Ref cycles end");
        c.LoadEntities("{ Entity Name: CV_ClusterDrone\n  Faction: Zirax\n}\n{ Entity Name: Freighter_ClassI, Ref: CV_ClusterDrone\n  ModelType: ChunkClusterCV\n}\n"
            + "{ Entity Name: ZiraxTest\n  Faction: Zirax\n}\n{ +Entity Name: ZiraxTestNetwork, Ref: ZiraxTest\n}\n{ Entity Name: Zirax\n  Faction: Zirax\n}\n{ +Entity Name: ZiraxMinigunGroup, Ref: Zirax\n}\n");
        var lin = c.EntitiesJson().Select(e => (string)e["code"]).ToList();
        Check(lin.SequenceEqual(new[] { "Zirax", "ZiraxMinigunGroup" }), "ships and test rigs excluded through their Ref lineage (live: Freighter_ClassI, ZiraxTestNetwork)", string.Join(",", lin));
    }

    static void CoverageDrift(string capsPath)
    {
        JObject caps = JObject.Parse(File.ReadAllText(capsPath));
        var fromJson = new Dictionary<string, string>();
        foreach (JProperty p in ((JObject)caps["actions"]).Properties()) fromJson[p.Name] = (string)p.Value["status"];
        foreach (JProperty p in ((JObject)caps["events"]).Properties()) fromJson[p.Name] = (string)p.Value["status"];
        var fromCode = Coverage.All.ToDictionary(e => e.Name, e => e.Status);
        bool same = fromJson.Count == fromCode.Count && fromCode.All(kv => fromJson.TryGetValue(kv.Key, out string s) && s == kv.Value);
        Check(same, "capabilities.json == Coverage registry",
            string.Join(", ", fromCode.Where(kv => !fromJson.TryGetValue(kv.Key, out string s) || s != kv.Value).Select(kv => kv.Key)));
        Check(Actions.Supported.All(a => fromCode.ContainsKey(a)), "every handled action has a coverage entry");
        Check(Coverage.All.Count(e => e.Kind == "action") == 19 && Coverage.All.Count(e => e.Kind == "event") == 6, "19 actions + 6 events classified");
    }

    // ------------------------------------------------------------------ wire (fake Takaro + fake game)

    sealed class FakeSocket : IWsConnection
    {
        public readonly BlockingCollection<string> ToConnector = new BlockingCollection<string>();
        public readonly BlockingCollection<string> FromConnector = new BlockingCollection<string>();
        public volatile bool Aborted;
        /// <summary>Answer pings like Takaro does, until Silent (a half-open link: frames go out, nothing comes back).</summary>
        public volatile bool AutoPong, Silent;
        /// <summary>Each Send blocks this long (a slow uplink).</summary>
        public volatile int SendDelayMs;
        public void Connect(Uri uri, TimeSpan timeout) { }
        public void Send(string text)
        {
            if (SendDelayMs > 0) Thread.Sleep(SendDelayMs);
            if (Aborted) throw new IOException("closed");
            FromConnector.Add(text);
            if (AutoPong && !Silent && text == "{\"type\":\"ping\"}") ToConnector.Add("{\"type\":\"pong\"}");
        }
        public string Receive()
        {
            while (!Aborted) { if (ToConnector.TryTake(out string s, 50) && !Silent) return s == "<close>" ? null : s; }
            return null;
        }
        public void Abort() { Aborted = true; }
        public void Dispose() { }
    }

    sealed class FakeGame : ModGameAPI
    {
        public GameBridge Bridge;
        public readonly List<KeyValuePair<CmdId, object>> Requests = new List<KeyValuePair<CmdId, object>>();
        public readonly Dictionary<int, PlayerInfo> Players = new Dictionary<int, PlayerInfo>();
        public Inventory Inv;
        public void Console_Write(string txt) { }
        public ulong Game_GetTickTime() { return 0; }
        public bool Game_Request(CmdId reqId, ushort seqNr, object data)
        {
            lock (Requests) Requests.Add(new KeyValuePair<CmdId, object>(reqId, data));
            switch (reqId)
            {
                case CmdId.Request_Dedi_Stats: Bridge.OnGameEvent(CmdId.Event_Dedi_Stats, seqNr, new DediStats()); break;
                case CmdId.Request_Player_List: Bridge.OnGameEvent(CmdId.Event_Player_List, seqNr, new IdList { list = Players.Keys.ToList() }); break;
                case CmdId.Request_Player_Info:
                    int id = ((Id)data).id;
                    if (Players.TryGetValue(id, out PlayerInfo pi)) Bridge.OnGameEvent(CmdId.Event_Player_Info, seqNr, pi);
                    else Bridge.OnGameEvent(CmdId.Event_Error, seqNr, new ErrorInfo(ErrorType.PlayerIdNotFound));
                    break;
                case CmdId.Request_Player_GetInventory: Bridge.OnGameEvent(CmdId.Event_Player_Inventory, seqNr, Inv); break;
                case CmdId.Request_Player_AddItem:
                case CmdId.Request_Entity_Teleport:
                case CmdId.Request_Player_ChangePlayerfield:
                case CmdId.Request_ConsoleCommand:
                    Bridge.OnGameEvent(CmdId.Event_Ok, seqNr, null); break;
                case CmdId.Request_GetBannedPlayers:
                    var b = new BannedPlayerData();
                    b.BannedPlayers.Add(new BannedPlayerData.BanEntry(76561198000000002UL, new DateTime(2030, 1, 1, 0, 0, 0, DateTimeKind.Utc)));
                    Bridge.OnGameEvent(CmdId.Event_BannedPlayers, seqNr, b); break;
                default: return false;
            }
            return true;
        }
    }

    sealed class FakeV2 : IGameV2
    {
        public readonly List<MessageData> Sent = new List<MessageData>();
        public void SendChatMessage(MessageData msg) { lock (Sent) Sent.Add(msg); }
        public Dictionary<string, int> GetBlockAndItemMapping() { return new Dictionary<string, int> { { "Pistol", 4099 }, { "IronOre", 4300 } }; }
        public IEnumerable<string> ContentDirs() { return new string[0]; }
        public string SaveGameDir() { return null; }
        public readonly Dictionary<int, PlayerData> Data = new Dictionary<int, PlayerData>();
        public PlayerData? GetPlayerDataFor(int entityId) { return Data.TryGetValue(entityId, out PlayerData d) ? d : (PlayerData?)null; }
    }

    sealed class ConsoleLog : ILog
    {
        public readonly List<string> Lines = new List<string>();
        public void Debug(string s) { }
        public void Info(string s) { lock (Lines) Lines.Add("INFO " + s); }
        public void Warn(string s) { lock (Lines) Lines.Add("WARN " + s); }
        public void Error(string s) { lock (Lines) Lines.Add("ERROR " + s); }
    }

    static JObject Next(FakeSocket s, Func<JObject, bool> match, int ms = 4000)
    {
        DateTime end = DateTime.UtcNow.AddMilliseconds(ms);
        while (DateTime.UtcNow < end)
        {
            if (!s.FromConnector.TryTake(out string f, 50)) continue;
            JObject o = JObject.Parse(f);
            if (match(o)) return o;
        }
        return null;
    }

    static void Wire()
    {
        var log = new ConsoleLog();
        var bridge = new GameBridge(log);
        var game = new FakeGame { Bridge = bridge };
        game.Players[101] = new PlayerInfo { entityId = 101, steamId = "76561198000000001", playerName = "Tester", playfield = "Akua", pos = new PVector3(1, 2, 3), ping = 42 };
        game.Inv = new Inventory(101, new[] { new ItemStack(4099, 1) }, new[] { new ItemStack(4300, 250), new ItemStack(0, 0) });
        bridge.Attach(game);
        var ticker = new Thread(() => { while (true) { bridge.Tick(); Thread.Sleep(2); } }) { IsBackground = true };
        ticker.Start();

        var v2 = new FakeV2();
        var players = new PlayerRegistry(null);
        var cfg = new Config { ServerName = "Dev", IdentityToken = "id", EnableDestructiveCommands = false };
        var actions = new Actions(bridge, v2, players, new Catalog(), null, cfg, log, new BanReasons(null), _ => { });

        var sockets = new BlockingCollection<FakeSocket>();
        TakaroClient client = null;
        client = new TakaroClient(() => { var s = new FakeSocket(); sockets.Add(s); return s; },
            new ClientOptions { Url = "wss://fake/", IdentityToken = "id", RegistrationToken = "reg", ServerName = "Dev", PingInterval = TimeSpan.FromMilliseconds(200), InboundTimeout = TimeSpan.FromSeconds(30) },
            log,
            req => Task.Run(async () =>
            {
                try { client.SendResponse(req.RequestId, await actions.Run(req.Action, Args.Parse(req.RawArgs))); }
                catch (Exception e) { client.SendError(req.RequestId, (e is AggregateException ae ? ae.InnerException : e).Message); }
            }));
        client.Start();

        FakeSocket sock = sockets.Take();
        JObject ident = Next(sock, o => (string)o["type"] == "identify");
        Check(ident != null && (string)ident["payload"]["registrationToken"] == "reg" && (string)ident["payload"]["identityToken"] == "id", "CONN identify carries both tokens");

        client.SendGameEvent("log", new JObject { ["msg"] = "queued before identify" });
        sock.ToConnector.Add("{\"type\":\"connected\",\"payload\":{}}");
        sock.ToConnector.Add("{\"type\":\"identifyResponse\",\"payload\":{\"gameServerId\":\"gs1\"}}");
        Check(Next(sock, o => (string)o["type"] == "gameEvent") != null, "events raised while unidentified are flushed after identify");
        Check(Next(sock, o => (string)o["type"] == "ping", 2000) != null, "WIRE-1 heartbeat ping sent");

        Func<string, JToken, JObject> ask = (action, args) =>
        {
            string rid = Guid.NewGuid().ToString();
            sock.ToConnector.Add(new JObject { ["type"] = "request", ["requestId"] = rid, ["payload"] = new JObject { ["action"] = action, ["args"] = args } }.ToString());
            return Next(sock, o => (string)o["requestId"] == rid && (string)o["type"] != "ping");
        };

        JObject r = ask("testReachability", "{}");
        Check(r != null && (string)r["type"] == "response" && (bool)r["payload"]["connectable"], "testReachability round-trips through the game");

        r = ask("getPlayers", "{}");
        JToken p0 = r?["payload"]?[0];
        Check(r?["payload"] is JArray && p0 != null && (string)p0["gameId"] == "76561198000000001" && (string)p0["platformId"] == "steam:76561198000000001"
              && (string)p0["steamId"] == "76561198000000001" && (string)p0["name"] == "Tester" && (int)p0["ping"] == 42, "getPlayers: bare array, SteamID64 identity fields");

        // WIRE-2 correlation: two in flight, each answered by its own requestId
        string a1 = Guid.NewGuid().ToString(), a2 = Guid.NewGuid().ToString();
        sock.ToConnector.Add(new JObject { ["type"] = "request", ["requestId"] = a1, ["payload"] = new JObject { ["action"] = "getPlayerLocation", ["args"] = "{\"gameId\":\"76561198000000001\"}" } }.ToString());
        sock.ToConnector.Add(new JObject { ["type"] = "request", ["requestId"] = a2, ["payload"] = new JObject { ["action"] = "listBans", ["args"] = "{}" } }.ToString());
        var got = new Dictionary<string, JObject>();
        for (int i = 0; i < 2; i++) { JObject o = Next(sock, x => (string)x["type"] == "response"); if (o != null) got[(string)o["requestId"]] = o; }
        Check(got.ContainsKey(a1) && (double)got[a1]["payload"]["x"] == 1 && (string)got[a1]["payload"]["dimension"] == "Akua", "WIRE-2 location answered by its requestId");
        Check(got.ContainsKey(a2) && got[a2]["payload"] is JArray && (string)got[a2]["payload"][0]["player"]["gameId"] == "76561198000000002", "WIRE-2 listBans answered by its requestId, bare array");

        // WIRE-3 args shapes
        Check((string)ask("getPlayer", new JObject { ["gameId"] = "76561198000000001" })?["payload"]?["name"] == "Tester", "WIRE-3 object args");
        Check((string)ask("getPlayer", "{\"player\":{\"gameId\":\"76561198000000001\"}}")?["payload"]?["name"] == "Tester", "WIRE-4 nested player");
        Check(ask("getPlayer", "{\"gameId\":\"76561190000000000\"}")?["payload"]?.Type == JTokenType.Null, "getPlayer unknown -> null payload");
        JToken li = ask("listItems", new JArray())?["payload"];
        Check(li is JArray && li.Count() == 2 && li.Any(i => (string)i["code"] == "IronOre" && (string)i["name"] == "Iron Ore"), "WIRE-3 [] args accepted, catalogue from the game mapping");

        r = ask("getPlayerInventory", "{\"gameId\":\"76561198000000001\"}");
        Check(r?["payload"] is JArray inv && inv.Count == 2 && (string)inv[1]["code"] == "IronOre" && (int)inv[1]["amount"] == 250, "inventory: toolbelt + bag, empty slots skipped");

        r = ask("giveItem", "{\"player\":{\"gameId\":\"76561198000000001\"},\"item\":\"IronOre\",\"amount\":5,\"quality\":null}");
        IdItemStack given = null;
        lock (game.Requests) given = game.Requests.Where(k => k.Key == CmdId.Request_Player_AddItem).Select(k => (IdItemStack)k.Value).LastOrDefault();
        Check(r != null && (string)r["type"] == "response" && given != null && given.id == 101 && given.itemStack.id == 4300 && given.itemStack.count == 5, "giveItem resolves code + entity, explicit null quality");
        r = ask("giveItem", "{\"gameId\":\"76561198000000001\",\"item\":\"Nope\",\"amount\":1}");
        Check(r != null && (string)r["type"] == "error", "giveItem unknown item is an error, not a success");

        r = ask("teleportPlayer", "{\"player\":{\"gameId\":\"76561198000000001\"},\"x\":10,\"y\":20,\"z\":30,\"dimension\":null}");
        IdPositionRotation tp;
        lock (game.Requests) tp = game.Requests.Where(k => k.Key == CmdId.Request_Entity_Teleport).Select(k => (IdPositionRotation)k.Value).LastOrDefault();
        Check(r != null && (string)r["type"] == "response" && tp != null && tp.pos.x == 10 && tp.pos.z == 30, "teleport same playfield, dimension null");
        ask("teleportPlayer", "{\"gameId\":\"76561198000000001\",\"x\":1,\"y\":2,\"z\":3,\"dimension\":\"Omicron\"}");
        bool changed;
        lock (game.Requests) changed = game.Requests.Any(k => k.Key == CmdId.Request_Player_ChangePlayerfield && ((IdPlayfieldPositionRotation)k.Value).playfield == "Omicron");
        Check(changed, "teleport to another dimension changes playfield");

        ask("sendMessage", "{\"message\":\"hello all\",\"opts\":{}}");
        ask("sendMessage", "{\"message\":\"psst\",\"opts\":{\"recipient\":{\"gameId\":\"76561198000000001\"},\"senderNameOverride\":\"Bot\"}}");
        MessageData g, w;
        lock (v2.Sent) { g = v2.Sent.FirstOrDefault(m => m.Text == "hello all"); w = v2.Sent.FirstOrDefault(m => m.Text == "psst"); }
        Check(g != null && g.Channel == Eleon.MsgChannel.Global && g.SenderNameOverride == "Dev", "sendMessage global with configured sender");
        Check(w != null && w.Channel == Eleon.MsgChannel.SinglePlayer && w.RecipientEntityId == 101 && w.SenderNameOverride == "Bot", "ACT-13 directed stays directed, sender override");
        Check(ServerEcho.IsOurs("psst"), "own messages are remembered for the echo filter");

        r = ask("executeConsoleCommand", "{\"command\":\"saveandexit 0\"}");
        Check(r != null && (bool)r["payload"]["success"] == false && ((string)r["payload"]["errorMessage"]).StartsWith("command_not_allowed"), "console allowlist refuses");
        r = ask("shutdown", "{}");
        Check(r != null && (string)r["type"] == "error" && ((string)r["payload"]["error"]).Contains("ENABLE_DESTRUCTIVE_COMMANDS"), "shutdown gate refuses by default");

        r = ask("getMapInfo", "{}");
        Check(r != null && (string)r["type"] == "error", "WIRE-6 unknown action answered, not dropped");

        // shutdown: saveandexit refused while a playfield server starts is retried, not silently dropped
        {
            var replies = new Queue<string>(new[] { "Cannot exit now because a new playfield server is currently starting. Please retry in 30 seconds.", "" });
            int waits = 0;
            string final = Actions.RunUntilAccepted(() => Task.FromResult(replies.Dequeue()), _ => { waits++; return Task.CompletedTask; }, log, 5, 0).Result;
            Check(final == "" && waits == 1 && replies.Count == 0, "shutdown retries saveandexit after a 'Cannot exit now' refusal");
            int calls = 0;
            Actions.RunUntilAccepted(() => { calls++; return Task.FromResult("Cannot exit now, retry in 30 seconds"); }, _ => Task.CompletedTask, log, 3, 0).Wait();
            Check(calls == 3, "shutdown retry is bounded");
        }

        // player-connected carries ip (console plys) and ping, never the IP remembered from an earlier session
        int plysReads = 0;
        actions.PlysReader = () => ++plysReads < 2
            ? "Players connected (0)\n"
            : "Players connected (1)\n C-Id:  E-Id, Player Name,    Playfield Name, IP address\n----\n    1:  101, Tester,          Akua,           10.1.2.3|51576\n";
        players.BySteam("76561198000000001").Ip = "192.0.2.99";
        var events = new Events(actions, players, new Catalog(), log, (type, data) => client.SendGameEvent(type, data));
        bridge.UnsolicitedEvent += events.OnLegacyEvent;
        bridge.OnGameEvent(CmdId.Event_Player_Connected, 0, new Id(101));
        JObject conn = Next(sock, o => (string)o["type"] == "gameEvent" && (string)o["payload"]["type"] == "player-connected", 6000);
        JToken cp = conn?["payload"]?["data"]?["player"];
        Check(cp != null && (string)cp["ip"] == "10.1.2.3" && (int)cp["ping"] == 42 && (string)cp["gameId"] == "76561198000000001" && plysReads == 2,
            "player-connected carries fresh ip (plys retried) and ping", conn?.ToString() ?? "no frame");
        bridge.UnsolicitedEvent -= events.OnLegacyEvent;
        actions.PlysReader = null;

        // entity-killed from the playfield agent's packet (shape captured live: KilledEntity on Akua, ParasaurBaby, pistol)
        {
            var kc = new Catalog();
            kc.LoadEntities("{ Entity Name: ParasaurBaby\n  Faction: Prey\n  IsEnemy: false\n}\n{ Entity Name: PlyMale\n  Faction: Player\n}\n");
            kc.LoadMapping(new Dictionary<string, int> { { "Pistol", 8195 } });
            var kev = new Events(actions, players, kc, log, (type, data) => client.SendGameEvent(type, data));
            string pkt = "{\"kind\":\"KilledEntity\",\"playfield\":\"Akua\",\"args\":[\"ActivityProvider\",\"DriveType\",\"44\",null,null],"
                + "\"victimStrings\":[\"\",\"Akua\",\"ParasaurBaby\"],\"heldItem\":8195,\"playerEntityId\":101,\"steamId\":\"76561198000000001\"}";
            kev.OnPlayfieldPacket("TakaroEmpyrion", "Akua", System.Text.Encoding.UTF8.GetBytes(pkt));
            JObject kill = Next(sock, o => (string)o["type"] == "gameEvent" && (string)o["payload"]["type"] == "entity-killed", 6000);
            JToken kd = kill?["payload"]?["data"];
            Check(kd != null && (string)kd["entity"] == "Parasaur Baby" && (string)kd["weapon"] == "Pistol" && (string)kd["player"]["gameId"] == "76561198000000001",
                "entity-killed: creature from the victim object, weapon from the held item, killer by entity id", kill?.ToString() ?? "no frame");
            string noHeld = pkt.Replace(",\"heldItem\":8195", "");
            kev.OnPlayfieldPacket("TakaroEmpyrion", "Akua", System.Text.Encoding.UTF8.GetBytes(noHeld));
            JObject kill2 = Next(sock, o => (string)o["type"] == "gameEvent" && (string)o["payload"]["type"] == "entity-killed", 6000);
            Check((string)kill2?["payload"]?["data"]?["weapon"] == "unknown", "entity-killed: weapon 'unknown' when the held item is not attributable");
            string noPlayer = pkt.Replace(",\"playerEntityId\":101", "");
            kev.OnPlayfieldPacket("TakaroEmpyrion", "Akua", System.Text.Encoding.UTF8.GetBytes(noPlayer));
            Check(Next(sock, o => (string)o["type"] == "gameEvent" && (string)o["payload"]["type"] == "entity-killed", 1500) == null, "creature killed by a non-player: no entity-killed");
        }

        // WIRE-5 reconnect: the socket dies, a new one identifies again, no duplicate senders
        sock.ToConnector.Add("<close>");
        FakeSocket sock2;
        Check(sockets.TryTake(out sock2, 8000), "WIRE-5 reconnects after close");
        if (sock2 != null)
        {
            Check(Next(sock2, o => (string)o["type"] == "identify") != null, "WIRE-5 re-identifies on the new socket");
            sock2.ToConnector.Add("{\"type\":\"identifyResponse\",\"payload\":{\"gameServerId\":\"gs1\"}}");
            client.SendGameEvent("log", new JObject { ["msg"] = "after reconnect" });
            Check(Next(sock2, o => (string)o["type"] == "gameEvent" && (string)o["payload"]["data"]["msg"] == "after reconnect") != null, "events flow on the new socket");
        }

        // seq correlation under timeout: a request the game never answers fails instead of hanging
        var silent = new GameBridge(log);
        silent.Attach(new SilentGame());
        Task<object> t = silent.Request(CmdId.Request_Player_Info, new Id(1));
        silent.Tick();
        Check(!t.Wait(200), "unanswered request stays pending");

        client.Stop();
    }

    sealed class SilentGame : ModGameAPI
    {
        public void Console_Write(string txt) { }
        public ulong Game_GetTickTime() { return 0; }
        public bool Game_Request(CmdId reqId, ushort seqNr, object data) { return true; }
    }
}
