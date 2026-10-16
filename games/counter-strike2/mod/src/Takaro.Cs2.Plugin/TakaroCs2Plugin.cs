using CounterStrikeSharp.API.Core;
using CounterStrikeSharp.API.Modules.Commands;
using Takaro.Cs2.Core;

namespace Takaro.Cs2.Plugin;

public sealed class TakaroCs2Plugin : BasePlugin, IPluginConfig<Cs2TakaroConfig>
{
    private TakaroWebSocketRunner? runner;
    private Cs2ServerAdapter? adapter;
    private Cs2BanStore? bans;
    private Cs2PlayerStore? playerStore;
    private readonly Dictionary<string, TakaroPlayer> knownPlayers = new(StringComparer.OrdinalIgnoreCase);
    private readonly Dictionary<int, TakaroPlayer> knownBySlot = new();
    private readonly HashSet<string> connectedEmitted = new(StringComparer.OrdinalIgnoreCase);

    public override string ModuleName => "Takaro CS2 Bridge";
    public override string ModuleVersion => "0.1.0";
    public override string ModuleAuthor => "Takaro community";

    public Cs2TakaroConfig Config { get; set; } = new();

    public void OnConfigParsed(Cs2TakaroConfig config)
    {
        Config = config;
    }

    public override void Load(bool hotReload)
    {
        var coreConfig = Config.ToCoreConfig();
        var errors = coreConfig.Validate();
        if (errors.Count > 0)
        {
            foreach (var error in errors)
            {
                Console.WriteLine($"[Takaro CS2] Config error: {error}");
            }

            return;
        }

        var dataDir = Path.GetFullPath(Path.Combine(ModuleDirectory, "..", "..", "data", "TakaroCs2"));
        try
        {
            bans = new Cs2BanStore(Path.Combine(dataDir, "bans.json"));
            playerStore = new Cs2PlayerStore(Path.Combine(dataDir, "players.json"));
        }
        catch (Exception ex)
        {
            Console.WriteLine($"[Takaro CS2] Ban store cannot be loaded: {ex.Message}");
            return;
        }
        adapter = new Cs2ServerAdapter(coreConfig);
        adapter.SetBanStore(bans);
        adapter.SetPlayerStore(playerStore);
        runner = new TakaroWebSocketRunner(coreConfig, adapter, message => Console.WriteLine($"[Takaro CS2] {message}"));
        _ = runner.StartAsync();

        RegisterEventHandler<EventPlayerConnectFull>(OnPlayerConnectFull);
        RegisterEventHandler<EventPlayerSpawn>(OnPlayerSpawn);
        RegisterListener<Listeners.OnClientAuthorized>(OnClientAuthorized);
        RegisterEventHandler<EventPlayerDisconnect>(OnPlayerDisconnect);
        RegisterEventHandler<EventPlayerDeath>(OnPlayerDeath);
        RegisterEventHandler<EventOtherDeath>(OnOtherDeath);
        AddCommandListener("say", OnSay, HookMode.Post);
        AddCommandListener("say_team", OnSayTeam, HookMode.Post);

        _ = EmitAsync("log", EventFactory.Log("Takaro CS2 bridge loaded."));
    }

    public override void Unload(bool hotReload)
    {
        _ = EmitAsync("log", EventFactory.Log("Takaro CS2 bridge unloading."));
        runner?.Dispose();
        runner = null;
        adapter = null;
        knownPlayers.Clear();
        knownBySlot.Clear();
        connectedEmitted.Clear();
        bans = null;
        playerStore = null;
    }

    private HookResult OnPlayerConnectFull(EventPlayerConnectFull ev, GameEventInfo info)
    {
        RememberAuthenticatedPlayer(ev.Userid);
        return HookResult.Continue;
    }

    private HookResult OnPlayerSpawn(EventPlayerSpawn ev, GameEventInfo info)
    {
        if (ev.Userid is { } controller)
        {
            var slot = controller.Slot;
            CounterStrikeSharp.API.Server.NextWorldUpdate(() =>
                RememberAuthenticatedPlayer(CounterStrikeSharp.API.Utilities.GetPlayerFromSlot(slot)));
        }
        return HookResult.Continue;
    }

    private void OnClientAuthorized(int slot, CounterStrikeSharp.API.Modules.Entities.SteamID steamId) =>
        CounterStrikeSharp.API.Server.NextWorldUpdate(() =>
        {
            var controller = CounterStrikeSharp.API.Utilities.GetPlayerFromSlot(slot);
            if (bans?.IsBanned(steamId.SteamId64.ToString()) == true)
            {
                controller?.Disconnect(CounterStrikeSharp.API.ValveConstants.Protobuf.NetworkDisconnectionReason.NETWORK_DISCONNECT_KICKBANADDED);
                return;
            }
            RememberAuthenticatedPlayer(controller);
        });

    private void RememberAuthenticatedPlayer(CCSPlayerController? controller)
    {
        var player = MapPlayer(controller);
        if (player is not null)
        {
            if (bans?.IsBanned(player.GameId) == true)
            {
                controller?.Disconnect(CounterStrikeSharp.API.ValveConstants.Protobuf.NetworkDisconnectionReason.NETWORK_DISCONNECT_KICKBANADDED);
                return;
            }
            if (!knownPlayers.ContainsKey(player.GameId))
            {
                Console.WriteLine($"[Takaro CS2] Authenticated player remembered: {player.GameId}.");
                knownPlayers[player.GameId] = player;
                if (controller is not null) knownBySlot[controller.Slot] = player;
                playerStore?.Remember(player);
            }
            if (controller is { PawnIsAlive: true } && connectedEmitted.Add(player.GameId))
            {
                adapter?.RememberPosition(player, controller);
                _ = EmitAsync("player-connected", EventFactory.PlayerConnected(player));
            }
        }
    }

    private HookResult OnPlayerDisconnect(EventPlayerDisconnect ev, GameEventInfo info)
    {
        var player = MapPlayer(ev.Userid) ?? (ev.Userid is not null && knownBySlot.TryGetValue(ev.Userid.Slot, out var known) ? known : null);
        if (player is not null)
        {
            adapter?.RememberPosition(player, ev.Userid);
            knownPlayers.Remove(player.GameId);
            if (connectedEmitted.Remove(player.GameId))
                _ = EmitAsync("player-disconnected", EventFactory.PlayerDisconnected(player, ev.Reason.ToString()));
            if (ev.Userid is not null) knownBySlot.Remove(ev.Userid.Slot);
        }

        return HookResult.Continue;
    }

    private HookResult OnPlayerDeath(EventPlayerDeath ev, GameEventInfo info)
    {
        var victim = MapPlayer(ev.Userid);
        if (victim is null)
        {
            return HookResult.Continue;
        }

        var attacker = MapPlayer(ev.Attacker);
        if (attacker?.GameId == victim.GameId)
        {
            attacker = null;
        }

        var origin = ev.Userid?.PlayerPawn.Value?.AbsOrigin;
        var position = origin is null ? null : new TakaroPosition(origin.X, origin.Y, origin.Z);
        _ = EmitAsync("player-death", EventFactory.PlayerDeath(victim, attacker, ev.Weapon, ev.Headshot, position));
        return HookResult.Continue;
    }

    private HookResult OnOtherDeath(EventOtherDeath ev, GameEventInfo info)
    {
        var attacker = MapPlayer(CounterStrikeSharp.API.Utilities.GetPlayerFromUserid(ev.Attacker));
        if (attacker is null || string.IsNullOrWhiteSpace(ev.Othertype) || string.IsNullOrWhiteSpace(ev.Weapon)) return HookResult.Continue;
        var entity = ev.Othertype.Trim().ToLowerInvariant() switch
        {
            "chicken" or "npc_chicken" => "Chicken",
            "hostage" or "npc_hostage" => "Hostage",
            _ => System.Globalization.CultureInfo.InvariantCulture.TextInfo.ToTitleCase(ev.Othertype.Replace('_', ' '))
        };
        var weaponCode = ev.Weapon.StartsWith("weapon_", StringComparison.OrdinalIgnoreCase) ? ev.Weapon : $"weapon_{ev.Weapon}";
        var weapon = Cs2ItemCatalog.Name(weaponCode);
        _ = EmitAsync("entity-killed", EventFactory.EntityKilled(attacker, entity, weapon));
        return HookResult.Continue;
    }

    private HookResult OnSay(CCSPlayerController? player, CommandInfo commandInfo) =>
        OnChat(player, commandInfo, teamOnly: false);

    private HookResult OnSayTeam(CCSPlayerController? player, CommandInfo commandInfo) =>
        OnChat(player, commandInfo, teamOnly: true);

    private HookResult OnChat(CCSPlayerController? controller, CommandInfo commandInfo, bool teamOnly)
    {
        var player = MapPlayer(controller);
        var message = commandInfo.ArgString.Trim().Trim('"');
        Console.WriteLine($"[Takaro CS2] Chat hook: authenticated={player is not null}, teamOnly={teamOnly}.");
        if (player is not null && !string.IsNullOrWhiteSpace(message))
        {
            _ = EmitAsync("chat-message", EventFactory.ChatMessage(player, message, teamOnly));
        }

        return HookResult.Continue;
    }

    private TakaroPlayer? MapPlayer(CCSPlayerController? controller)
    {
        var player = Cs2ServerAdapter.ToCs2Player(controller);
        if (player is null || (!Config.IncludeBots && player.IsBot))
        {
            return null;
        }

        return PlayerMapper.ToTakaroPlayer(player);
    }

    private Task EmitAsync(string eventType, object data)
    {
        Console.WriteLine($"[Takaro CS2] Event queued: {eventType}.");
        return runner?.SendGameEventAsync(eventType, data) ?? Task.CompletedTask;
    }
}
