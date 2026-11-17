using System;
using System.Collections.Concurrent;
using System.Diagnostics;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using Newtonsoft.Json.Linq;
using TakaroEmpyrion.Game;
using TakaroEmpyrion.Protocol;

public static partial class ContractTests
{
    sealed class PositionClock
    {
        long _ticks;
        public TimeSpan Now() { return TimeSpan.FromTicks(Interlocked.Read(ref _ticks)); }
        public void Set(double seconds) { Interlocked.Exchange(ref _ticks, TimeSpan.FromSeconds(seconds).Ticks); }
    }

    static bool PositionWait(Func<bool> ready, int timeoutMs = 3000)
    {
        var watch = Stopwatch.StartNew();
        while (!ready() && watch.ElapsedMilliseconds < timeoutMs) Thread.Sleep(5);
        return ready();
    }

    static FakeSocket PositionIdentify(BlockingCollection<FakeSocket> sockets, TakaroClient client)
    {
        if (!sockets.TryTake(out FakeSocket socket, 4000) || Next(socket, f => (string)f["type"] == "identify") == null)
            throw new Exception("position test connection did not identify");
        socket.ToConnector.Add("{\"type\":\"identifyResponse\",\"payload\":{\"gameServerId\":\"position-test\",\"protocolVersion\":1}}");
        if (!PositionWait(() => client.Identified && client.ProtocolVersion == 1)) throw new Exception("position test negotiation failed");
        return socket;
    }

    static void PositionFrame(FakeSocket socket, JObject frame)
    {
        socket.ToConnector.Add(frame.ToString());
        socket.ToConnector.Add("{\"type\":\"ping\"}");
        if (Next(socket, f => (string)f["type"] == "pong") == null) throw new Exception("position frame synchronization failed");
    }

    static void PositionAck(FakeSocket socket, JObject frame, long? upTo = null, string stream = null)
    {
        PositionFrame(socket, new JObject
        {
            ["type"] = "eventAck", ["payload"] = new JObject
            {
                ["streamId"] = stream ?? (string)frame["payload"]["streamId"],
                ["upTo"] = upTo ?? (long)frame["payload"]["seq"],
            }
        });
    }

    static JObject NextPosition(FakeSocket socket, int timeoutMs = 2000)
    {
        return Next(socket, f => (string)f["type"] == "gameEvent" && (string)f["payload"]?["type"] == "player-positions", timeoutMs);
    }

    static JArray PositionBatch(double x)
    {
        return new JArray(new JObject
        {
            ["gameId"] = "76561198000000001",
            ["position"] = new JObject { ["x"] = x, ["y"] = 2, ["z"] = 3, ["dimension"] = "Akua" }
        });
    }

    static void PositionDelivery()
    {
        var clock = new PositionClock();
        var sockets = new BlockingCollection<FakeSocket>();
        var log = new ConsoleLog();
        var client = new TakaroClient(() => { var s = new FakeSocket { AutoPong = true }; sockets.Add(s); return s; },
            new ClientOptions
            {
                Url = "wss://fake/", IdentityToken = "position-test", ServerName = "Position test",
                Manifest = () => Capabilities.Build(Actions.Declared, null), PositionClock = clock.Now,
                PingInterval = TimeSpan.FromMilliseconds(25), InboundTimeout = TimeSpan.FromSeconds(60),
                StableAfter = TimeSpan.Zero,
            }, log, _ => { });
        client.Start();
        try
        {
            FakeSocket first = PositionIdentify(sockets, client);
            int reads = 0;
            var publisher = new PositionPublisher(() => Task.FromResult(PositionBatch(++reads)), client, log);
            clock.Set(2.999);
            Check(!publisher.PublishOnce().Result && reads == 0, "POSITION initial connection waits declared interval before reading");
            clock.Set(3);
            Check(publisher.PublishOnce().Result, "POSITION first current snapshot queued");
            JObject one = NextPosition(first);
            Check(one != null && (long)one["payload"]["seq"] == 1 && (string)one["payload"]["streamId"] != client.StreamId
                && client.UnackedEvents == 0, "POSITION separate transient stream bypasses gameplay acknowledgement state");
            clock.Set(20);
            Check(!publisher.PublishOnce().Result && reads == 1, "POSITION delayed receipt acknowledgement blocks fresh reads");
            PositionAck(first, one, stream: client.StreamId);
            Check(!publisher.PublishOnce().Result && reads == 1, "POSITION gameplay acknowledgement cannot release position gate");
            PositionAck(first, one, upTo: 2);
            Check(!publisher.PublishOnce().Result && reads == 1, "POSITION future acknowledgement cannot release pending snapshot");
            PositionAck(first, one);
            clock.Set(22.999);
            Check(!publisher.PublishOnce().Result && reads == 1, "POSITION next interval starts at matching receipt acknowledgement");
            clock.Set(23);
            Check(publisher.PublishOnce().Result && reads == 2, "POSITION fresh read after acknowledgement plus three seconds");
            JObject two = NextPosition(first);
            clock.Set(24);
            PositionFrame(first, new JObject { ["type"] = "eventError", ["payload"] = new JObject
                { ["streamId"] = two["payload"]["streamId"], ["seq"] = two["payload"]["seq"] },
                ["error"] = new JObject { ["code"] = "test_refusal", ["message"] = "test" } });
            clock.Set(26.999);
            Check(!publisher.PublishOnce().Result, "POSITION rejected batch still waits one interval");
            clock.Set(27);
            Check(publisher.PublishOnce().Result, "POSITION rejection safely releases the transient gate");
            JObject three = NextPosition(first);

            client.SendGameEvent("log", new JObject { ["msg"] = "retained gameplay" });
            JObject durable = Next(first, f => (string)f["payload"]?["data"]?["msg"] == "retained gameplay");
            clock.Set(28); PositionAck(first, three); clock.Set(31);
            var oldRead = new TaskCompletionSource<JArray>();
            var newRead = new TaskCompletionSource<JArray>();
            int deferredReads = 0;
            var deferred = new PositionPublisher(() => ++deferredReads == 1 ? oldRead.Task : newRead.Task, client, log);
            Task<bool> oldPublish = deferred.PublishOnce();
            Check(!oldPublish.IsCompleted && deferredReads == 1, "POSITION reserves one asynchronous game read");
            first.ToConnector.Add("<close>");
            FakeSocket second = PositionIdentify(sockets, client);
            JObject replay = Next(second, f => (string)f["payload"]?["data"]?["msg"] == "retained gameplay");
            Check(replay != null && (string)replay["payload"]["streamId"] == (string)durable["payload"]["streamId"]
                && (long)replay["payload"]["seq"] == (long)durable["payload"]["seq"],
                "POSITION reconnect preserves unrelated gameplay stream and sequence");
            clock.Set(34);
            Task<bool> newPublish = deferred.PublishOnce();
            Check(!newPublish.IsCompleted && deferredReads == 2, "POSITION new connection can request a fresh snapshot");
            oldRead.SetResult(PositionBatch(999));
            Check(oldPublish.Wait(1000) && !oldPublish.Result, "POSITION read crossing reconnect is discarded");
            Check(!deferred.PublishOnce().Result && deferredReads == 2,
                "POSITION old completion cannot release a newer asynchronous read reservation");
            newRead.SetResult(PositionBatch(222));
            Check(newPublish.Wait(1000) && newPublish.Result, "POSITION current epoch read is accepted");
            JObject fresh = NextPosition(second);
            Check(fresh != null && (long)fresh["payload"]["seq"] == 1
                && (string)fresh["payload"]["streamId"] != (string)one["payload"]["streamId"]
                && (double)fresh["payload"]["data"]["positions"][0]["position"]["x"] == 222,
                "POSITION reconnect starts at sequence one with current coordinates");
            clock.Set(64);
            Check(PositionWait(() => second.Aborted), "POSITION missing acknowledgement reconnects after thirty seconds");
            FakeSocket third = PositionIdentify(sockets, client);
            // Replay is still durable; it must not be confused with a transient position batch.
            Next(third, f => (string)f["payload"]?["data"]?["msg"] == "retained gameplay");
            clock.Set(67);
            var oversized = new PositionPublisher(() => Task.FromResult(new JArray(Enumerable.Range(0, 1001)
                .Select(i => new JObject { ["gameId"] = i.ToString(), ["position"] = new JObject { ["x"] = i, ["y"] = 2, ["z"] = 3 } }))), client, log);
            Check(!oversized.PublishOnce().Result && NextPosition(third, 150) == null,
                "POSITION oversized snapshot is rejected whole without delayed partial batches");
            clock.Set(70);
            Check(publisher.PublishOnce().Result, "POSITION valid snapshot can follow an oversized rejection");
            JObject afterOversize = NextPosition(third);
            Check(afterOversize != null && (long)afterOversize["payload"]["seq"] == 1,
                "POSITION rejected oversize snapshot consumes no sequence");
            clock.Set(71); PositionAck(third, afterOversize); clock.Set(74);
            var stopRead = new TaskCompletionSource<JArray>();
            var stopped = new PositionPublisher(() => stopRead.Task, client, log);
            Task<bool> stopPublish = stopped.PublishOnce();
            stopped.Stop(); stopRead.SetResult(PositionBatch(444));
            Check(stopPublish.Wait(1000) && !stopPublish.Result && NextPosition(third, 150) == null,
                "POSITION stopped publisher discards its outstanding read");
        }
        finally { client.Stop(); }
    }
}
