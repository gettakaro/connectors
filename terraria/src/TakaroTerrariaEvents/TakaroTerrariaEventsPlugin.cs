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
        Commands.ChatCommands.Add(new Command(AdminPermission, TakaroInventory, "takaroinv")
        {
            HelpText = "Prints a player's inventory for Takaro."
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
            Commands.ChatCommands.RemoveAll(command => command.Names.Contains("takaroinv"));
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

    private static void TakaroInventory(CommandArgs args)
    {
        if (args.Parameters.Count != 1)
        {
            args.Player.SendErrorMessage("Usage: /takaroinv <player>");
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

        var player = matches[0].TPlayer;
        var totals = new Dictionary<int, int>();
        var names = new Dictionary<int, string>();
        CollectItems(player?.inventory, totals, names);
        CollectItems(player?.armor, totals, names);
        CollectItems(player?.dye, totals, names);
        CollectItems(player?.miscEquips, totals, names);
        CollectItems(player?.miscDyes, totals, names);
        CollectItem(player?.trashItem, totals, names);
        CollectChest(player?.bank, totals, names);
        CollectChest(player?.bank2, totals, names);
        CollectChest(player?.bank3, totals, names);
        CollectChest(player?.bank4, totals, names);
        CollectLoadouts(player?.Loadouts, totals, names);

        var items = totals.Select(entry => new
        {
            code = entry.Key.ToString(CultureInfo.InvariantCulture),
            name = names.TryGetValue(entry.Key, out var name) ? name : entry.Key.ToString(CultureInfo.InvariantCulture),
            amount = entry.Value,
            quality = string.Empty
        }).ToArray();

        var json = JsonSerializer.Serialize(new { items });
        args.Player.SendInfoMessage($"TAKARO_INVENTORY {json}");
    }

    private static void CollectItems(Item[]? slots, Dictionary<int, int> totals, Dictionary<int, string> names)
    {
        if (slots is null)
        {
            return;
        }

        foreach (var item in slots)
        {
            CollectItem(item, totals, names);
        }
    }

    private static void CollectItem(Item? item, Dictionary<int, int> totals, Dictionary<int, string> names)
    {
        if (item is null || item.type <= 0 || item.stack <= 0)
        {
            return;
        }

        totals[item.type] = totals.TryGetValue(item.type, out var amount) ? amount + item.stack : item.stack;
        if (!names.ContainsKey(item.type))
        {
            names[item.type] = NonEmpty(item.Name) ?? $"Item {item.type.ToString(CultureInfo.InvariantCulture)}";
        }
    }

    private static void CollectChest(Chest? chest, Dictionary<int, int> totals, Dictionary<int, string> names)
    {
        CollectItems(chest?.item, totals, names);
    }

    private static void CollectLoadouts(EquipmentLoadout[]? loadouts, Dictionary<int, int> totals, Dictionary<int, string> names)
    {
        if (loadouts is null)
        {
            return;
        }

        foreach (var loadout in loadouts)
        {
            if (loadout is null)
            {
                continue;
            }

            CollectItems(loadout.Armor, totals, names);
            CollectItems(loadout.Dye, totals, names);
        }
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

        var killer = ActivePlayerByIndex(npc.lastInteraction) ?? FirstActiveTSPlayer();

        Emit("entity-killed", new
        {
            player = killer is null ? null : PlayerDto(killer, killer.Name),
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
            weapon = HeldWeaponName(killer)
        });
    }

    // Best-effort weapon attribution: Terraria's NPC carries no record of what
    // killed it, so we report the killer's held item at the moment the kill fires.
    // This is a proxy, not exact attribution - a projectile fired earlier can land
    // after the player swaps weapons, and damage-over-time, minion or sentry kills
    // may credit a held item that dealt none of the damage. Terraria exposes no
    // true damage source on NPC death, so this is the best signal available.
    // Returns an empty string when unknown; Takaro's entity-killed DTO types
    // weapon as a string, so we never emit null or omit the field.
    private static string HeldWeaponName(TSPlayer? player)
    {
        var item = player?.TPlayer?.HeldItem;
        if (item is null || item.type <= 0)
        {
            return string.Empty;
        }

        return NonEmpty(item.Name) ?? string.Empty;
    }

    private static TSPlayer? ActivePlayerByIndex(int index)
    {
        if (index < 0 || index >= TShock.Players.Length)
        {
            return null;
        }

        var player = TShock.Players[index];
        return player is not null && player.Active ? player : null;
    }

    private static TSPlayer? FirstActiveTSPlayer()
    {
        foreach (var player in TShock.Players)
        {
            if (player is not null && player.Active)
            {
                return player;
            }
        }

        return null;
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
