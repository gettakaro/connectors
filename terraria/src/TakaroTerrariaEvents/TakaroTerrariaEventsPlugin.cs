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

        var killer = ResolveKiller(npc);

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

    // Resolves the player to credit for an NPC kill.
    //
    // Terraria records no "who landed the killing blow" on NPC death, so we probe
    // the interaction bookkeeping it does keep, strongest signal first:
    //
    //  1. npc.playerInteraction - a bool[] indexed by player slot, set for every
    //     player who damaged this NPC. This is the only signal that survives
    //     ranged, projectile, minion and trap kills, because the engine flags the
    //     projectile's owner, not whoever happened to swing last. We only trust it
    //     when exactly one player is flagged; with several contributors there is no
    //     way to tell which one finished the NPC.
    //  2. npc.lastInteraction - the most recent player to interact. Note this is
    //     NOT -1 when unset: Terraria initialises it to 255 (Main.maxPlayers), a
    //     sentinel that is still inside TShock.Players' bounds, so a naive index
    //     check treats the "nobody" value as a real slot.
    //  3. npc.target / npc.oldTarget - who the NPC was aggroed on. Weaker still,
    //     since aggro can point at a player who dealt no damage at all, but it is
    //     better than nothing for AI types that never set an interaction.
    //
    // Returns null when nothing resolves, which surfaces as no player on the event.
    private static TSPlayer? ResolveKiller(NPC npc)
    {
        return SoleInteractingPlayer(npc)
            ?? ActivePlayerByIndex(npc.lastInteraction)
            ?? ActivePlayerByIndex(npc.target)
            ?? ActivePlayerByIndex(npc.oldTarget)
            ?? SoleActiveTSPlayer();
    }

    // Returns the only player flagged in npc.playerInteraction, or null when zero
    // or several are flagged. Two players who both hit the NPC give us no basis to
    // pick between them, and crediting the wrong one is worse than crediting none.
    private static TSPlayer? SoleInteractingPlayer(NPC npc)
    {
        var interactions = npc.playerInteraction;
        if (interactions is null)
        {
            return null;
        }

        TSPlayer? found = null;
        for (var index = 0; index < interactions.Length; index++)
        {
            if (!interactions[index])
            {
                continue;
            }

            var player = ActivePlayerByIndex(index);
            if (player is null)
            {
                continue;
            }

            if (found is not null)
            {
                return null;
            }

            found = player;
        }

        return found;
    }

    // Best-effort weapon attribution: Terraria's NPC carries no record of what
    // killed it, so we report the killer's selected item at the moment the kill
    // fires. This is a proxy, not exact attribution - a projectile fired earlier
    // can land after the player swaps weapons, and damage-over-time, minion or
    // sentry kills may credit an item that dealt none of the damage. Terraria
    // exposes no true damage source on NPC death, so this is the best signal
    // available.
    //
    // We only report items that could plausibly have dealt the killing blow, so a
    // zero-damage consumable or tool that merely occupied the selected hotbar slot
    // (a Mushroom, a torch, a building block) is rejected rather than reported as
    // the murder weapon. A wrong weapon name is worse than no weapon name, because
    // downstream consumers cannot tell it is wrong; an honest unknown can at least
    // be filtered.
    //
    // Returns an empty string for every genuinely-unknown case; Takaro's
    // entity-killed DTO types weapon as a string, so we never emit null or omit the
    // field, and the bridge renders empty as "unknown".
    private static string HeldWeaponName(TSPlayer? player)
    {
        return WeaponName(SelectedItem(player));
    }

    // Player.HeldItem is compiled as inventory[selectedItem], so it is the same
    // object rather than an independent fallback; we index the inventory ourselves
    // to stay safe when selectedItem is out of range mid-swap, and fall back to
    // HeldItem only if that indexing is not possible.
    private static Item? SelectedItem(TSPlayer? player)
    {
        var terrariaPlayer = player?.TPlayer;
        if (terrariaPlayer is null)
        {
            return null;
        }

        var inventory = terrariaPlayer.inventory;
        var selected = terrariaPlayer.selectedItem;
        if (inventory is not null && selected >= 0 && selected < inventory.Length)
        {
            return inventory[selected];
        }

        return terrariaPlayer.HeldItem;
    }

    // An item only counts as the killing weapon when it actually exists in the slot
    // (type/stack) and can deal damage at all (damage). Item.damage is 0 for
    // consumables, blocks and pure utility tools, which is exactly the false
    // attribution we want to drop.
    private static string WeaponName(Item? item)
    {
        if (item is null || item.type <= 0 || item.stack <= 0 || item.damage <= 0)
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

    // Last-resort killer guess. Only answers when exactly one player is online, in
    // which case the kill must be theirs. With two or more connected, picking the
    // first active slot is just a guess that is usually wrong on a busy server, and
    // attributing a kill to the wrong player is worse than attributing it to nobody
    // - so we return null and let the event report no killer.
    private static TSPlayer? SoleActiveTSPlayer()
    {
        TSPlayer? found = null;
        foreach (var player in TShock.Players)
        {
            if (player is null || !player.Active)
            {
                continue;
            }

            if (found is not null)
            {
                return null;
            }

            found = player;
        }

        return found;
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
