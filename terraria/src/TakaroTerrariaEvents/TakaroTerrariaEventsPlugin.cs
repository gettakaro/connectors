using System.Globalization;
using System.Text.Json;
using Terraria;
using TerrariaApi.Server;
using TShockAPI;

namespace TakaroTerrariaEvents;

[ApiVersion(2, 1)]
public sealed class TakaroTerrariaEventsPlugin : TerrariaPlugin
{
    private const string AdminPermission = "takaro.admin";

    public override string Name => "Takaro Terraria Events";
    public override Version Version => new(0, 1, 0);
    public override string Author => "Takaro";
    public override string Description => "Emits Takaro event markers for player deaths and NPC kills.";

    public TakaroTerrariaEventsPlugin(Main game)
        : base(game)
    {
    }

    public override void Initialize()
    {
        GetDataHandlers.KillMe.Register(OnPlayerDeath, HandlerPriority.Normal, false);
        ServerApi.Hooks.NpcKilled.Register(this, OnNpcKilled);
        Commands.ChatCommands.Add(new Command(AdminPermission, TakaroTeleport, "takarotp")
        {
            HelpText = "Teleports a player to world X/Y coordinates for Takaro."
        });
        Commands.ChatCommands.Add(new Command(AdminPermission, TakaroPosition, "takaropos")
        {
            HelpText = "Prints a player's world X/Y coordinates for Takaro."
        });
        TShock.Log.ConsoleInfo("Takaro Terraria Events plugin loaded");
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing)
        {
            GetDataHandlers.KillMe.UnRegister(OnPlayerDeath);
            ServerApi.Hooks.NpcKilled.Deregister(this, OnNpcKilled);
            Commands.ChatCommands.RemoveAll(command => command.Names.Contains("takarotp"));
            Commands.ChatCommands.RemoveAll(command => command.Names.Contains("takaropos"));
        }

        base.Dispose(disposing);
    }

    private static void TakaroTeleport(CommandArgs args)
    {
        if (args.Parameters.Count != 3)
        {
            args.Player.SendErrorMessage("Usage: /takarotp <player> <x> <y>");
            return;
        }

        var matches = TSPlayer.FindByNameOrID(args.Parameters[0]);
        if (matches.Count != 1)
        {
            args.Player.SendErrorMessage(matches.Count == 0
                ? $"No player found matching '{args.Parameters[0]}'."
                : $"Multiple players found matching '{args.Parameters[0]}'.");
            return;
        }

        if (!float.TryParse(args.Parameters[1], NumberStyles.Float, CultureInfo.InvariantCulture, out var x)
            || !float.TryParse(args.Parameters[2], NumberStyles.Float, CultureInfo.InvariantCulture, out var y))
        {
            args.Player.SendErrorMessage("X and Y must be numeric world coordinates.");
            return;
        }

        var target = matches[0];
        var success = target.Teleport(x, y, 1);
        if (!success)
        {
            args.Player.SendErrorMessage($"Failed to teleport {target.Name}.");
            return;
        }

        args.Player.SendSuccessMessage($"Teleported {target.Name} to {x.ToString(CultureInfo.InvariantCulture)}, {y.ToString(CultureInfo.InvariantCulture)}.");
    }

    private static void TakaroPosition(CommandArgs args)
    {
        if (args.Parameters.Count != 1)
        {
            args.Player.SendErrorMessage("Usage: /takaropos <player>");
            return;
        }

        var matches = TSPlayer.FindByNameOrID(args.Parameters[0]);
        if (matches.Count != 1)
        {
            args.Player.SendErrorMessage(matches.Count == 0
                ? $"No player found matching '{args.Parameters[0]}'."
                : $"Multiple players found matching '{args.Parameters[0]}'.");
            return;
        }

        var position = matches[0].TPlayer.position;
        var json = JsonSerializer.Serialize(new
        {
            x = position.X,
            y = position.Y,
            z = 0
        });
        args.Player.SendInfoMessage($"TAKARO_POSITION {json}");
    }

    private static void OnPlayerDeath(object? sender, GetDataHandlers.KillMeEventArgs args)
    {
        var player = args.Player;
        var name = NonEmpty(player?.Name) ?? $"player:{args.PlayerId}";
        var reason = DeathReasonText(args.PlayerDeathReason, name);

        Emit("player-death", new
        {
            player = PlayerDto(player, name),
            reason,
            damage = args.Damage,
            pvp = args.Pvp,
            direction = args.Direction
        });
    }

    private static void OnNpcKilled(NpcKilledEventArgs args)
    {
        var npc = args.npc;
        if (npc is null)
        {
            return;
        }

        Emit("entity-killed", new
        {
            player = PlayerByIndex(npc.lastInteraction) ?? FirstActivePlayer(),
            entity = new
            {
                gameId = $"npc:{npc.whoAmI}",
                name = NonEmpty(npc.GivenOrTypeName) ?? NonEmpty(npc.FullName) ?? $"NPC {npc.type}",
                platformId = $"terraria:npc:{npc.whoAmI}",
                type = npc.type,
                netId = npc.netID,
                boss = npc.boss,
                position = new { x = npc.position.X, y = npc.position.Y }
            },
        });
    }

    private static object PlayerDto(TSPlayer? player, string fallbackName)
    {
        var name = NonEmpty(player?.Name) ?? fallbackName;
        var stableId = NonEmpty(player?.UUID) ?? name;
        return new
        {
            gameId = name,
            name,
            platformId = $"terraria:{stableId}",
            ip = NonEmpty(player?.IP),
            tshockIndex = player?.Index
        };
    }

    private static object? PlayerByIndex(int index)
    {
        if (index < 0 || index >= TShock.Players.Length)
        {
            return null;
        }

        var player = TShock.Players[index];
        if (player is null || !player.Active)
        {
            return null;
        }

        return PlayerDto(player, player.Name);
    }

    private static object? FirstActivePlayer()
    {
        foreach (var player in TShock.Players)
        {
            if (player is not null && player.Active)
            {
                return PlayerDto(player, player.Name);
            }
        }

        return null;
    }

    private static string DeathReasonText(Terraria.DataStructures.PlayerDeathReason reason, string playerName)
    {
        try
        {
            return reason.GetDeathText(playerName).ToString();
        }
        catch
        {
            return $"{playerName} died";
        }
    }

    private static string? NonEmpty(string? value)
    {
        return string.IsNullOrWhiteSpace(value) ? null : value.Trim();
    }

    private static void Emit(string type, object data)
    {
        var json = JsonSerializer.Serialize(new { type, data });
        TShock.Log.ConsoleInfo($"TAKARO_EVENT {json}");
    }
}
