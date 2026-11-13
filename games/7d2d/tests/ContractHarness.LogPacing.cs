using System;
using System.Collections.Generic;
using Newtonsoft.Json;
using Newtonsoft.Json.Linq;
using Takaro.Services;
using Takaro.WebSocket;

// Takaro drops `log` events above 100 per 5 s per server (event-rate-limited). The
// backlog after a boot or an outage is paced below that; nothing is dropped, logs
// keep their order and responses and other events are not held behind them.
public static partial class ContractHarness
{
    private sealed class PacedLedger
    {
        public long Now = 1000L * TimeSpan.TicksPerSecond;
        public readonly OutboundLedger Ledger;
        public readonly List<OutboundLedger.Entry> Written = new List<OutboundLedger.Entry>();

        public PacedLedger(int budget = 3, int windowSeconds = 5, int maxPending = 100)
        {
            Ledger = new OutboundLedger(
                maxPending,
                maxPending,
                "stream-pacing",
                budget,
                TimeSpan.FromSeconds(windowSeconds),
                () => Now
            );
        }

        public void Log(string json) => Ledger.Enqueue(json, true, false, true);

        public void Event(string json) => Ledger.Enqueue(json, true, false);

        public void Response(string json) => Ledger.Enqueue(json, false, false);

        public void Advance(double seconds) => Now += (long)(seconds * TimeSpan.TicksPerSecond);

        // What FlushPending does on one poll: write until nothing may go.
        public int Flush()
        {
            int count = 0;
            for (OutboundLedger.Entry next; (next = Ledger.NextToSend()) != null; count++)
            {
                Ledger.MarkWritten(next);
                Written.Add(next);
            }
            return count;
        }

        // Confirms every written frame, as an eventAck covering them would.
        public void Confirm()
        {
            long upTo = 0;
            foreach (OutboundLedger.Entry e in Written)
                upTo = Math.Max(upTo, e.Seq);
            Ledger.AcknowledgeUpTo("stream-pacing", upTo);
        }

        // Runs the sender in 0.1 s steps for at most the given seconds. Each written
        // line reaches Takaro (and is confirmed) delay(writtenAt) seconds later.
        // Returns the arrival times in arrival order.
        public List<long> Deliver(Func<long, double> delay, int seconds)
        {
            var onTheWay = new List<KeyValuePair<long, OutboundLedger.Entry>>();
            var arrivals = new List<long>();
            long lastArrival = 0;
            for (int step = 0; step < seconds * 10; step++)
            {
                for (OutboundLedger.Entry next; (next = Ledger.NextToSend()) != null; )
                {
                    Ledger.MarkWritten(next);
                    Written.Add(next);
                    // One TCP stream: nothing overtakes what was written before it.
                    lastArrival = Math.Max(
                        lastArrival,
                        Now + (long)(delay(Now) * TimeSpan.TicksPerSecond)
                    );
                    onTheWay.Add(new KeyValuePair<long, OutboundLedger.Entry>(lastArrival, next));
                }
                Advance(0.1);
                long upTo = 0;
                while (onTheWay.Count > 0 && onTheWay[0].Key <= Now)
                {
                    arrivals.Add(onTheWay[0].Key);
                    upTo = Math.Max(upTo, onTheWay[0].Value.Seq);
                    onTheWay.RemoveAt(0);
                }
                if (upTo > 0)
                    Ledger.AcknowledgeUpTo("stream-pacing", upTo);
                if (Ledger.PendingCount == 0 && onTheWay.Count == 0)
                    break;
            }
            return arrivals;
        }

        public List<string> WrittenJson()
        {
            return Written.ConvertAll(e => e.Json);
        }
    }

    private static void FamilyLogPacingMessageClassification()
    {
        WebSocketTransport.Instance.TerminalMessages.Clear();
        GameEventPublisher.SendLogEvent("a log line");
        GameEventPublisher.SendGameEvent(
            GameEventTypes.ChatMessage,
            new Dictionary<string, object> { { "msg", "hi" } }
        );
        True(WebSocketTransport.Instance.TerminalMessages[0].IsLogEvent(), "a log event is paced");
        True(
            !WebSocketTransport.Instance.TerminalMessages[1].IsLogEvent(),
            "a chat event is not paced"
        );
        True(
            !WebSocketMessage
                .CreateResponse("r1", new Dictionary<string, object> { { "type", "log" } })
                .IsLogEvent(),
            "a response is never a log event, whatever its payload"
        );
        WebSocketTransport.Instance.TerminalMessages.Clear();
    }

    private static void FamilyLogPacingKeepsTheBudgetAndDropsNothing()
    {
        // A 500-line backlog at the real budget over a network that delivers and
        // confirms each line 0.2 s after it is written.
        var rig = new PacedLedger(
            OutboundLedger.DefaultLogBudget,
            (int)OutboundLedger.DefaultLogWindow.TotalSeconds,
            500
        );
        for (int i = 0; i < 500; i++)
            rig.Log("log" + i);
        List<long> arrivals = rig.Deliver(_ => 0.2, 120);

        Equal(500, rig.Written.Count, "every log line of a 500-line backlog is written");
        for (int i = 0; i < 500; i++)
            Equal("log" + i, rig.Written[i].Json, "log lines keep their order");
        AssertTakaroLogLimits(arrivals, "steady link");
        long span = arrivals[arrivals.Count - 1] - arrivals[0];
        True(
            span < 40 * TimeSpan.TicksPerSecond,
            $"a 500-line backlog drains in under 40 s ({span / TimeSpan.TicksPerSecond} s)"
        );
    }

    private static void FamilyLogPacingSurvivesLinesBunchingOnTheWay()
    {
        // A large response (listItems takes seconds to send) holds the log lines
        // written behind it, which then reach Takaro together. Lines on the way
        // keep their place in the budget, so the bunch never exceeds it.
        var rig = new PacedLedger(
            OutboundLedger.DefaultLogBudget,
            (int)OutboundLedger.DefaultLogWindow.TotalSeconds,
            500
        );
        for (int i = 0; i < 400; i++)
            rig.Log("log" + i);
        // Everything written between 3 s and 8 s arrives at 8 s; later, a slow link.
        long origin = rig.Now;
        List<long> arrivals = rig.Deliver(
            written =>
            {
                double at = (double)(written - origin) / TimeSpan.TicksPerSecond;
                if (at >= 3 && at < 8)
                    return 8 - at;
                return at >= 20 && at < 30 ? 2.5 : 0.1;
            },
            200
        );
        Equal(400, arrivals.Count, "every line arrives");
        AssertTakaroLogLimits(arrivals, "bunching link");
        True(
            rig.Ledger.NextToSend() == null && rig.Ledger.PendingCount == 0,
            "nothing is left behind"
        );
    }

    // Takaro counts log events per server when they arrive: 100 per fixed 5 s
    // bucket and 600 per 30 s. Check every bucket alignment, not just one.
    private static void AssertTakaroLogLimits(List<long> arrivals, string link)
    {
        for (int i = 0; i < arrivals.Count; i++)
        {
            int in5 = 0,
                in30 = 0;
            for (int j = i; j < arrivals.Count; j++)
            {
                long gap = arrivals[j] - arrivals[i];
                if (gap < 5 * TimeSpan.TicksPerSecond)
                    in5++;
                if (gap < 30 * TimeSpan.TicksPerSecond)
                    in30++;
            }
            True(in5 <= 80, $"{link}: at most 80 log lines reach Takaro in any 5 s ({in5})");
            True(in30 <= 600, $"{link}: at most 600 log lines reach Takaro in any 30 s ({in30})");
        }
    }

    private static void FamilyLogPacingLetsResponsesAndEventsPass()
    {
        var rig = new PacedLedger(budget: 3);
        for (int i = 0; i < 6; i++)
            rig.Log("log" + i);
        rig.Response("resp1");
        rig.Event("chat1");
        rig.Log("log6");
        rig.Event("chat2");

        rig.Flush();
        Equal(
            "log0,log1,log2,resp1,chat1,chat2",
            string.Join(",", rig.WrittenJson()),
            "over budget, the response and the events go past the parked log lines in their own order"
        );
        Equal(4, rig.Ledger.PendingCount, "the parked log lines are kept");

        rig.Advance(5);
        Equal(0, rig.Flush(), "unconfirmed log lines keep their place however long ago they went");
        rig.Confirm();
        Equal(0, rig.Flush(), "confirmed ones keep it for the window after the confirmation");
        rig.Advance(5);
        rig.Flush();
        rig.Confirm();
        rig.Advance(5);
        rig.Flush();
        Equal(
            "log0,log1,log2,resp1,chat1,chat2,log3,log4,log5,log6",
            string.Join(",", rig.WrittenJson()),
            "the parked log lines follow in order once the window allows"
        );

        var seqs = new List<long>();
        foreach (OutboundLedger.Entry e in rig.Written)
            if (e.Replayable)
                seqs.Add(e.Seq);
        for (int i = 1; i < seqs.Count; i++)
            True(seqs[i] > seqs[i - 1], "event numbers rise in write order");
        Equal(0L, rig.Written[3].Seq, "a response carries no number");

        // Takaro's stream ledger keeps the highest number seen and drops the rest.
        long watermark = 0;
        int accepted = 0;
        foreach (long seq in seqs)
        {
            if (seq > watermark)
            {
                watermark = seq;
                accepted++;
            }
        }
        Equal(9, accepted, "Takaro accepts every event: none arrives below the watermark");
    }

    private static void FamilyLogPacingHoldsEventsBehindANumberedLogLine()
    {
        // Takaro drops an event numbered at or below the highest it has seen, so
        // once a parked log line has a number (picked, then the write failed),
        // no event may pass it; responses still may.
        var rig = new PacedLedger(budget: 1);
        rig.Ledger.RequeueInFlight(1, false);
        rig.Log("logA");
        rig.Log("logB");
        rig.Flush();
        Equal(2L, rig.Ledger.PeekHead().Seq, "the parked line got its number");

        rig.Event("chat");
        rig.Response("resp");
        rig.Flush();
        Equal(
            "logA,resp",
            string.Join(",", rig.WrittenJson()),
            "the response passes, the chat event waits behind the numbered log line"
        );

        rig.Confirm();
        rig.Advance(5);
        rig.Flush();
        Equal(
            "logA,resp,logB,chat",
            string.Join(",", rig.WrittenJson()),
            "the numbered log line goes with its number, then the event"
        );
        Equal(3L, rig.Written[3].Seq, "the event after it is numbered higher");
    }

    private static void FamilyLogPacingRequeuedLinesFitTheBudget()
    {
        // Lines that were on a socket that died give their place back: they go
        // again on the next socket without waiting for the window.
        var rig = new PacedLedger(budget: 2);
        rig.Ledger.RequeueInFlight(1, false);
        rig.Log("logA");
        rig.Log("logB");
        rig.Log("logC");
        rig.Flush();
        Equal(2, rig.Ledger.InFlightCount, "two lines are on the wire, unconfirmed");
        Equal(2, rig.Ledger.RequeueInFlight(2, false), "the socket dies: both are requeued");
        rig.Flush();
        Equal(
            "logA,logB,logA,logB",
            string.Join(",", rig.WrittenJson()),
            "the requeued lines go again at once, with their numbers"
        );
        Equal(1L, rig.Written[2].Seq, "a resent line keeps its number");
        Equal(1, rig.Ledger.PendingCount, "the third line still waits for the budget");
    }

    private static void FamilyLogPacingAckPingIsNotHeldBehindLogs()
    {
        // v0: a pong confirms events written before its ping. With only parked log
        // lines left the sender still needs to send that ping, so NextToSend must
        // report "nothing sendable" and then return the ping.
        var rig = new PacedLedger(budget: 1);
        rig.Ledger.RequeueInFlight(1, true);
        rig.Log("log0");
        rig.Log("log1");
        rig.Flush();
        True(rig.Ledger.NeedsAckPing, "the written log line needs a confirming ping");
        True(rig.Ledger.NextToSend() == null, "only an over-budget log line is left");
        rig.Ledger.Enqueue("{\"type\":\"ping\"}", false, true);
        OutboundLedger.Entry ping = rig.Ledger.NextToSend();
        True(ping != null && ping.IsPing, "the ping goes past the parked log line");
        rig.Ledger.MarkWritten(ping);
        Equal(1, rig.Ledger.AcknowledgePong(), "its pong confirms the written log line");
        Equal(1, rig.Ledger.PendingCount, "the parked log line is still queued");
    }

    private static void FamilyLogPacingWakesTheSenderForParkedLines()
    {
        var rig = new PacedLedger(budget: 2, windowSeconds: 1);
        Equal(
            -1,
            rig.Ledger.MillisecondsUntilLogBudget(),
            "nothing parked: the sender sleeps its normal poll"
        );
        rig.Log("log0");
        Equal(0, rig.Ledger.MillisecondsUntilLogBudget(), "a log line within budget may go now");
        rig.Log("log1");
        rig.Log("log2");
        rig.Flush();
        Equal(1, rig.Ledger.PendingCount, "the third line is parked");
        Equal(
            100,
            rig.Ledger.MillisecondsUntilLogBudget(),
            "while the budget is held by unconfirmed lines the sender polls for the confirmation"
        );
        rig.Confirm();
        rig.Advance(0.25);
        Equal(
            750,
            rig.Ledger.MillisecondsUntilLogBudget(),
            "once confirmed, the sender wakes when the oldest confirmation leaves the window"
        );
        rig.Advance(0.75);
        Equal(0, rig.Ledger.MillisecondsUntilLogBudget(), "then the parked line may go");
        Equal(1, rig.Flush(), "and goes");
    }

    private static void FamilyLogPacingDropKeepsTheRest()
    {
        // A poison frame picked past parked log lines is dropped alone.
        var rig = new PacedLedger(budget: 0);
        rig.Log("log0");
        rig.Event("poison");
        OutboundLedger.Entry next = rig.Ledger.NextToSend();
        Equal("poison", next.Json, "the event is picked past the parked log line");
        rig.Ledger.Drop(next);
        Equal(1, rig.Ledger.PendingCount, "only the dropped frame is gone");
        Equal("log0", rig.Ledger.PeekHead().Json, "the log line stays queued");
    }
}
