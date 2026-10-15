using CounterStrikeSharp.API;
using CounterStrikeSharp.API.Core;
using CounterStrikeSharp.API.Modules.Utils;
using CounterStrikeSharp.API.ValveConstants.Protobuf;
using Takaro.Cs2.Core;

namespace Takaro.Cs2.Plugin;

public sealed partial class Cs2ServerAdapter
{
    private Cs2BanStore? bans;
    private Cs2PlayerStore? playerStore;

    public void SetBanStore(Cs2BanStore store) => bans = store;
    public void SetPlayerStore(Cs2PlayerStore store) => playerStore = store;

    public Task<TakaroActionResult> GetPlayerLocationAsync(string identifier, CancellationToken cancellationToken = default) =>
        RunOnServerAsync(() =>
        {
            var target = FindController(identifier);
            var point = target is { PawnIsAlive: true } ? target.PlayerPawn.Value?.AbsOrigin : null;
            return point is null
                ? TakaroActionResult.Fail("position_unavailable", "Player is offline or has no spawned pawn.")
                : TakaroActionResult.Ok(new { x = point.X, y = point.Y, z = point.Z });
        }, cancellationToken);

    public Task<TakaroActionResult> GetPlayerInventoryAsync(string identifier, CancellationToken cancellationToken = default) =>
        RunOnServerAsync(() =>
        {
            var services = FindController(identifier)?.PlayerPawn.Value?.WeaponServices;
            if (services is null)
                return TakaroActionResult.Fail("inventory_unavailable", "Player is offline or has no weapon services.");
            var items = services.MyWeapons.Select(handle => handle.Value)
                .Where(weapon => weapon is { IsValid: true })
                .Select(weapon => new { code = weapon!.DesignerName, name = Cs2ItemCatalog.Name(weapon.DesignerName), amount = 1 })
                .ToArray();
            return TakaroActionResult.Ok(items);
        }, cancellationToken);

    public Task<TakaroActionResult> ListItemsAsync(CancellationToken cancellationToken = default) =>
        Task.FromResult(TakaroActionResult.Ok(Cs2ItemCatalog.All.Select(item => new { code = item.code, name = item.name }).ToArray()));

    public async Task<TakaroActionResult> GiveItemAsync(string identifier, string code, int amount, CancellationToken cancellationToken = default)
    {
        var before = await RunOnServerAsync(() =>
        {
            if (!Cs2ItemCatalog.Contains(code))
                return TakaroActionResult.Fail("unknown_item", "Item code is not in the supported CS2 catalog.");
            if (amount != 1)
                return TakaroActionResult.Fail("invalid_amount", "CS2 weapons are individual equipped items; this action delivers one.");
            var target = FindController(identifier);
            if (target is not { PawnIsAlive: true } || target.PlayerPawn.Value?.WeaponServices is not { } services)
                return TakaroActionResult.Fail("player_not_ready", "Player is offline or has no spawned pawn.");
            var count = CountItem(target, code);
            var slot = Cs2ItemCatalog.FirearmSlot(code);
            string? replacedCode = null;
            if (slot is not null)
            {
                var occupied = services.MyWeapons.Select(handle => handle.Value)
                    .FirstOrDefault(weapon => weapon is { IsValid: true } && Cs2ItemCatalog.FirearmSlot(weapon.DesignerName) == slot);
                if (occupied is not null)
                {
                    if (occupied.DesignerName == code)
                        return TakaroActionResult.Fail("slot_occupied", "Player already has this weapon in its CS2 equipment slot.");
                    replacedCode = occupied.DesignerName;
                    target.PlayerPawn.Value.RemovePlayerItem(occupied);
                    occupied.Remove();
                }
            }
            if (target.GiveNamedItem(code) == IntPtr.Zero)
            {
                if (replacedCode is not null) target.GiveNamedItem(replacedCode);
                return TakaroActionResult.Fail("delivery_failed", "CS2 refused to create the requested item.");
            }
            return TakaroActionResult.Ok(new DeliveryBefore(count, replacedCode));
        }, cancellationToken);
        if (!before.Success) return before;
        return await RunOnServerAsync(() =>
        {
            var target = FindController(identifier);
            var previous = (DeliveryBefore)before.Payload!;
            if (target is not null && CountItem(target, code) > previous.Count)
                return TakaroActionResult.Ok();
            if (target is not null && previous.ReplacedCode is not null) target.GiveNamedItem(previous.ReplacedCode);
            return TakaroActionResult.Fail("delivery_failed", "CS2 did not add the requested item to equipped inventory.");
        }, cancellationToken);
    }

    public Task<TakaroActionResult> TeleportPlayerAsync(string identifier, TakaroPosition position, CancellationToken cancellationToken = default) =>
        RunOnServerAsync(() =>
        {
            var target = FindController(identifier);
            var pawn = target is { PawnIsAlive: true } ? target.PlayerPawn.Value : null;
            if (pawn?.AbsOrigin is null)
                return TakaroActionResult.Fail("position_unavailable", "Player is offline or has no spawned pawn.");
            pawn.Teleport(new Vector((float)position.X, (float)position.Y, (float)position.Z));
            var actual = pawn.AbsOrigin;
            return actual is not null && Math.Abs(actual.X - position.X) < 2 && Math.Abs(actual.Y - position.Y) < 2 && Math.Abs(actual.Z - position.Z) < 2
                ? TakaroActionResult.Ok()
                : TakaroActionResult.Fail("teleport_failed", "CS2 did not place the pawn at the requested position.");
        }, cancellationToken);

    public Task<TakaroActionResult> KickPlayerAsync(string identifier, string? reason, CancellationToken cancellationToken = default) =>
        RunOnServerAsync(() =>
        {
            var target = FindController(identifier);
            if (target is null) return TakaroActionResult.Fail("player_not_found", "Player is not online and authenticated.");
            target.Disconnect(NetworkDisconnectionReason.NETWORK_DISCONNECT_KICKED);
            return TakaroActionResult.Ok();
        }, cancellationToken);

    public Task<TakaroActionResult> BanPlayerAsync(string identifier, string? reason, string? expiresAt, CancellationToken cancellationToken = default) =>
        RunOnServerAsync(() =>
        {
            var steamId = SteamId(identifier);
            if (steamId is null) return TakaroActionResult.Fail("invalid_player", "A SteamID64 is required for a durable ban.");
            DateTimeOffset? expiry = null;
            if (expiresAt is not null)
            {
                if (!DateTimeOffset.TryParse(expiresAt, out var parsed) || parsed <= DateTimeOffset.UtcNow)
                    return TakaroActionResult.Fail("invalid_expiry", "expiresAt must be a future ISO timestamp.");
                expiry = parsed;
            }
            var target = FindController(identifier);
            if (bans is null) return TakaroActionResult.Fail("ban_store_unavailable", "Ban store is not loaded.");
            bans.Add(steamId, target?.PlayerName ?? steamId, reason ?? "Takaro ban", expiry);
            target?.Disconnect(NetworkDisconnectionReason.NETWORK_DISCONNECT_KICKBANADDED);
            return TakaroActionResult.Ok();
        }, cancellationToken);

    public Task<TakaroActionResult> UnbanPlayerAsync(string identifier, CancellationToken cancellationToken = default) =>
        Task.FromResult(bans?.Remove(SteamId(identifier) ?? identifier) == true
            ? TakaroActionResult.Ok() : TakaroActionResult.Fail("ban_not_found", "No active ban for this SteamID64."));

    public Task<TakaroActionResult> ListBansAsync(CancellationToken cancellationToken = default) =>
        Task.FromResult(bans is null
            ? TakaroActionResult.Fail("ban_store_unavailable", "Ban store is not loaded.")
            : TakaroActionResult.Ok(bans.Active().Select(ban => new
            {
                player = new { gameId = ban.SteamId64, name = ban.Name, steamId = ban.SteamId64, platformId = $"steam:{ban.SteamId64}" },
                reason = ban.Reason,
                expiresAt = ban.ExpiresAt
            }).ToArray()));

    public Task<TakaroActionResult> ShutdownAsync(CancellationToken cancellationToken = default)
    {
        if (!config.AllowShutdown)
            return Task.FromResult(TakaroActionResult.Fail("shutdown_disabled", "Shutdown is disabled by default."));
        _ = Task.Run(async () =>
        {
            await Task.Delay(500, CancellationToken.None);
            Server.NextWorldUpdate(() => Server.ExecuteCommand("quit"));
        });
        return Task.FromResult(TakaroActionResult.Ok());
    }

    private static CCSPlayerController? FindController(string identifier) =>
        Utilities.GetPlayers().FirstOrDefault(player =>
            ToCs2Player(player) is { } mapped && PlayerMapper.Find([PlayerMapper.ToTakaroPlayer(mapped)], identifier) is not null);

    private static int CountItem(CCSPlayerController player, string code) =>
        player.PlayerPawn.Value?.WeaponServices?.MyWeapons.Count(handle =>
            handle.Value is { IsValid: true } weapon && weapon.DesignerName == code) ?? 0;

    private sealed record DeliveryBefore(int Count, string? ReplacedCode);

    private static string? SteamId(string identifier)
    {
        var value = identifier.StartsWith("steam:", StringComparison.OrdinalIgnoreCase) ? identifier[6..] : identifier;
        return ulong.TryParse(value, out var id) && id >= 76561197960265728UL ? value : null;
    }
}

internal static class Cs2ItemCatalog
{
    public static readonly (string code, string name)[] All =
    [
        ("weapon_ak47", "AK-47"), ("weapon_m4a1", "M4A4"),
        ("weapon_awp", "AWP"), ("weapon_deagle", "Desert Eagle"),
        ("weapon_glock", "Glock-18"), ("weapon_usp_silencer", "USP-S"),
        ("weapon_flashbang", "Flashbang"), ("weapon_smokegrenade", "Smoke Grenade"),
        ("weapon_hegrenade", "High Explosive Grenade"), ("weapon_molotov", "Molotov")
    ];

    public static bool Contains(string code) => All.Any(item => item.code == code);
    public static string Name(string code) => All.FirstOrDefault(item => item.code == code).name ?? code;
    public static string? FirearmSlot(string code) => code switch
    {
        "weapon_ak47" or "weapon_m4a1" or "weapon_awp" => "primary",
        "weapon_deagle" or "weapon_glock" or "weapon_usp_silencer" or "weapon_hkp2000" or "weapon_p250" => "secondary",
        _ => null
    };
}
