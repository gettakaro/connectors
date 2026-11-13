using System;
using System.Collections.Generic;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using Eleon.Modding;
using Newtonsoft.Json.Linq;
using TakaroEmpyrion.Core;
using TakaroEmpyrion.Game;

/// <summary>
/// player-connected on a join that coincides with a playfield server start: the dedi leaves Request_Player_Info
/// unanswered for a while (live 2026-11-13, first join after boot). The event must still reach Takaro.
/// </summary>
public static partial class ContractTests
{
    /// <summary>Leaves the first <see cref="Unanswered"/> Request_Player_Info calls without a reply, like a dedi busy starting a playfield server.</summary>
    sealed class SlowJoinGame : ModGameAPI
    {
        public GameBridge Bridge;
        public int Unanswered;
        public int InfoRequests;
        public readonly Dictionary<int, PlayerInfo> Players = new Dictionary<int, PlayerInfo>();
        public void Console_Write(string txt) { }
        public ulong Game_GetTickTime() { return 0; }
        public bool Game_Request(CmdId reqId, ushort seqNr, object data)
        {
            if (reqId != CmdId.Request_Player_Info) return false;
            int n = Interlocked.Increment(ref InfoRequests);
            if (n <= Unanswered) return true;
            int id = ((Id)data).id;
            if (Players.TryGetValue(id, out PlayerInfo pi)) Bridge.OnGameEvent(CmdId.Event_Player_Info, seqNr, pi);
            else Bridge.OnGameEvent(CmdId.Event_Error, seqNr, new ErrorInfo(ErrorType.PlayerIdNotFound));
            return true;
        }
    }

    sealed class JoinRig
    {
        public GameBridge Bridge;
        public SlowJoinGame Game;
        public FakeV2 V2 = new FakeV2();
        public PlayerRegistry Players = new PlayerRegistry(null);
        public Actions Actions;
        public Events Events;
        public ConsoleLog Log = new ConsoleLog();
        public readonly List<KeyValuePair<string, JObject>> Emitted = new List<KeyValuePair<string, JObject>>();
        public readonly List<TimeSpan> Waits = new List<TimeSpan>();
        volatile bool _stop;
        public long MaxTickMs;

        public JoinRig(int unanswered)
        {
            Bridge = new GameBridge(Log) { RequestTimeout = TimeSpan.FromMilliseconds(150) };
            Game = new SlowJoinGame { Bridge = Bridge, Unanswered = unanswered };
            Game.Players[101] = new PlayerInfo { entityId = 101, steamId = "76561198000000001", playerName = "Tester", playfield = "Akua", pos = new PVector3(1, 2, 3), ping = 42 };
            Bridge.Attach(Game);
            var ticker = new Thread(() =>
            {
                var sw = new System.Diagnostics.Stopwatch();
                while (!_stop)
                {
                    sw.Restart(); Bridge.Tick();
                    if (sw.ElapsedMilliseconds > MaxTickMs) MaxTickMs = sw.ElapsedMilliseconds;
                    Thread.Sleep(1);
                }
            }) { IsBackground = true };
            ticker.Start();
            var cfg = new Config { ServerName = "Dev", IdentityToken = "id" };
            Actions = new Actions(Bridge, V2, Players, new Catalog(), null, cfg, Log, new BanReasons(null), _ => { });
            Actions.PlayerInfoRetryBase = TimeSpan.FromMilliseconds(20);
            Actions.PlayerInfoBudget = TimeSpan.FromSeconds(2);
            Actions.Delay = d => { lock (Waits) Waits.Add(d); return Task.Delay(d); };
            Events = new Events(Actions, Players, new Catalog(), Log, (type, data) => { lock (Emitted) Emitted.Add(new KeyValuePair<string, JObject>(type, data)); });
            Bridge.UnsolicitedEvent += Events.OnLegacyEvent;
        }

        public JObject WaitFor(string type, int ms)
        {
            DateTime end = DateTime.UtcNow.AddMilliseconds(ms);
            while (DateTime.UtcNow < end)
            {
                lock (Emitted) { var hit = Emitted.FirstOrDefault(e => e.Key == type); if (hit.Key != null) return hit.Value; }
                Thread.Sleep(20);
            }
            return null;
        }

        public int Count(string type) { lock (Emitted) return Emitted.Count(e => e.Key == type); }
        public bool Logged(string level, string part) { lock (Log.Lines) return Log.Lines.Any(l => l.StartsWith(level) && l.Contains(part)); }
        public void Stop() { _stop = true; }
    }

    static void FirstJoinWhilePlayfieldStarts()
    {
        // 1. Player info unanswered twice (timeouts), answered on the third ask: full player-connected.
        {
            var rig = new JoinRig(2);
            rig.Bridge.OnGameEvent(CmdId.Event_Player_Connected, 0, new Id(101));
            JObject ev = rig.WaitFor("player-connected", 4000);
            JToken p = ev?["player"];
            Check(p != null && (string)p["gameId"] == "76561198000000001" && (string)p["steamId"] == "76561198000000001" && (int)p["ping"] == 42
                  && (string)ev["position"]?["dimension"] == "Akua",
                "first join: player-connected sent after 2 unanswered Request_Player_Info (regression: dropped after one 10 s timeout)", ev?.ToString() ?? "no event");
            Check(rig.Game.InfoRequests >= 3 && rig.Waits.Count == 2 && rig.Waits[0] == TimeSpan.FromMilliseconds(20) && rig.Waits[1] == TimeSpan.FromMilliseconds(40),
                "first join: retries back off (base, 2x base)", "requests " + rig.Game.InfoRequests + " waits " + string.Join(",", rig.Waits));
            Check(rig.Players.BySteam("76561198000000001")?.Online == true, "first join: player marked online");
            Check(rig.MaxTickMs < 50, "first join: the game thread never waits on the retries", "max tick " + rig.MaxTickMs + " ms");
            Thread.Sleep(300);
            Check(rig.Count("player-connected") == 1, "first join: exactly one player-connected");
            rig.Stop();
        }

        // 2. Never answered within the budget, the v2 API knows the player: send id + name, no invented ping/position.
        {
            var rig = new JoinRig(int.MaxValue);
            rig.V2.Data[101] = new PlayerData { EntityId = 101, SteamId = "76561198000000001", PlayerName = "Tester", PlayfieldName = "Akua", IsOnline = true };
            var sw = System.Diagnostics.Stopwatch.StartNew();
            rig.Bridge.OnGameEvent(CmdId.Event_Player_Connected, 0, new Id(101));
            JObject ev = rig.WaitFor("player-connected", 6000);
            JToken p = ev?["player"];
            Check(p != null && (string)p["gameId"] == "76561198000000001" && (string)p["name"] == "Tester" && (string)p["platformId"] == "steam:76561198000000001"
                  && p["ping"] == null && ev["position"] == null,
                "no answer at all: player-connected still sent with the known Steam id and name", ev?.ToString() ?? "no event");
            Check(sw.Elapsed >= TimeSpan.FromSeconds(1.5) && sw.Elapsed < TimeSpan.FromSeconds(5), "no answer: gives up after the budget, not before, not never", sw.Elapsed.ToString());
            Check(rig.Logged("WARN", "sent with the known id and name only"), "no answer: fallback is logged as a warning");
            rig.Stop();
        }

        // 3. Never answered, v2 has nothing, the registry (players.json) knows the entity from an earlier session.
        {
            var rig = new JoinRig(int.MaxValue);
            rig.Actions.PlayerInfoBudget = TimeSpan.FromMilliseconds(500);
            rig.Players.Upsert("76561198000000001", 101, "Tester");
            rig.Bridge.OnGameEvent(CmdId.Event_Player_Connected, 0, new Id(101));
            JObject ev = rig.WaitFor("player-connected", 4000);
            Check((string)ev?["player"]?["gameId"] == "76561198000000001" && (string)ev["player"]["name"] == "Tester",
                "no answer: player-connected from the registry when v2 has no data", ev?.ToString() ?? "no event");
            rig.Stop();
        }

        // 4. Nothing known anywhere: no event with an invented id, an error in the log.
        {
            var rig = new JoinRig(int.MaxValue);
            rig.Actions.PlayerInfoBudget = TimeSpan.FromMilliseconds(500);
            rig.Bridge.OnGameEvent(CmdId.Event_Player_Connected, 0, new Id(555));
            Thread.Sleep(1500);
            Check(rig.Count("player-connected") == 0 && rig.Logged("ERROR", "no player id known"), "unknown entity: no player-connected with an invented id, error logged");
            rig.Stop();
        }

        // 5. The player leaves while the connect is still being prepared: neither event.
        {
            var rig = new JoinRig(int.MaxValue);
            rig.Actions.PlayerInfoBudget = TimeSpan.FromMilliseconds(800);
            rig.Players.Upsert("76561198000000001", 101, "Tester");
            rig.Bridge.OnGameEvent(CmdId.Event_Player_Connected, 0, new Id(101));
            Thread.Sleep(200);
            rig.Bridge.OnGameEvent(CmdId.Event_Player_Disconnected, 0, new Id(101));
            Thread.Sleep(1800);
            Check(rig.Count("player-connected") == 0 && rig.Count("player-disconnected") == 0, "left during the join: no connect and no unpaired disconnect");
            rig.Stop();
        }

        // 6. A quick leave and rejoin while the first connect is pending: one player-connected, from the rejoin.
        {
            var rig = new JoinRig(3);
            rig.Bridge.OnGameEvent(CmdId.Event_Player_Connected, 0, new Id(101));
            Thread.Sleep(100);
            rig.Bridge.OnGameEvent(CmdId.Event_Player_Disconnected, 0, new Id(101));
            rig.Bridge.OnGameEvent(CmdId.Event_Player_Connected, 0, new Id(101));
            rig.WaitFor("player-connected", 4000);
            Thread.Sleep(800);
            Check(rig.Count("player-connected") == 1 && rig.Count("player-disconnected") == 0, "leave + rejoin during the join: exactly one player-connected",
                "connected " + rig.Count("player-connected") + " disconnected " + rig.Count("player-disconnected"));
            rig.Stop();
        }
    }
}
