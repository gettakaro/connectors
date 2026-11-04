using System;
using System.Collections.Generic;
using System.IO;
using System.Threading.Tasks;
using Eleon;
using Eleon.Modding;
using Newtonsoft.Json.Linq;
using TakaroEmpyrion.Core;
using TakaroEmpyrion.Game;
using TakaroEmpyrion.Map;
using TakaroEmpyrion.Protocol;

namespace TakaroEmpyrion
{
    /// <summary>
    /// Entry point loaded by Empyrion's mod loader on the dedicated server (ModTargets: Dedi). Implements both
    /// the v2 IMod (chat send/receive, item mapping, paths) and the legacy ModInterface (players, inventory,
    /// teleport, bans, console, events), as Eleon's own DemoModDedi does. Holds the Takaro WebSocket itself.
    /// </summary>
    public sealed class TakaroMod : IMod, ModInterface
    {
        public const string Version = "0.1.0"; // x-release-please-version

        IModApi _api;
        FileLog _log;
        Config _cfg;
        GameBridge _game;
        TakaroClient _client;
        Actions _actions;
        Events _events;
        PlayerRegistry _players;
        bool _started;
        PlayfieldAgent _agent;
        MapService _map;
        System.Threading.Timer _mapTimer;

        // ------------------------------------------------------------ IMod (v2)

        public void Init(IModApi modApi)
        {
            _api = modApi;
            string dir = Path.Combine(modApi.Application.GetPathFor(AppFolder.Mod), "TakaroEmpyrion");
            if (modApi.Application.Mode == ApplicationMode.PlayfieldServer)
            {
                // Playfield servers only forward kill/death game events to the dedi; the Takaro link lives on the dedi.
                bool debug = false;
                try { debug = Config.Load(Path.Combine(dir, "TakaroConfig.txt")).Debug; } catch (Exception) { }
                _agent = new PlayfieldAgent(modApi, dir, debug);
                _agent.Start();
                return;
            }
            if (modApi.Application.Mode != ApplicationMode.DedicatedServer) return;
            _log = new FileLog(Path.Combine(dir, "Logs"), modApi.LogWarning);
            try
            {
                _cfg = Config.Load(Path.Combine(dir, "TakaroConfig.txt"));
            }
            catch (Exception e)
            {
                _log.Error("Takaro connector disabled: " + e.Message + " (copy TakaroConfig.txt.example to TakaroConfig.txt)");
                return;
            }
            _log.DebugEnabled = _cfg.Debug;
            _cfg.ReadTelnetFromDedicatedYaml(Environment.CurrentDirectory, Environment.GetCommandLineArgs());
            _log.Info("Takaro connector " + Version + " starting (mode " + modApi.Application.Mode + ", telnet console " +
                      (_cfg.TelnetPort > 0 ? "on port " + _cfg.TelnetPort : "off") + ")");

            if (_game == null) _game = new GameBridge(_log); else _game.SetLog(_log);
            _players = new PlayerRegistry(Path.Combine(dir, "players.json"));
            _players.MarkAllOffline();
            var v2 = new V2Adapter(modApi);
            var telnet = new TelnetConsole(_cfg.TelnetPort, _cfg.TelnetPassword);
            var catalog = new Catalog();
            string saveGame = v2.SaveGameDir();
            string cacheFolder = null;
            try { cacheFolder = modApi.Application.GetPathFor(AppFolder.Cache); } catch (Exception) { }
            _map = new MapService(
                () => MapService.FindPlayfieldsDir(cacheFolder, saveGame),
                () => string.IsNullOrEmpty(saveGame) ? null : Path.Combine(saveGame, "Templates"),
                _log);
            _actions = new Actions(_game, v2, _players, catalog, telnet, _cfg, _log, new BanReasons(Path.Combine(dir, "bans.json")),
                cmd => Task.Delay(1500).ContinueWith(_ => RunShutdownCommand(cmd)), _map);
            _client = new TakaroClient(() => new ClientWsConnection(), new ClientOptions
            {
                Url = _cfg.WsUrl,
                IdentityToken = _cfg.IdentityToken,
                RegistrationToken = _cfg.RegistrationToken,
                ServerName = _cfg.ServerName,
                Manifest = _cfg.ProtocolVersion >= 1 ? () => Capabilities.Build(Actions.Declared, _map.Capability()) : (Func<JArray>)null,
            }, _log, OnRequest);
            _events = new Events(_actions, _players, catalog, _log, (type, data) => _client.SendGameEvent(type, data));
            _game.UnsolicitedEvent += _events.OnLegacyEvent;
            modApi.Application.ChatMessageSent += _events.OnChat;
            modApi.Network.RegisterReceiverForPlayfieldPackets(_events.OnPlayfieldPacket);
            // The map scan reads only file listings and small yaml files; it runs before the first identify so the
            // manifest already names every planet, then once a minute for planets whose terrain appears later.
            try { _map.Rescan(); } catch (Exception e) { _log.Warn("map: scan failed: " + e.Message); }
            _client.Start();
            _mapTimer = new System.Threading.Timer(_ => RescanMap(), null, TimeSpan.FromMinutes(1), TimeSpan.FromMinutes(1));
            _started = true;
        }

        /// <summary>Runs the shutdown console command after the response has gone out, and logs what the game said.</summary>
        async Task RunShutdownCommand(string cmd)
        {
            try
            {
                _log.Info("shutdown: running '" + cmd + "'");
                string reply = await Actions.RunUntilAccepted(() => _actions.RunConsole(cmd), Task.Delay, _log);
                _log.Info("shutdown: console replied '" + LogText.Clip(reply ?? "", 300) + "'");
            }
            catch (Exception e) { _log.Error("shutdown: '" + cmd + "' failed: " + e.GetType().Name + ": " + e.Message); }
        }

        void RescanMap()
        {
            try
            {
                if (_map.Rescan() && _client.SendCapabilities()) _log.Info("map: sent the updated capability manifest");
            }
            catch (Exception e) { _log.Warn("map: rescan failed: " + e.Message); }
        }

        public void Shutdown()
        {
            _agent?.Stop();
            if (!_started) return;
            _mapTimer?.Dispose();
            _log.Info("Takaro connector stopping");
            try { _players.SaveSoon(force: true); } catch (Exception) { }
            _client.Stop();
        }

        // ------------------------------------------------------------ ModInterface (v1)

        public void Game_Start(ModGameAPI dediAPI)
        {
            if (_game == null) _game = new GameBridge(_log ?? (ILog)new NullLog());
            _game.Attach(dediAPI);
        }

        public void Game_Update()
        {
            _game?.Tick();
        }

        public void Game_Exit() { Shutdown(); }

        public void Game_Event(CmdId eventId, ushort seqNr, object data)
        {
            if (_game == null) return;
            try { _game.OnGameEvent(eventId, seqNr, data); }
            catch (Exception e) { _log?.Error("Game_Event " + eventId + " threw: " + e); }
        }

        // ------------------------------------------------------------ requests

        void OnRequest(InboundRequest req)
        {
            Task.Run(async () =>
            {
                try
                {
                    Args args = Args.Parse(req.RawArgs);
                    if (_cfg.Debug && args.Raw.Count > 0)
                    {
                        var shown = (JObject)args.Raw.DeepClone();
                        if (shown["player"] is JObject pl) shown["player"] = new JObject { ["gameId"] = args.PlayerGameId() };
                        _log.Debug(req.Action + " args " + LogText.Clip(shown.ToString(Newtonsoft.Json.Formatting.None), 400));
                    }
                    JToken result = await _actions.Run(req.Action, args);
                    _client.SendResponse(req.RequestId, result);
                }
                catch (Exception e)
                {
                    Actions.Classify(e, out string code, out string msg);
                    if (code == ErrorCodes.NotFound && req.Action == "getMapTile") _log.Debug(req.Action + ": " + msg);
                    else _log.Warn(req.Action + " failed (" + code + "): " + msg);
                    _client.SendError(req.RequestId, msg, code);
                }
            });
        }

        sealed class V2Adapter : IGameV2
        {
            readonly IModApi _api;
            public V2Adapter(IModApi api) { _api = api; }
            public void SendChatMessage(MessageData msg) { _api.Application.SendChatMessage(msg); }
            public Dictionary<string, int> GetBlockAndItemMapping() { return _api.Application.GetBlockAndItemMapping(); }
            public IEnumerable<string> ContentDirs()
            {
                yield return _api.Application.GetPathFor(AppFolder.ActiveScenario);
                yield return _api.Application.GetPathFor(AppFolder.Content);
            }
            public string SaveGameDir() { return _api.Application.GetPathFor(AppFolder.SaveGame); }
        }

        sealed class NullLog : ILog
        {
            public void Debug(string s) { }
            public void Info(string s) { }
            public void Warn(string s) { }
            public void Error(string s) { }
        }
    }
}
