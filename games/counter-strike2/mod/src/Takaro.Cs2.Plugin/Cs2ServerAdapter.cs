using CounterStrikeSharp.API;
using CounterStrikeSharp.API.Core;
using CounterStrikeSharp.API.Modules.Cvars;
using Takaro.Cs2.Core;

namespace Takaro.Cs2.Plugin;

public sealed partial class Cs2ServerAdapter : ICs2TakaroAdapter
{
    private readonly ConnectorConfig config;
    private readonly CommandAllowlist commandAllowlist;
    private Cs2MapAssets? mapAssets;

    public Cs2ServerAdapter(ConnectorConfig config)
    {
        this.config = config;
        commandAllowlist = config.CreateCommandAllowlist();
    }

    public void SetMapAssets(Cs2MapAssets assets) => mapAssets = assets;

    public async Task<TakaroActionResult> ListLocationsAsync(CancellationToken cancellationToken = default)
    {
        var map = await RunOnServerAsync(() => Server.MapName, cancellationToken);
        return await Task.Run(() => mapAssets?.ListLocations(map) ??
            TakaroActionResult.Fail("map_unavailable", "Map assets have not been configured."), cancellationToken);
    }

    public async Task<TakaroActionResult> GetMapInfoAsync(CancellationToken cancellationToken = default)
    {
        var map = await RunOnServerAsync(() => Server.MapName, cancellationToken);
        return await Task.Run(() => mapAssets?.GetInfo(map) ??
            TakaroActionResult.Ok(new { enabled = false, mapBlockSize = 0, maxZoom = 0, mapSizeX = 0, mapSizeY = 0, mapSizeZ = 0 }), cancellationToken);
    }

    public async Task<TakaroActionResult> GetMapTileAsync(int x, int y, int zoom, CancellationToken cancellationToken = default)
    {
        var map = await RunOnServerAsync(() => Server.MapName, cancellationToken);
        return await Task.Run(() => mapAssets?.GetTile(map, x, y, zoom) ??
            TakaroActionResult.Fail("map_unavailable", "Map assets have not been configured."), cancellationToken);
    }

    public async Task<TakaroActionResult> TestReachabilityAsync(CancellationToken cancellationToken = default)
    {
        var payload = await RunOnServerAsync(() => new
        {
            connectable = true,
            reason = (string?)null,
            map = Server.MapName,
            maxPlayers = Server.MaxPlayers
        }, cancellationToken);

        return TakaroActionResult.Ok(payload);
    }

    public async Task<IReadOnlyList<TakaroPlayer>> GetPlayersAsync(CancellationToken cancellationToken = default)
    {
        var players = await RunOnServerAsync<IReadOnlyList<TakaroPlayer>>(() =>
            PlayerMapper.ToTakaroPlayers(Utilities.GetPlayers().Select(ToCs2Player).OfType<Cs2Player>(), config.IncludeBots), cancellationToken);

        return players;
    }

    public async Task<TakaroPlayer?> GetPlayerAsync(string identifier, CancellationToken cancellationToken = default)
    {
        var players = await GetPlayersAsync(cancellationToken);
        return PlayerMapper.Find(players, identifier) ?? playerStore?.Find(identifier);
    }

    public async Task<TakaroActionResult> SendMessageAsync(string message, string? recipient, string? sender, CancellationToken cancellationToken = default)
    {
        return await RunOnServerAsync(() =>
        {
            var line = string.IsNullOrWhiteSpace(sender) ? message : $"{sender}: {message}";
            if (recipient is not null)
            {
                var target = FindController(recipient);
                if (target is null) return TakaroActionResult.Fail("player_not_found", "Recipient is not online and authenticated.");
                target.PrintToChat(line);
                return TakaroActionResult.Ok();
            }

            var players = Utilities.GetPlayers().Where(player => ToCs2Player(player) is not null).ToArray();
            if (players.Length == 0) return TakaroActionResult.Fail("no_recipients", "No authenticated player can receive chat.");
            foreach (var player in players) player.PrintToChat(line);
            return TakaroActionResult.Ok();
        }, cancellationToken);
    }

    public Task<TakaroActionResult> ExecuteConsoleCommandAsync(string command, CancellationToken cancellationToken = default)
    {
        if (!commandAllowlist.IsAllowed(command))
            return Task.FromResult(TakaroActionResult.Ok(new { success = false, rawResult = "", errorMessage = "Command is outside the configured allowlist." }));

        // CounterStrikeSharp cannot capture ExecuteCommand output. A read-only ConVar query
        // has an authoritative result from the server and never executes arbitrary text.
        if (command.Length == 0 || command.Any(character => !char.IsAsciiLetterOrDigit(character) && character != '_'))
            return Task.FromResult(TakaroActionResult.Ok(new { success = false, rawResult = "", errorMessage = "Only a single read-only ConVar name can return a console result." }));

        return RunOnServerAsync(() =>
        {
            var variable = ConVar.Find(command);
            return variable is null
                ? TakaroActionResult.Ok(new { success = false, rawResult = "", errorMessage = "The named ConVar does not exist." })
                : TakaroActionResult.Ok(new { success = true, rawResult = $"{variable.Name} = {variable.StringValue}", errorMessage = (string?)null });
        }, cancellationToken);
    }

    public static Cs2Player? ToCs2Player(CCSPlayerController? controller)
    {
        if (controller is null || !controller.IsValid || controller.IsHLTV)
        {
            return null;
        }

        var steamId = controller.AuthorizedSteamID?.SteamId64 ?? 0;
        if (steamId < 76561197960265728UL)
        {
            return null;
        }

        var ping = controller.Ping > int.MaxValue ? int.MaxValue : (int)controller.Ping;
        return new Cs2Player(
            Name: string.IsNullOrWhiteSpace(controller.PlayerName) ? steamId.ToString() : controller.PlayerName,
            SteamId64: steamId,
            Ip: controller.IpAddress,
            Ping: ping,
            IsBot: controller.IsBot);
    }

    private static Task RunOnServerAsync(Action action, CancellationToken cancellationToken)
    {
        return RunOnServerAsync(() =>
        {
            action();
            return true;
        }, cancellationToken);
    }

    private static Task<TResult> RunOnServerAsync<TResult>(Func<TResult> action, CancellationToken cancellationToken)
    {
        var completion = new TaskCompletionSource<TResult>(TaskCreationOptions.RunContinuationsAsynchronously);
        var registration = cancellationToken.Register(() => completion.TrySetCanceled(cancellationToken));

        Server.NextWorldUpdate(() =>
        {
            if (cancellationToken.IsCancellationRequested)
            {
                return;
            }

            try
            {
                completion.TrySetResult(action());
            }
            catch (Exception ex)
            {
                completion.TrySetException(ex);
            }
        });

        return completion.Task.ContinueWith(task =>
        {
            registration.Dispose();
            return task.GetAwaiter().GetResult();
        }, CancellationToken.None, TaskContinuationOptions.ExecuteSynchronously, TaskScheduler.Default);
    }
}
