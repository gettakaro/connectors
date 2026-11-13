using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Diagnostics;
using System.Threading;
using System.Threading.Tasks;
using Eleon.Modding;
using TakaroEmpyrion.Protocol;

namespace TakaroEmpyrion.Game
{
    public sealed class GameRequestException : Exception
    {
        public readonly ErrorType ErrorType;
        public GameRequestException(string message, ErrorType type = ErrorType.None) : base(message) { ErrorType = type; }
    }

    /// <summary>
    /// The only path from connector threads into the game. Work is queued and run inside Game_Update under a
    /// per-frame time budget (the game thread does nothing else for us). Legacy (v1) requests get a seqNr; the
    /// matching Game_Event resolves the Task. Continuations run asynchronously so they never execute on the
    /// game thread.
    /// </summary>
    public sealed class GameBridge
    {
        readonly ConcurrentQueue<Action> _work = new ConcurrentQueue<Action>();
        readonly ConcurrentDictionary<ushort, Pending> _pending = new ConcurrentDictionary<ushort, Pending>();
        ILog _log;
        readonly Stopwatch _sw = new Stopwatch();
        ModGameAPI _api;
        int _seq = 1000;

        public static readonly TimeSpan DefaultRequestTimeout = TimeSpan.FromSeconds(10);
        /// <summary>How long a v1 request waits for its reply event before it fails.</summary>
        public TimeSpan RequestTimeout = DefaultRequestTimeout;
        public double FrameBudgetMs = 2.0;
        public int MainThreadId { get; private set; }
        public long FramesSeen { get; private set; }

        /// <summary>Unsolicited events (connect, chat, statistics, ...): eventId, data. Called on the game thread.</summary>
        public event Action<CmdId, object> UnsolicitedEvent;

        sealed class Pending
        {
            public TaskCompletionSource<object> Tcs;
            public CmdId Request;
            public DateTime Deadline;
            public TimeSpan Timeout;
        }

        public GameBridge(ILog log) { _log = log; }

        public void SetLog(ILog log) { if (log != null) _log = log; }

        public void Attach(ModGameAPI api) { _api = api; }
        public bool Attached { get { return _api != null; } }

        /// <summary>Runs an action on the game thread and returns its result.</summary>
        public Task<T> OnGameThread<T>(Func<T> fn)
        {
            var tcs = new TaskCompletionSource<T>(TaskCreationOptions.RunContinuationsAsynchronously);
            _work.Enqueue(() =>
            {
                try { tcs.TrySetResult(fn()); }
                catch (Exception e) { tcs.TrySetException(e); }
            });
            return tcs.Task;
        }

        /// <summary>Sends a v1 request and completes with the reply event's data (or faults on Event_Error/timeout).</summary>
        public Task<object> Request(CmdId id, object data)
        {
            var tcs = new TaskCompletionSource<object>(TaskCreationOptions.RunContinuationsAsynchronously);
            if (_api == null) { tcs.SetException(new GameRequestException("game API not attached yet")); return tcs.Task; }
            ushort seq = NextSeq();
            TimeSpan timeout = RequestTimeout;
            _pending[seq] = new Pending { Tcs = tcs, Request = id, Deadline = DateTime.UtcNow + timeout, Timeout = timeout };
            _work.Enqueue(() =>
            {
                bool ok;
                try { ok = _api.Game_Request(id, seq, data); }
                catch (Exception e) { Fail(seq, new GameRequestException(id + " threw: " + e.Message)); return; }
                if (!ok) Fail(seq, new GameRequestException(id + " was refused by the game"));
            });
            return tcs.Task;
        }

        ushort NextSeq()
        {
            while (true)
            {
                int n = Interlocked.Increment(ref _seq);
                ushort s = (ushort)(1000 + (n % 60000));
                if (!_pending.ContainsKey(s)) return s;
            }
        }

        void Fail(ushort seq, Exception e)
        {
            if (_pending.TryRemove(seq, out Pending p)) p.Tcs.TrySetException(e);
        }

        /// <summary>Game_Update: drain queued work within the frame budget and expire timed-out requests.</summary>
        public void Tick()
        {
            if (MainThreadId == 0) MainThreadId = Thread.CurrentThread.ManagedThreadId;
            FramesSeen++;
            _sw.Restart();
            while (_sw.Elapsed.TotalMilliseconds < FrameBudgetMs && _work.TryDequeue(out Action a))
            {
                try { a(); } catch (Exception e) { _log.Error("game-thread work threw: " + e); }
            }
            if ((FramesSeen & 63) == 0) ExpireTimeouts();
        }

        void ExpireTimeouts()
        {
            DateTime now = DateTime.UtcNow;
            foreach (KeyValuePair<ushort, Pending> kv in _pending)
                if (kv.Value.Deadline < now) Fail(kv.Key, new GameRequestException(kv.Value.Request + " got no answer within " + kv.Value.Timeout.TotalSeconds + "s"));
        }

        /// <summary>Game_Event: resolve a pending request by seqNr, else hand it to the event adapter.</summary>
        public void OnGameEvent(CmdId eventId, ushort seqNr, object data)
        {
            if (seqNr != 0 && _pending.TryRemove(seqNr, out Pending p))
            {
                if (eventId == CmdId.Event_Error)
                {
                    var info = data as ErrorInfo;
                    ErrorType t = info != null ? info.errorType : ErrorType.None;
                    p.Tcs.TrySetException(new GameRequestException(p.Request + " failed: " + t, t));
                }
                else p.Tcs.TrySetResult(data);
                return;
            }
            Action<CmdId, object> h = UnsolicitedEvent;
            if (h != null) h(eventId, data);
        }

        public int PendingCount { get { return _pending.Count; } }
        public int QueuedWork { get { return _work.Count; } }
    }
}
