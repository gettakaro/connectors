using System;
using System.Collections.Concurrent;
using System.Threading.Tasks;
using Takaro.Interfaces;

namespace Takaro.Services
{
    /// <summary>
    /// Marshals work from background threads (WebSocket handlers) onto the game
    /// main thread. The queue is drained from ModEvents.GameUpdate; callers await
    /// the returned Task, whose continuations run off the game thread.
    /// </summary>
    public class MainThreadDispatcher : IService
    {
        private static MainThreadDispatcher _instance;
        private static readonly object _lock = new object();

        private const int MaxOpsPerTick = 32;

        private readonly ConcurrentQueue<Action> _queue = new ConcurrentQueue<Action>();
        private volatile bool _shuttingDown;

        public static MainThreadDispatcher Instance
        {
            get
            {
                if (_instance != null)
                    return _instance;
                lock (_lock)
                {
                    if (_instance == null)
                        _instance = new MainThreadDispatcher();
                }
                return _instance;
            }
        }

        public Task<T> Run<T>(Func<T> fn)
        {
            var tcs = new TaskCompletionSource<T>(
                TaskCreationOptions.RunContinuationsAsynchronously
            );

            if (_shuttingDown)
            {
                tcs.SetException(new InvalidOperationException("Game is shutting down"));
                return tcs.Task;
            }

            _queue.Enqueue(() =>
            {
                try
                {
                    tcs.SetResult(fn());
                }
                catch (Exception ex)
                {
                    tcs.SetException(ex);
                }
            });
            return tcs.Task;
        }

        public Task Run(Action fn)
        {
            return Run(() =>
            {
                fn();
                return true;
            });
        }

        public void OnGameUpdate(ref ModEvents.SGameUpdateData data)
        {
            if (!_pumpConfirmed)
            {
                _pumpConfirmed = true;
                LogService.Instance.Info("Main thread pump active (ModEvents.GameUpdate)");
            }

            for (int i = 0; i < MaxOpsPerTick && _queue.TryDequeue(out Action op); i++)
                op();
        }

        private bool _pumpConfirmed;

        /// <summary>
        /// Called from GameShutdown on the main thread: run whatever is still
        /// queued (completing pending awaits), then fail-fast any later arrivals.
        /// </summary>
        public void Shutdown()
        {
            _shuttingDown = true;
            while (_queue.TryDequeue(out Action op))
                op();
        }

        public void OnInit() { }

        public void OnDestroy() { }
    }
}
