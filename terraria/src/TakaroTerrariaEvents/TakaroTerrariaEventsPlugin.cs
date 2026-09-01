using System.Globalization;
using System.Linq;
using System.Text.Json;
using Terraria;
using TerrariaApi.Server;
using TShockAPI;

namespace TakaroTerrariaEvents;

[ApiVersion(2, 1)]
public sealed class TakaroTerrariaEventsPlugin : TerrariaPlugin
{
    private const string AdminPermission = "takaro.admin";

    // Main inventory slot range, verified against the Terraria assembly's own constants:
    // Main.InventoryItemSlotsStart = 0 and Main.InventoryItemSlotsCount = 50. Coin slots
    // start at 50, ammo slots at 54, and Main.InventorySlotsTotal is 58. See
    // BuildPlacementPlan for why the coin and ammo slots are excluded.
    private const int MainInventorySlotStart = 0;
    private const int MainInventorySlotCount = 50;

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
        Commands.ChatCommands.Add(new Command(AdminPermission, TakaroGive, "takarogive")
        {
            HelpText = "Gives a player an item for Takaro, refusing if the inventory is full."
        });
        Commands.ChatCommands.Add(new Command(AdminPermission, TakaroBan, "takaroban")
        {
            HelpText = "Bans a player by UUID for Takaro so the ban survives a reconnect."
        });
        Commands.ChatCommands.Add(new Command(AdminPermission, TakaroUnban, "takarounban")
        {
            HelpText = "Lifts every active Takaro ban identifier for a player."
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
            Commands.ChatCommands.RemoveAll(command => command.Names.Contains("takarogive"));
            Commands.ChatCommands.RemoveAll(command => command.Names.Contains("takaroban"));
            Commands.ChatCommands.RemoveAll(command => command.Names.Contains("takarounban"));
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

    // Bans a player on their TShock UUID rather than their display name.
    //
    // TShock only matches an untyped name ban against players who authenticated under
    // that name, so on a server where players join unauthenticated a name ban records
    // cleanly and then lets the player walk straight back in. `uuid:` is matched on every
    // join regardless of authentication. The IP is banned alongside it as a second
    // identifier, since a fresh client install produces a new UUID.
    //
    // REST cannot supply this: /v2/players/list exposes nickname/group/state and no UUID,
    // which is why the sidecar has to come through the plugin for bans.
    private static void TakaroBan(CommandArgs args)
    {
        if (args.Parameters.Count < 1)
        {
            args.Player.SendErrorMessage("Usage: /takaroban <player> [reason]");
            return;
        }

        var matches = TSPlayer.FindByNameOrID(args.Parameters[0]);
        if (matches.Count != 1)
        {
            var failure = JsonSerializer.Serialize(new
            {
                success = false,
                reason = matches.Count == 0
                    ? $"No player found matching '{args.Parameters[0]}'."
                    : $"Multiple players found matching '{args.Parameters[0]}'."
            });
            args.Player.SendInfoMessage($"TAKARO_BAN {failure}");
            return;
        }

        var target = matches[0];
        var reason = args.Parameters.Count > 1
            ? string.Join(" ", args.Parameters.GetRange(1, args.Parameters.Count - 1))
            : "Banned by Takaro";

        var identifiers = BanIdentifiersFor(target);
        if (identifiers.Count == 0)
        {
            var failure = JsonSerializer.Serialize(new
            {
                success = false,
                reason = $"{target.Name} exposes no UUID or IP to ban on."
            });
            args.Player.SendInfoMessage($"TAKARO_BAN {failure}");
            return;
        }

        var applied = new List<string>();
        foreach (var identifier in identifiers)
        {
            try
            {
                // Tag the ban with the player name so TakaroUnban can find it later: a
                // banned player is offline by definition, so the UUID cannot be re-derived
                // from a live TSPlayer at unban time.
                TShock.Bans.InsertBan(identifier, $"{reason} {BanNameTag(target.Name)}", "takaro", DateTime.UtcNow, DateTime.MaxValue);
                applied.Add(identifier);
            }
            catch (Exception ex)
            {
                TShock.Log.ConsoleError($"Takaro ban on {identifier} failed: {ex.Message}");
            }
        }

        if (applied.Count == 0)
        {
            var failure = JsonSerializer.Serialize(new
            {
                success = false,
                reason = $"Could not record any ban identifier for {target.Name}."
            });
            args.Player.SendInfoMessage($"TAKARO_BAN {failure}");
            return;
        }

        target.Disconnect($"Banned: {reason}");

        var json = JsonSerializer.Serialize(new
        {
            success = true,
            player = target.Name,
            identifiers = applied,
            reason
        });
        args.Player.SendInfoMessage($"TAKARO_BAN {json}");
    }

    // Lifts every active ban matching any identifier this player is known by.
    //
    // A ban may have been recorded against the UUID, the IP or the bare name depending on
    // which build wrote it, and clearing only one leaves the player locked out while the
    // call reports success -- the exact failure this replaces.
    private static void TakaroUnban(CommandArgs args)
    {
        if (args.Parameters.Count != 1)
        {
            args.Player.SendErrorMessage("Usage: /takarounban <player>");
            return;
        }

        var name = args.Parameters[0];
        var removed = new List<string>();

        try
        {
            // A banned player is offline, so their UUID cannot be read from a live TSPlayer
            // here -- looking them up that way is why an unban could only ever succeed for
            // someone who was not actually banned. Scan the ban list instead: match the name
            // tag written at ban time, plus any bare-name ban from an older build.
            var tag = BanNameTag(name);
            foreach (var ban in TShock.Bans.Bans.Values.ToArray())
            {
                var identifier = ban.Identifier ?? string.Empty;
                var matchesTag = (ban.Reason ?? string.Empty).Contains(tag, StringComparison.OrdinalIgnoreCase);
                var matchesName = identifier.Equals(name, StringComparison.OrdinalIgnoreCase)
                    || identifier.Equals($"name:{name}", StringComparison.OrdinalIgnoreCase);

                if (!matchesTag && !matchesName)
                {
                    continue;
                }

                TShock.Bans.RemoveBan(ban.TicketNumber, true);
                removed.Add($"#{ban.TicketNumber} {identifier}");
            }

            // An online player can also be matched directly, which covers a ban recorded
            // before this tagging existed.
            var matches = TSPlayer.FindByNameOrID(name);
            if (matches.Count == 1)
            {
                foreach (var identifier in BanIdentifiersFor(matches[0]))
                {
                    foreach (var ban in TShock.Bans.RetrieveBansByIdentifier(identifier))
                    {
                        TShock.Bans.RemoveBan(ban.TicketNumber, true);
                        removed.Add($"#{ban.TicketNumber} {identifier}");
                    }
                }
            }
        }
        catch (Exception ex)
        {
            TShock.Log.ConsoleError($"Takaro unban for {name} failed: {ex.Message}");
        }

        var json = JsonSerializer.Serialize(new
        {
            success = true,
            player = name,
            removed,
            count = removed.Count
        });
        args.Player.SendInfoMessage($"TAKARO_UNBAN {json}");
    }

    /// Marker appended to a Takaro ban reason so the ban can be found again once the
    /// player is offline and their UUID is no longer readable from a live TSPlayer.
    private static string BanNameTag(string playerName) => $"[takaro:{playerName}]";

    // UUID first: it is the identifier TShock matches on every join. IP second, so a
    // reinstalled client with a fresh UUID is still caught.
    private static List<string> BanIdentifiersFor(TSPlayer player)
    {
        var identifiers = new List<string>();
        var uuid = NonEmpty(player.UUID);
        if (uuid is not null)
        {
            identifiers.Add($"uuid:{uuid}");
        }

        var ip = NonEmpty(player.IP);
        if (ip is not null)
        {
            identifiers.Add($"ip:{ip}");
        }

        return identifiers;
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

    /// <summary>
    /// Gives an item to a player, placing the whole order into their inventory or refusing
    /// outright. There is no partial delivery and no floor drop.
    ///
    /// TShock's own Commands.Give refuses when TSPlayer.InventorySlotAvailable is false —
    /// that property only counts completely empty slots, so a player whose inventory is
    /// full is refused even when the item would have stacked onto an existing partial
    /// stack. A refused give makes a Takaro shop purchase charge the player and deliver
    /// nothing, so we do not use that gate.
    ///
    /// We do not use TSPlayer.GiveItem either, for two reasons confirmed against this
    /// server's config and the TShock assembly:
    ///
    ///   1. GiveItem only places items directly when TShock.Config.Settings.
    ///      GiveItemsDirectly is true AND Main.ServerSideCharacter is enabled. This server
    ///      has GiveItemsDirectly false and no SSC, so every give routes to GiveItemByDrop
    ///      and lands on the ground. A purchase at the player's feet is easy to miss and
    ///      can despawn, so a drop is a delivery failure, not a success.
    ///   2. Even with direct placement enabled, GiveItemDirectly fills what it can and
    ///      then drops the remainder, so a request for 50 into room for 20 places 20 and
    ///      drops 30. That is a partially lost order.
    ///
    /// So we do the placement ourselves: compute exact capacity, refuse unless the entire
    /// amount fits, and only then write into the inventory and sync each touched slot.
    /// Takaro turns a reported failure into a shop-order-delivery-failed event, which a
    /// module can surface to the player, so refusing is visible and actionable.
    /// </summary>
    private static void TakaroGive(CommandArgs args)
    {
        if (args.Parameters.Count is < 3 or > 4)
        {
            args.Player.SendErrorMessage("Usage: /takarogive <player> <itemId> <amount> [prefix]");
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

        if (!int.TryParse(args.Parameters[1], NumberStyles.Integer, CultureInfo.InvariantCulture, out var itemId) || itemId == 0)
        {
            EmitGiveFailure(args, $"Invalid item id '{args.Parameters[1]}'.");
            return;
        }

        if (!int.TryParse(args.Parameters[2], NumberStyles.Integer, CultureInfo.InvariantCulture, out var amount) || amount <= 0)
        {
            EmitGiveFailure(args, $"Invalid amount '{args.Parameters[2]}'.");
            return;
        }

        var prefix = 0;
        if (args.Parameters.Count == 4
            && !int.TryParse(args.Parameters[3], NumberStyles.Integer, CultureInfo.InvariantCulture, out prefix))
        {
            EmitGiveFailure(args, $"Invalid prefix '{args.Parameters[3]}'.");
            return;
        }

        if (prefix is < 0 or > byte.MaxValue)
        {
            EmitGiveFailure(args, $"Invalid prefix '{args.Parameters[3]}'.");
            return;
        }

        var target = matches[0];
        var template = new Item();
        template.netDefaults(itemId);
        if (template.type == 0)
        {
            EmitGiveFailure(args, $"Unknown item id '{itemId.ToString(CultureInfo.InvariantCulture)}'.");
            return;
        }

        template.stack = amount;
        template.prefix = (byte)prefix;

        var itemName = NonEmpty(template.Name) ?? itemId.ToString(CultureInfo.InvariantCulture);

        // Plan the entire placement before touching anything. BuildPlacementPlan returns
        // null when the full amount does not fit, so we either apply a complete plan or
        // mutate nothing at all.
        List<PlannedPlacement> plan;
        try
        {
            plan = BuildPlacementPlan(target.TPlayer, template, amount, out var capacity);
            if (plan.Count == 0)
            {
                var missing = amount - capacity;
                EmitGiveFailure(
                    args,
                    $"Inventory is full. Need room for {missing.ToString(CultureInfo.InvariantCulture)} more {itemName} and try again.");
                return;
            }
        }
        catch (Exception ex)
        {
            EmitGiveFailure(args, $"Failed to give item: {ex.Message}");
            return;
        }

        try
        {
            ApplyPlacementPlan(target, template, plan);
        }
        catch (Exception ex)
        {
            EmitGiveFailure(args, $"Failed to give item: {ex.Message}");
            return;
        }

        var json = JsonSerializer.Serialize(new
        {
            success = true,
            delivered = amount,
            method = "inventory",
            item = itemName
        });
        args.Player.SendInfoMessage($"TAKARO_GIVE {json}");
    }

    // One slot's worth of a planned delivery: put Amount more of the item into Slot.
    // IsEmptySlot distinguishes "top up an existing stack" from "occupy a fresh slot",
    // which decides whether the apply step reuses the slot's Item or builds a new one.
    private readonly record struct PlannedPlacement(int Slot, int Amount, bool IsEmptySlot);

    // Builds the complete list of slot writes needed to deliver `amount` of `template`,
    // or an empty list when the inventory cannot take the whole amount. `capacity` always
    // receives the real capacity so the caller can report the exact shortfall.
    //
    // Slot range: main inventory only, indices 0..49. Verified against the Terraria
    // assembly's own constants — Main.InventoryItemSlotsStart = 0,
    // Main.InventoryItemSlotsCount = 50, Main.InventoryCoinSlotsStart = 50,
    // Main.InventoryAmmoSlotsStart = 54, Main.InventorySlotsTotal = 58 (index 58 is the
    // mouse slot).
    //
    // Coin slots (50..53) and ammo slots (54..57) are deliberately EXCLUDED, so capacity
    // is a deliberate under-estimate for coins and ammo. Those slots accept items only
    // under extra conditions (Item.IsACoin for coins; Item.FitsAmmoSlot plus
    // Item.CanFillEmptyAmmoSlot for ammo, which excludes bait, paints/coatings and a
    // hardcoded item list). Counting them would mean reimplementing that gating exactly,
    // and any drift would make capacity over-report — leaving a tail with nowhere to go,
    // which is the drop we are trying to eliminate. Under-counting can only produce a
    // refusal, which is safe and visible; over-counting produces a lost order. A player
    // with a full main inventory but free ammo slots is told to make room, and an ammo
    // give still succeeds normally whenever the main inventory has space.
    private static List<PlannedPlacement> BuildPlacementPlan(Player? player, Item template, int amount, out int capacity)
    {
        capacity = 0;
        var plan = new List<PlannedPlacement>();

        var inventory = player?.inventory;
        if (inventory is null)
        {
            return plan;
        }

        var lastSlot = Math.Min(MainInventorySlotStart + MainInventorySlotCount, inventory.Length);

        // Pass 1: top up partial stacks of the same item. Item.CanStack compares type and
        // prefix, so a prefixed give will not merge into an unprefixed stack (and returns
        // false for an empty slot, whose type is 0). Doing this pass first keeps the
        // inventory tidy and leaves empty slots free for anything that still does not fit.
        for (var slot = MainInventorySlotStart; slot < lastSlot; slot++)
        {
            var existing = inventory[slot];
            if (existing is null || existing.type <= 0 || existing.stack <= 0)
            {
                continue;
            }

            if (!Item.CanStack(existing, template))
            {
                continue;
            }

            var headroom = existing.maxStack - existing.stack;
            if (headroom <= 0)
            {
                continue;
            }

            capacity += headroom;

            var remaining = amount - PlannedTotal(plan);
            if (remaining > 0)
            {
                plan.Add(new PlannedPlacement(slot, Math.Min(headroom, remaining), IsEmptySlot: false));
            }
        }

        // Pass 2: empty slots, each of which can hold a full maxStack of the new item.
        for (var slot = MainInventorySlotStart; slot < lastSlot; slot++)
        {
            var existing = inventory[slot];
            if (existing is not null && existing.type > 0 && existing.stack > 0)
            {
                continue;
            }

            var slotCapacity = template.maxStack;
            if (slotCapacity <= 0)
            {
                continue;
            }

            capacity += slotCapacity;

            var remaining = amount - PlannedTotal(plan);
            if (remaining > 0)
            {
                plan.Add(new PlannedPlacement(slot, Math.Min(slotCapacity, remaining), IsEmptySlot: true));
            }
        }

        // All-or-nothing: an incomplete plan is discarded entirely so no caller can
        // accidentally apply a partial delivery.
        if (PlannedTotal(plan) < amount)
        {
            plan.Clear();
        }

        return plan;
    }

    private static int PlannedTotal(List<PlannedPlacement> plan)
    {
        var total = 0;
        foreach (var placement in plan)
        {
            total += placement.Amount;
        }

        return total;
    }

    // Applies a plan that BuildPlacementPlan already proved complete, then syncs every
    // touched slot to the clients.
    //
    // Sync convention verified against the assemblies rather than assumed. TShock's own
    // TSPlayer.SendItemSlotPacketFor does exactly:
    //     NetMessage.SendData(5, Index, -1, null, Index, slot, prefix)
    // and NetMessage.SendData's packet-5 writer confirms the argument meanings:
    //     packetWriter.Write((byte)number);    // owning player index
    //     packetWriter.Write((short)number2);  // inventory slot index
    //     ... then stack, prefix and type are read from
    //     Main.player[number].inventory[slot] itself, not from the arguments.
    // So the packet carries the server-side slot contents; number3 is only a bit flag in
    // the trailing BitsByte (TShock passes the prefix there, which the writer ignores as a
    // prefix), so we pass 0.
    //
    // Note TSPlayer.SendData does NOT take number6/number7 — its signature is
    // SendData(PacketTypes, string text, int number, float number2, float number3,
    // float number4, int number5) — so we call NetMessage.SendData directly, matching
    // TShock. remoteClient: -1 broadcasts to every client, which is what other players
    // need to render the change; passing target.Index would send to that client only.
    private static void ApplyPlacementPlan(TSPlayer target, Item template, List<PlannedPlacement> plan)
    {
        var inventory = target.TPlayer.inventory;

        foreach (var placement in plan)
        {
            if (placement.IsEmptySlot)
            {
                var fresh = new Item();
                fresh.netDefaults(template.type);
                fresh.stack = placement.Amount;
                if (template.prefix != 0)
                {
                    fresh.prefix = template.prefix;
                }

                inventory[placement.Slot] = fresh;
            }
            else
            {
                inventory[placement.Slot].stack += placement.Amount;
            }
        }

        // Sync after every write lands, so a client never sees a half-applied inventory.
        foreach (var placement in plan)
        {
            NetMessage.SendData(
                (int)PacketTypes.PlayerSlot,
                -1,
                -1,
                null,
                target.Index,
                placement.Slot,
                0f);
        }
    }

    private static void EmitGiveFailure(CommandArgs args, string reason)
    {
        var json = JsonSerializer.Serialize(new
        {
            success = false,
            delivered = 0,
            method = "none",
            reason
        });
        args.Player.SendInfoMessage($"TAKARO_GIVE {json}");
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
            attacker = ResolveDeathAttacker(args.PlayerDeathReason),
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

    // Resolves who or what killed the player, as a Takaro player-shaped DTO.
    //
    // PlayerDeathReason.TryGetCausingEntity is the public accessor Terraria uses to
    // build its own death text ("... by Demon Eye"), so the killer is available even
    // though the plugin previously dropped it. It already unwraps a projectile back
    // to its owning NPC or player, so no separate projectile probe is needed.
    //
    // Returns null for a fall, drowning or lava death - those genuinely have no
    // attacker, and inventing one would be worse than reporting none.
    private static object? ResolveDeathAttacker(Terraria.DataStructures.PlayerDeathReason reason)
    {
        try
        {
            if (!reason.TryGetCausingEntity(out var entity) || entity is null)
            {
                return null;
            }

            if (entity is Player player)
            {
                var killer = ActivePlayerByIndex(player.whoAmI);
                return killer is not null
                    ? PlayerDto(killer, killer.Name)
                    : PlayerDto(null, NonEmpty(player.name) ?? $"player:{player.whoAmI}");
            }

            if (entity is NPC npc)
            {
                return new
                {
                    gameId = $"npc:{npc.whoAmI}",
                    name = NonEmpty(npc.GivenOrTypeName) ?? NonEmpty(npc.FullName) ?? $"NPC {npc.type}",
                    platformId = $"terraria:npc:{npc.whoAmI}",
                    type = npc.type,
                    netId = npc.netID,
                    boss = npc.boss
                };
            }
        }
        catch
        {
            // A malformed death reason must never take the event down; the death
            // itself still carries the reason text.
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
