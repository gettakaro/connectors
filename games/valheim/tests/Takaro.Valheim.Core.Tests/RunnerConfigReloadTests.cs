using System.Collections.Concurrent;
using System.Net;
using System.Net.Sockets;
using System.Net.WebSockets;
using System.Text;
using System.Text.Json;
using Microsoft.VisualStudio.TestTools.UnitTesting;
using Takaro.Valheim.Core;
using Takaro.Valheim.Plugin;

namespace Takaro.Valheim.Core.Tests;

/// <summary>
/// The runner against a fake Takaro: no connection without a token, a loud banner for a missing or
/// rejected token, and a saved token change reconnecting at once instead of after the backoff.
/// </summary>
[TestClass]
public sealed class RunnerConfigReloadTests
{
    private const string ConfigPath = "/srv/valheim/BepInEx/plugins/TakaroValheim/takaro.cfg";
    private static readonly TimeSpan Wait = TimeSpan.FromSeconds(10);

    [TestMethod]
    public async Task MissingTokenWaitsWithABannerAndAPastedTokenConnects()
    {
        using var takaro = new FakeTakaro();
        var logs = new Log();
        using var runner = Runner(takaro, "", "id-1", logs);
        await runner.StartAsync();

        await Until(() => logs.Warnings.Any(w => w.Contains("registrationToken not set", StringComparison.Ordinal)));
        var banner = logs.Warnings.First(w => w.Contains("registrationToken not set", StringComparison.Ordinal));
        StringAssert.Contains(banner, "*****");
        StringAssert.Contains(banner, ConfigPath);
        StringAssert.Contains(banner, "no restart needed");
        await Task.Delay(500);
        Assert.AreEqual(0, takaro.Identifies.Count, "No connection without a token.");

        runner.UpdateSettings(Config(takaro, "good", "id-1"));

        await Until(() => logs.Infos.Any(l => l.Contains("identified as gameServerId=gs-id-1", StringComparison.Ordinal)));
        Assert.AreEqual("good", takaro.Identifies.First().RegistrationToken);
    }

    [TestMethod]
    public async Task RejectedTokenShowsABannerAndACorrectedTokenSkipsTheBackoff()
    {
        using var takaro = new FakeTakaro();
        var logs = new Log();
        // A 30 s first backoff: only skipping it can connect within the wait below.
        using var runner = Runner(takaro, "bad", "id-1", logs, backoffUnit: TimeSpan.FromSeconds(15));
        await runner.StartAsync();

        await Until(() => logs.Warnings.Any(w => w.Contains("Takaro rejected this server", StringComparison.Ordinal)));
        var banner = logs.Warnings.First(w => w.Contains("Takaro rejected this server", StringComparison.Ordinal));
        StringAssert.Contains(banner, "Invalid registrationToken provided");
        StringAssert.Contains(banner, ConfigPath);
        StringAssert.Contains(banner, "*****");

        var fixedAt = DateTime.UtcNow;
        runner.UpdateSettings(Config(takaro, "good", "id-1"));

        await Until(() => logs.Infos.Any(l => l.Contains("identified as gameServerId=gs-id-1", StringComparison.Ordinal)));
        Assert.IsTrue(DateTime.UtcNow - fixedAt < TimeSpan.FromSeconds(10));
        Assert.AreEqual("good", takaro.Identifies.Last().RegistrationToken);
    }

    [TestMethod]
    public async Task ConflictOnTheSecondIdentifyOfANewServerIsNotARejection()
    {
        using var takaro = new FakeTakaro(conflictOnSecondIdentify: true);
        var logs = new Log();
        using var runner = Runner(takaro, "good", "new-id", logs);
        await runner.StartAsync();

        await Until(() => takaro.Identifies.Count >= 2 && logs.Infos.Any(l => l.Contains("409", StringComparison.Ordinal)));
        await Task.Delay(1500);

        Assert.IsTrue(logs.Infos.Any(l => l.Contains("identified as gameServerId=gs-new-id", StringComparison.Ordinal)));
        Assert.IsFalse(logs.Warnings.Any(w => w.Contains("rejected", StringComparison.Ordinal)));
        Assert.AreEqual(2, takaro.Identifies.Count, "The connection is kept, no reconnect.");
    }

    [TestMethod]
    public async Task IdentityChangeOnALiveConnectionReconnectsWithTheNewIdentity()
    {
        using var takaro = new FakeTakaro();
        var logs = new Log();
        using var runner = Runner(takaro, "good", "id-1", logs);
        await runner.StartAsync();
        await Until(() => logs.Infos.Any(l => l.Contains("gs-id-1", StringComparison.Ordinal)));

        runner.UpdateSettings(Config(takaro, "good", "id-2"));

        await Until(() => logs.Infos.Any(l => l.Contains("gs-id-2", StringComparison.Ordinal)));
        Assert.AreEqual("id-2", takaro.Identifies.Last().IdentityToken);
        await Until(() => takaro.Closed >= 1);
    }

    [TestMethod]
    public async Task UnchangedConnectionSettingsDoNotReconnect()
    {
        using var takaro = new FakeTakaro();
        var logs = new Log();
        var config = Config(takaro, "good", "id-1");
        using var runner = new TakaroWebSocketRunner(config, new IdleAdapter(), logs.Infos.Enqueue, null, logs.Warnings.Enqueue, ConfigPath);
        await runner.StartAsync();
        await Until(() => logs.Infos.Any(l => l.Contains("gs-id-1", StringComparison.Ordinal)));
        var identifies = takaro.Identifies.Count;

        runner.UpdateSettings(config with { ChatSenderName = "Odin" });
        await Task.Delay(1000);

        Assert.AreEqual(identifies, takaro.Identifies.Count);
    }

    [TestMethod]
    public async Task SettingsChangesRacingDisposeAreHarmless()
    {
        using var takaro = new FakeTakaro();
        var logs = new Log();
        var runner = Runner(takaro, "good", "id-1", logs);
        await runner.StartAsync();

        var flips = Task.Run(() =>
        {
            for (var i = 0; i < 200; i++)
            {
                runner.UpdateSettings(Config(takaro, i % 2 == 0 ? "good" : "", "id-" + (i % 3)));
            }
        });
        await Task.Delay(50);
        runner.Dispose();
        await flips;
        runner.UpdateSettings(Config(takaro, "good", "id-9"));
        await Task.Delay(300);

        Assert.IsFalse(runner.IsRunning && takaro.Identifies.Any(i => i.IdentityToken == "id-9"));
    }

    private static TakaroWebSocketRunner Runner(FakeTakaro takaro, string token, string identity, Log logs, TimeSpan? backoffUnit = null) =>
        new(Config(takaro, token, identity), new IdleAdapter(), logs.Infos.Enqueue, null, logs.Warnings.Enqueue, ConfigPath, backoffUnit);

    private static ConnectorConfig Config(FakeTakaro takaro, string token, string identity) =>
        ConnectorConfig.FromDictionary(new Dictionary<string, string>
        {
            ["registrationToken"] = token,
            ["identityToken"] = identity,
            ["takaroWsUrl"] = takaro.Url
        });

    private static async Task Until(Func<bool> condition)
    {
        var deadline = DateTime.UtcNow + Wait;
        while (!condition())
        {
            if (DateTime.UtcNow > deadline)
            {
                Assert.Fail("Timed out waiting for the runner.");
            }

            await Task.Delay(50);
        }
    }

    private sealed class Log
    {
        public ConcurrentQueue<string> Infos { get; } = new();
        public ConcurrentQueue<string> Warnings { get; } = new();
    }

    private sealed record Identify(string RegistrationToken, string IdentityToken);

    /// <summary>Answers identify like Takaro: an error for token "bad", else a game server id.</summary>
    private sealed class FakeTakaro : IDisposable
    {
        private readonly HttpListener listener = new();
        private readonly CancellationTokenSource stop = new();
        private readonly ConcurrentQueue<Identify> identifies = new();
        private int closed;

        private readonly bool conflictOnSecondIdentify;

        public FakeTakaro(bool conflictOnSecondIdentify = false)
        {
            this.conflictOnSecondIdentify = conflictOnSecondIdentify;
            var probe = new TcpListener(IPAddress.Loopback, 0);
            probe.Start();
            var port = ((IPEndPoint)probe.LocalEndpoint).Port;
            probe.Stop();
            listener.Prefixes.Add($"http://127.0.0.1:{port}/");
            listener.Start();
            Url = $"ws://127.0.0.1:{port}/";
            _ = Task.Run(AcceptAsync);
        }

        public string Url { get; }

        public IReadOnlyList<Identify> Identifies => identifies.ToArray();

        public int Closed => Volatile.Read(ref closed);

        public void Dispose()
        {
            stop.Cancel();
            listener.Close();
        }

        private async Task AcceptAsync()
        {
            while (!stop.IsCancellationRequested)
            {
                HttpListenerContext context;
                try
                {
                    context = await listener.GetContextAsync();
                }
                catch (Exception)
                {
                    return;
                }

                _ = Task.Run(() => ServeAsync(context));
            }
        }

        private async Task ServeAsync(HttpListenerContext context)
        {
            try
            {
                var socket = (await context.AcceptWebSocketAsync(null)).WebSocket;
                var buffer = new byte[16 * 1024];
                var identifiesOnSocket = 0;
                if (conflictOnSecondIdentify)
                {
                    await socket.SendAsync(Encoding.UTF8.GetBytes("""{"type":"connected"}"""), WebSocketMessageType.Text, true, stop.Token);
                }

                while (socket.State == WebSocketState.Open && !stop.IsCancellationRequested)
                {
                    var result = await socket.ReceiveAsync(new ArraySegment<byte>(buffer), stop.Token);
                    if (result.MessageType == WebSocketMessageType.Close)
                    {
                        break;
                    }

                    using var frame = JsonDocument.Parse(Encoding.UTF8.GetString(buffer, 0, result.Count));
                    var type = frame.RootElement.GetProperty("type").GetString();
                    string? reply = type switch
                    {
                        "ping" => """{"type":"pong"}""",
                        "identify" => IdentifyReply(frame.RootElement.GetProperty("payload"), ++identifiesOnSocket),
                        _ => null
                    };
                    if (reply is not null)
                    {
                        await socket.SendAsync(Encoding.UTF8.GetBytes(reply), WebSocketMessageType.Text, true, stop.Token);
                    }
                }
            }
            catch (Exception)
            {
            }
            finally
            {
                Interlocked.Increment(ref closed);
            }
        }

        private string IdentifyReply(JsonElement payload, int onSocket)
        {
            var identify = new Identify(
                payload.GetProperty("registrationToken").GetString()!,
                payload.GetProperty("identityToken").GetString()!);
            identifies.Enqueue(identify);
            if (conflictOnSecondIdentify && onSocket == 2)
            {
                return """{"type":"identifyResponse","payload":{"error":{"message":"Request failed with status code 409"}}}""";
            }

            return identify.RegistrationToken == "bad"
                ? """{"type":"identifyResponse","payload":{"error":{"message":"Invalid registrationToken provided"}}}"""
                : "{\"type\":\"identifyResponse\",\"payload\":{\"gameServerId\":\"gs-" + identify.IdentityToken + "\"}}";
        }
    }

    private sealed class IdleAdapter : IValheimTakaroAdapter
    {
        private static Task<TakaroActionResult> Unused() => Task.FromResult(TakaroActionResult.Error("unused", "unused"));

        public Task<TakaroActionResult> TestReachabilityAsync(CancellationToken cancellationToken = default) => Unused();
        public Task<TakaroActionResult> GetPlayersAsync(CancellationToken cancellationToken = default) => Task.FromResult(TakaroActionResult.Ok(Array.Empty<TakaroPlayer>()));
        public Task<TakaroActionResult> GetPlayerAsync(string identifier, CancellationToken cancellationToken = default) => Unused();
        public Task<TakaroActionResult> GetPlayerLocationAsync(string identifier, CancellationToken cancellationToken = default) => Unused();
        public Task<TakaroActionResult> GetPlayerInventoryAsync(string identifier, CancellationToken cancellationToken = default) => Unused();
        public Task<TakaroActionResult> GiveItemAsync(string identifier, string itemCode, int amount, string? quality, CancellationToken cancellationToken = default) => Unused();
        public Task<TakaroActionResult> SendMessageAsync(string message, string? recipientIdentifier, string? senderNameOverride, CancellationToken cancellationToken = default) => Unused();
        public Task<TakaroActionResult> ExecuteConsoleCommandAsync(string command, CancellationToken cancellationToken = default) => Unused();
        public Task<TakaroActionResult> ListItemsAsync(CancellationToken cancellationToken = default) => Unused();
        public Task<TakaroActionResult> ListEntitiesAsync(CancellationToken cancellationToken = default) => Unused();
        public Task<TakaroActionResult> ListLocationsAsync(CancellationToken cancellationToken = default) => Unused();
        public Task<TakaroActionResult> GetMapInfoAsync(CancellationToken cancellationToken = default) => Unused();
        public Task<TakaroActionResult> GetMapTileAsync(CancellationToken cancellationToken = default) => Unused();
        public Task<TakaroActionResult> TeleportPlayerAsync(string identifier, TakaroPosition position, CancellationToken cancellationToken = default) => Unused();
        public Task<TakaroActionResult> KickPlayerAsync(string identifier, string? reason, CancellationToken cancellationToken = default) => Unused();
        public Task<TakaroActionResult> BanPlayerAsync(string identifier, string? reason, CancellationToken cancellationToken = default) => Unused();
        public Task<TakaroActionResult> UnbanPlayerAsync(string identifier, CancellationToken cancellationToken = default) => Unused();
        public Task<TakaroActionResult> ListBansAsync(CancellationToken cancellationToken = default) => Unused();
        public Task<TakaroActionResult> ShutdownAsync(CancellationToken cancellationToken = default) => Unused();
    }
}
