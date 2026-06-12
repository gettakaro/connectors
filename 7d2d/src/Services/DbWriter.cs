using System;
using System.Collections.Concurrent;
using System.Threading;
using Takaro.Interfaces;

namespace Takaro.Services
{
    /// <summary>
    /// Single background thread that owns all writes to the state mirror database.
    /// Game-thread callers capture cheap POCO snapshots and enqueue; they never
    /// block on DB I/O.
    /// </summary>
    public class DbWriter : IService
    {
        private static DbWriter _instance;
        private static readonly object _lock = new object();

        private const int QueueDepthWarnThreshold = 1000;

        private BlockingCollection<Action> _queue;
        private Thread _thread;

        public static DbWriter Instance
        {
            get
            {
                if (_instance != null)
                    return _instance;
                lock (_lock)
                {
                    if (_instance == null)
                        _instance = new DbWriter();
                }
                return _instance;
            }
        }

        public void OnInit()
        {
            _queue = new BlockingCollection<Action>();
            _thread = new Thread(Drain) { IsBackground = true, Name = "Takaro-DbWriter" };
            _thread.Start();
        }

        public void Enqueue(Action op)
        {
            if (_queue == null || _queue.IsAddingCompleted)
                return;

            if (_queue.Count > QueueDepthWarnThreshold)
                LogService.Instance.Warn($"DbWriter queue depth at {_queue.Count}");

            _queue.Add(op);
        }

        private void Drain()
        {
            foreach (Action op in _queue.GetConsumingEnumerable())
            {
                try
                {
                    lock (Persistence.Database.Instance.SyncRoot)
                    {
                        op();
                    }
                }
                catch (Exception ex)
                {
                    // A failed write must not kill the writer thread — later ops
                    // would be dropped silently, which is worse than logging.
                    LogService.Instance.Error($"DbWriter operation failed: {ex.Message}");
                    Log.Exception(ex);
                }
            }
        }

        public void OnDestroy()
        {
            _queue?.CompleteAdding();
            _thread?.Join(TimeSpan.FromSeconds(5));
            _thread = null;
        }
    }
}
