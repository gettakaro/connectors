using System.Text.Json;
using Microsoft.VisualStudio.TestTools.UnitTesting;
using Takaro.Valheim.Core;

namespace Takaro.Valheim.Core.Tests;

[TestClass]
public sealed class MainThreadActionSchedulerTests
{
    [TestMethod]
    public async Task BackgroundDispatchWaitsForDrainAndExecutesExactlyOnceOnTheDrainingThread()
    {
        using var scheduler = new QueuedMainThreadActionScheduler(capacity: 4);
        var adapter = new ThreadRecordingAdapter();
        var dispatcher = new TakaroRequestDispatcher(adapter, scheduler);
        using var args = JsonDocument.Parse("""{"gameId":"Steam_1","item":"Wood","amount":1}""");

        var responseTask = Task.Run(() => dispatcher.DispatchAsync(
            new TakaroRequest("give-main-thread", "giveItem", args.RootElement)));

        Assert.IsTrue(
            SpinWait.SpinUntil(() => scheduler.PendingCount == 1, TimeSpan.FromSeconds(2)),
            "The background request was not queued deterministically.");
        Assert.AreEqual(0, adapter.InvocationCount);
        Assert.IsFalse(responseTask.IsCompleted, "A response must not complete before scheduled adapter work runs.");

        var drainingThread = Environment.CurrentManagedThreadId;
        Assert.AreEqual(1, scheduler.Drain());
        var response = await responseTask;

        Assert.IsTrue(response.Success);
        Assert.AreEqual(1, adapter.InvocationCount);
        Assert.AreEqual(drainingThread, adapter.InvocationThreadId);
        Assert.AreEqual(0, scheduler.PendingCount);
    }

    [TestMethod]
    public async Task ScheduledExceptionsFaultTheCallerWithoutASecondInvocation()
    {
        using var scheduler = new QueuedMainThreadActionScheduler(capacity: 2);
        var invocations = 0;
        var scheduled = scheduler.ScheduleAsync<int>(() =>
        {
            invocations++;
            throw new InvalidOperationException("scheduled failure");
        });

        Assert.AreEqual(1, scheduler.Drain());
        var exception = await Assert.ThrowsExceptionAsync<InvalidOperationException>(() => scheduled);

        Assert.AreEqual("scheduled failure", exception.Message);
        Assert.AreEqual(1, invocations);
    }

    [TestMethod]
    public async Task CancellationBeforeDrainCompletesWithoutInvokingTheAction()
    {
        using var scheduler = new QueuedMainThreadActionScheduler(capacity: 2);
        using var cancellation = new CancellationTokenSource();
        var invocations = 0;
        var scheduled = scheduler.ScheduleAsync(() => ++invocations, cancellation.Token);

        cancellation.Cancel();

        await Assert.ThrowsExceptionAsync<TaskCanceledException>(() => scheduled);
        Assert.AreEqual(1, scheduler.Drain(), "The canceled queue entry should be consumed by the bounded drain.");
        Assert.AreEqual(0, invocations);
    }

    [TestMethod]
    public async Task BoundedQueueRejectsOverflowAndDisposeCompletesPendingCallers()
    {
        var scheduler = new QueuedMainThreadActionScheduler(capacity: 1);
        var pending = scheduler.ScheduleAsync(() => 1);
        var overflow = scheduler.ScheduleAsync(() => 2);

        var overflowError = await Assert.ThrowsExceptionAsync<InvalidOperationException>(() => overflow);
        StringAssert.Contains(overflowError.Message, "capacity");

        scheduler.Dispose();
        await Assert.ThrowsExceptionAsync<ObjectDisposedException>(() => pending);
        Assert.AreEqual(0, scheduler.PendingCount);
    }

    [TestMethod]
    public async Task InlineSchedulerPreservesExistingPureUnitTestBehavior()
    {
        var invocationThread = 0;

        var result = await InlineMainThreadActionScheduler.Instance.ScheduleAsync(() =>
        {
            invocationThread = Environment.CurrentManagedThreadId;
            return 42;
        });

        Assert.AreEqual(42, result);
        Assert.AreEqual(Environment.CurrentManagedThreadId, invocationThread);
    }

    private sealed class ThreadRecordingAdapter : IValheimTakaroAdapter
    {
        private int invocationCount;

        public int InvocationCount => Volatile.Read(ref invocationCount);
        public int InvocationThreadId { get; private set; }

        public Task<TakaroActionResult> GiveItemAsync(
            string identifier,
            string itemCode,
            int amount,
            string? quality,
            CancellationToken cancellationToken = default)
        {
            Interlocked.Increment(ref invocationCount);
            InvocationThreadId = Environment.CurrentManagedThreadId;
            return Task.FromResult(TakaroActionResult.Ok(new { dropped = true }));
        }

        public Task<TakaroActionResult> TestReachabilityAsync(CancellationToken cancellationToken = default) => Unexpected();
        public Task<TakaroActionResult> GetPlayersAsync(CancellationToken cancellationToken = default) => Unexpected();
        public Task<TakaroActionResult> GetPlayerAsync(string identifier, CancellationToken cancellationToken = default) => Unexpected();
        public Task<TakaroActionResult> GetPlayerLocationAsync(string identifier, CancellationToken cancellationToken = default) => Unexpected();
        public Task<TakaroActionResult> GetPlayerInventoryAsync(string identifier, CancellationToken cancellationToken = default) => Unexpected();
        public Task<TakaroActionResult> SendMessageAsync(string message, string? recipientIdentifier, string? senderNameOverride, CancellationToken cancellationToken = default) => Unexpected();
        public Task<TakaroActionResult> ExecuteConsoleCommandAsync(string command, CancellationToken cancellationToken = default) => Unexpected();
        public Task<TakaroActionResult> ListItemsAsync(CancellationToken cancellationToken = default) => Unexpected();
        public Task<TakaroActionResult> ListEntitiesAsync(CancellationToken cancellationToken = default) => Unexpected();
        public Task<TakaroActionResult> ListLocationsAsync(CancellationToken cancellationToken = default) => Unexpected();
        public Task<TakaroActionResult> GetMapInfoAsync(CancellationToken cancellationToken = default) => Unexpected();
        public Task<TakaroActionResult> GetMapTileAsync(CancellationToken cancellationToken = default) => Unexpected();
        public Task<TakaroActionResult> TeleportPlayerAsync(string identifier, TakaroPosition position, CancellationToken cancellationToken = default) => Unexpected();
        public Task<TakaroActionResult> KickPlayerAsync(string identifier, string? reason, CancellationToken cancellationToken = default) => Unexpected();
        public Task<TakaroActionResult> BanPlayerAsync(string identifier, string? reason, CancellationToken cancellationToken = default) => Unexpected();
        public Task<TakaroActionResult> UnbanPlayerAsync(string identifier, CancellationToken cancellationToken = default) => Unexpected();
        public Task<TakaroActionResult> ListBansAsync(CancellationToken cancellationToken = default) => Unexpected();
        public Task<TakaroActionResult> ShutdownAsync(CancellationToken cancellationToken = default) => Unexpected();

        private static Task<TakaroActionResult> Unexpected() =>
            throw new AssertFailedException("Only giveItem should be invoked in this scheduler test.");
    }
}
