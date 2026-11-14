using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using Newtonsoft.Json.Linq;
using TakaroEmpyrion.Protocol;

/// <summary>Reconnect policy (gettakaro/connectors#858): fast first retry, streak backoff, 60 s cap, dead-link detection.</summary>
public static partial class ContractTests
{
    static readonly TimeSpan Cap = TimeSpan.FromSeconds(60);

    static void ReconnectDelays()
    {
        // First retry after a stable session: 0.05-0.35 s, jittered.
        TimeSpan lo = ReconnectPolicy.Delay(0, 0, Cap), hi = ReconnectPolicy.Delay(0, 0.999999, Cap);
        Check(lo.TotalMilliseconds >= 50 && hi.TotalMilliseconds <= 350 && hi > lo, "RECONNECT first retry is 0.05-0.35 s with jitter", lo + " .. " + hi);

        // Streak: failure n waits within [2^n/2, 2^n] s, never past the cap.
        bool doubling = true;
        for (int n = 1; n <= 5; n++)
        {
            double ceil = Math.Pow(2, n);
            double a = ReconnectPolicy.Delay(n, 0, Cap).TotalSeconds, b = ReconnectPolicy.Delay(n, 0.999999, Cap).TotalSeconds;
            if (Math.Abs(a - ceil / 2) > 0.001 || b > ceil || b < ceil * 0.99) doubling = false;
        }
        Check(doubling, "RECONNECT consecutive failures back off exponentially (1-2 s, 2-4 s, ... 16-32 s)");
        bool capped = true;
        foreach (int n in new[] { 6, 7, 10, 16, 50, int.MaxValue })
        {
            TimeSpan d = ReconnectPolicy.Delay(n, 0.999999, Cap);
            if (d > Cap || d < TimeSpan.FromSeconds(59)) capped = false;
        }
        Check(capped && ReconnectPolicy.Delay(6, 0, Cap) == TimeSpan.FromSeconds(30), "RECONNECT backoff caps at 60 s (30-60 s jitter window at the cap)");
        Check(ReconnectPolicy.Delay(3, 0.25, Cap) != ReconnectPolicy.Delay(3, 0.75, Cap), "RECONNECT jitter spreads the wait");
        Check(ReconnectPolicy.Delay(9, 0.5, TimeSpan.FromSeconds(10)) <= TimeSpan.FromSeconds(10), "RECONNECT honours a lower MaxBackoff");

        // Streak bookkeeping.
        var p = new ReconnectPolicy(TimeSpan.FromSeconds(10), Cap);
        TimeSpan w1 = p.OnConnectionEnded(TimeSpan.FromMinutes(30), 0.5);
        TimeSpan w2 = p.OnConnectionEnded(null, 0.5);
        TimeSpan w3 = p.OnConnectionEnded(null, 0.5);
        TimeSpan w4 = p.OnConnectionEnded(TimeSpan.FromSeconds(3), 0.5);
        Check(w1 < TimeSpan.FromSeconds(1) && w2 >= TimeSpan.FromSeconds(1) && w3 > w2 && w4 > w3,
              "RECONNECT stable drop is fast; refused attempts and a 3 s session extend the streak", w1 + " " + w2 + " " + w3 + " " + w4);
        TimeSpan w5 = p.OnConnectionEnded(TimeSpan.FromSeconds(10), 0.5);
        Check(w5 < TimeSpan.FromSeconds(1) && p.Failures == 1, "RECONNECT streak resets after a connection stayed identified for StableAfter");
    }

    sealed class TimedFactory
    {
        public readonly BlockingCollection<FakeSocket> Sockets = new BlockingCollection<FakeSocket>();
        public readonly List<DateTime> Attempts = new List<DateTime>();
        public volatile int Refuse;
        public IWsConnection Create()
        {
            lock (Attempts) Attempts.Add(DateTime.UtcNow);
            if (Refuse > 0) { Refuse--; return new RefusingSocket(); }
            var s = new FakeSocket { AutoPong = true }; Sockets.Add(s); return s;
        }
    }

    sealed class RefusingSocket : IWsConnection
    {
        public void Connect(Uri uri, TimeSpan timeout) { throw new IOException("Connection refused"); }
        public void Send(string text) { throw new IOException("closed"); }
        public string Receive() { return null; }
        public void Abort() { }
        public void Dispose() { }
    }

    static FakeSocket Identify(TimedFactory f, int timeoutMs = 4000)
    {
        FakeSocket s;
        if (!f.Sockets.TryTake(out s, timeoutMs)) return null;
        if (Next(s, o => (string)o["type"] == "identify") == null) return null;
        s.ToConnector.Add("{\"type\":\"identifyResponse\",\"payload\":{\"gameServerId\":\"gs1\"}}");
        return s;
    }

    static void ReconnectWire()
    {
        var log = new ConsoleLog();
        var f = new TimedFactory();
        var client = new TakaroClient(f.Create,
            new ClientOptions
            {
                Url = "wss://fake/", IdentityToken = "id", ServerName = "Dev",
                PingInterval = TimeSpan.FromMilliseconds(200), InboundTimeout = TimeSpan.FromMilliseconds(1500),
                StableAfter = TimeSpan.FromMilliseconds(300),
            },
            log, _ => { });
        client.Start();
        FakeSocket s1 = Identify(f);
        Check(s1 != null, "RECONNECT-WIRE first connection identified");
        if (s1 == null) { client.Stop(); return; }
        System.Threading.Thread.Sleep(500); // stable

        // Mid-session reset: the next socket comes within the fast window, and an event raised in the gap survives.
        var sw = Stopwatch.StartNew();
        s1.ToConnector.Add("<close>");
        System.Threading.Thread.Sleep(20);
        client.SendGameEvent("log", new JObject { ["msg"] = "raised during the gap" });
        FakeSocket s2 = Identify(f, 3000);
        Check(s2 != null && sw.ElapsedMilliseconds < 1200, "RECONNECT-WIRE stable connection reset -> re-identified in under 1.2 s", sw.ElapsedMilliseconds + " ms");
        if (s2 == null) { client.Stop(); return; }
        Check(Next(s2, o => (string)o["type"] == "gameEvent" && (string)o["payload"]["data"]["msg"] == "raised during the gap") != null,
              "RECONNECT-WIRE event raised while disconnected is delivered after the reconnect");

        // Half-open: the peer goes silent (pings unanswered, no RST). The client closes it within the inbound timeout and comes back fast.
        System.Threading.Thread.Sleep(400);
        while (s2.FromConnector.TryTake(out _)) { }
        s2.Silent = true;
        sw.Restart();
        FakeSocket s3 = Identify(f, 6000);
        long detect = sw.ElapsedMilliseconds;
        Check(s3 != null && s2.Aborted && detect >= 1100 && detect < 3500, "RECONNECT-WIRE silent link closed after the inbound timeout and re-identified fast", detect + " ms");
        if (s3 == null) { client.Stop(); return; }

        // Refused attempts back off: gaps grow (0.05-0.35 s, then 1-2 s, then 2-4 s) and the client recovers on its own.
        System.Threading.Thread.Sleep(500);
        int before; lock (f.Attempts) before = f.Attempts.Count;
        f.Refuse = 2;
        s3.ToConnector.Add("<close>");
        FakeSocket s4 = Identify(f, 12000);
        List<DateTime> at; lock (f.Attempts) at = f.Attempts.Skip(before).ToList();
        bool grows = at.Count == 3 && (at[1] - at[0]) >= TimeSpan.FromSeconds(0.95) && (at[2] - at[1]) >= TimeSpan.FromSeconds(1.95);
        Check(s4 != null && grows, "RECONNECT-WIRE refused attempts back off and the client recovers once accepted",
              string.Join(", ", at.Select((t, i) => i == 0 ? "0" : ((t - at[i - 1]).TotalMilliseconds.ToString("0") + "ms"))));
        client.Stop();
    }

    /// <summary>Pings stuck behind a backlog of frames on a slow uplink are not "unanswered": the healthy socket stays.</summary>
    static void ReconnectSlowUplink()
    {
        var f = new TimedFactory();
        var client = new TakaroClient(f.Create,
            new ClientOptions
            {
                Url = "wss://fake/", IdentityToken = "id", ServerName = "Dev",
                PingInterval = TimeSpan.FromMilliseconds(200), InboundTimeout = TimeSpan.FromMilliseconds(1500),
                StableAfter = TimeSpan.FromMilliseconds(300),
            },
            new ConsoleLog(), _ => { });
        client.Start();
        FakeSocket s = Identify(f);
        if (s == null) { Check(false, "RECONNECT-SLOW first connection identified"); client.Stop(); return; }
        System.Threading.Thread.Sleep(300);
        s.SendDelayMs = 700;
        for (int i = 0; i < 5; i++) client.SendGameEvent("log", new JObject { ["msg"] = "burst " + i });
        System.Threading.Thread.Sleep(4500); // 5 x 0.7 s of sends: inbound is silent for > 1.5 s while pings wait in the queue
        Check(!s.Aborted && f.Sockets.Count == 0, "RECONNECT-SLOW pings queued behind a slow send do not close a healthy socket");
        s.SendDelayMs = 0;
        client.Stop();
    }

    /// <summary>A connect that never returns, even after Abort (what the game's Mono did on a refused reconnect).</summary>
    sealed class HangingSocket : IWsConnection
    {
        public volatile bool Aborted, Disposed;
        readonly ManualResetEvent _never = new ManualResetEvent(false);
        public void Connect(Uri uri, TimeSpan timeout) { _never.WaitOne(); }
        public void Send(string text) { throw new IOException("not connected"); }
        public string Receive() { return null; }
        public void Abort() { Aborted = true; }
        public void Dispose() { Disposed = true; }
    }

    sealed class HangingFactory
    {
        public readonly TimedFactory Inner = new TimedFactory();
        public readonly List<HangingSocket> Hung = new List<HangingSocket>();
        public volatile int Hang;
        public IWsConnection Create()
        {
            if (Hang > 0)
            {
                Hang--;
                lock (Inner.Attempts) Inner.Attempts.Add(DateTime.UtcNow);
                var h = new HangingSocket(); lock (Hung) Hung.Add(h); return h;
            }
            return Inner.Create();
        }
    }

    static ClientOptions StuckOptions()
    {
        return new ClientOptions
        {
            Url = "wss://fake/", IdentityToken = "id", ServerName = "Dev",
            PingInterval = TimeSpan.FromMilliseconds(200), InboundTimeout = TimeSpan.FromMilliseconds(1500),
            StableAfter = TimeSpan.FromMilliseconds(300), MaxBackoff = TimeSpan.FromSeconds(1),
            ConnectDeadline = TimeSpan.FromMilliseconds(300), ConnectGrace = TimeSpan.FromMilliseconds(200),
        };
    }

    /// <summary>
    /// A refused reconnect whose connect never returns (gettakaro/connectors, Empyrion 0.2.1 smoke): each attempt is
    /// abandoned at the deadline, the normal backoff schedules the next one, and the event raised meanwhile arrives.
    /// </summary>
    static void ReconnectStuckConnect()
    {
        var f = new HangingFactory();
        var client = new TakaroClient(f.Create, StuckOptions(), new ConsoleLog(), _ => { });
        client.Start();
        FakeSocket s1 = Identify(f.Inner);
        Check(s1 != null, "CONNECT-DEADLINE first connection identified");
        if (s1 == null) { client.Stop(); return; }
        Thread.Sleep(500);

        int before; lock (f.Inner.Attempts) before = f.Inner.Attempts.Count;
        f.Hang = 3;
        var sw = Stopwatch.StartNew();
        s1.ToConnector.Add("<close>");
        Thread.Sleep(50);
        client.SendGameEvent("chat-message", new JObject { ["msg"] = "typed while the connect hung" });
        FakeSocket s2 = Identify(f.Inner, 8000);
        long took = sw.ElapsedMilliseconds;
        List<DateTime> at; lock (f.Inner.Attempts) at = f.Inner.Attempts.Skip(before).ToList();
        string gaps = string.Join(", ", at.Select((t, i) => i == 0 ? "0" : ((t - at[i - 1]).TotalMilliseconds.ToString("0") + "ms")));
        Check(s2 != null && at.Count == 4, "CONNECT-DEADLINE three connects that never return are abandoned and the fourth attempt identifies", took + " ms; attempts " + gaps);
        // Each abandoned attempt costs the deadline (0.5 s) plus the backoff (0.5-1 s at this cap).
        bool spaced = at.Count == 4 && Enumerable.Range(1, 3).All(i => (at[i] - at[i - 1]) >= TimeSpan.FromMilliseconds(900));
        Check(spaced, "CONNECT-DEADLINE abandoned attempts follow the backoff (no tight loop)", gaps);
        bool aborted; lock (f.Hung) aborted = f.Hung.Count == 3 && f.Hung.All(h => h.Aborted);
        Check(aborted, "CONNECT-DEADLINE every abandoned socket is aborted");
        Check(client.AbandonedConnects == 3, "CONNECT-DEADLINE abandoned attempts are counted while their thread is stuck", client.AbandonedConnects.ToString());
        if (s2 != null)
            Check(Next(s2, o => (string)o["type"] == "gameEvent" && (string)o["payload"]["data"]["msg"] == "typed while the connect hung") != null,
                  "CONNECT-DEADLINE event raised during the stuck connects is delivered after the reconnect");
        client.Stop();
    }

    /// <summary>Stop during a connect that never returns: Stop returns at once and no further attempt starts.</summary>
    static void StopDuringStuckConnect()
    {
        var f = new HangingFactory { Hang = 1000 };
        ClientOptions o = StuckOptions();
        o.ConnectDeadline = TimeSpan.FromSeconds(30); // the stop must not wait for the deadline
        var client = new TakaroClient(f.Create, o, new ConsoleLog(), _ => { });
        client.Start();
        Thread.Sleep(300);
        var sw = Stopwatch.StartNew();
        client.Stop();
        long stopMs = sw.ElapsedMilliseconds;
        Thread.Sleep(1500);
        int attempts; lock (f.Inner.Attempts) attempts = f.Inner.Attempts.Count;
        bool aborted; lock (f.Hung) aborted = f.Hung.Count == 1 && f.Hung[0].Aborted;
        Check(stopMs < 100 && attempts == 1 && aborted, "CONNECT-DEADLINE Stop during a stuck connect returns at once, aborts it and starts no new attempt",
              "stop " + stopMs + " ms, attempts " + attempts + ", aborted " + aborted);
    }

    /// <summary>ClientWsConnection waits on ConnectAsync with a timeout instead of trusting its cancellation token.</summary>
    static void ConnectTaskBounded()
    {
        var never = new TaskCompletionSource<bool>();
        var sw = Stopwatch.StartNew();
        bool finished = ClientWsConnection.WaitBounded(never.Task, TimeSpan.FromMilliseconds(200));
        Check(!finished && sw.ElapsedMilliseconds >= 180 && sw.ElapsedMilliseconds < 1000, "CONNECT-DEADLINE a ConnectAsync task that never completes is given up after the timeout", sw.ElapsedMilliseconds + " ms");
        var failed = new TaskCompletionSource<bool>();
        failed.SetException(new System.Net.WebSockets.WebSocketException("Unable to connect to the remote server"));
        string thrown = null;
        try { ClientWsConnection.WaitBounded(failed.Task, TimeSpan.FromSeconds(1)); } catch (Exception e) { thrown = e.GetType().Name; }
        Check(thrown == "WebSocketException", "CONNECT-DEADLINE a refused connect surfaces its own exception", thrown ?? "none");
        var ok = new TaskCompletionSource<bool>(); ok.SetResult(true);
        Check(ClientWsConnection.WaitBounded(ok.Task, TimeSpan.FromSeconds(1)), "CONNECT-DEADLINE a completed connect returns true");
    }
}
