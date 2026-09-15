namespace Takaro.Valheim.Core;

public interface IMainThreadActionScheduler
{
    Task<T> ScheduleAsync<T>(Func<T> action, CancellationToken cancellationToken = default);
}

public sealed class InlineMainThreadActionScheduler : IMainThreadActionScheduler
{
    public static InlineMainThreadActionScheduler Instance { get; } = new();

    private InlineMainThreadActionScheduler()
    {
    }

    public Task<T> ScheduleAsync<T>(Func<T> action, CancellationToken cancellationToken = default)
    {
        if (action is null)
        {
            throw new ArgumentNullException(nameof(action));
        }
        if (cancellationToken.IsCancellationRequested)
        {
            return Task.FromCanceled<T>(cancellationToken);
        }

        try
        {
            return Task.FromResult(action());
        }
        catch (Exception ex)
        {
            return Task.FromException<T>(ex);
        }
    }
}

public sealed class QueuedMainThreadActionScheduler : IMainThreadActionScheduler, IDisposable
{
    public const int DefaultCapacity = 256;
    public const int DefaultMaxActionsPerDrain = 64;

    private readonly object gate = new();
    private readonly Queue<IWorkItem> pending = new();
    private readonly int capacity;
    private bool disposed;

    public QueuedMainThreadActionScheduler(int capacity = DefaultCapacity)
    {
        if (capacity <= 0)
        {
            throw new ArgumentOutOfRangeException(nameof(capacity), "Main-thread queue capacity must be positive.");
        }

        this.capacity = capacity;
    }

    public int PendingCount
    {
        get
        {
            lock (gate)
            {
                return pending.Count;
            }
        }
    }

    public Task<T> ScheduleAsync<T>(Func<T> action, CancellationToken cancellationToken = default)
    {
        if (action is null)
        {
            throw new ArgumentNullException(nameof(action));
        }
        if (cancellationToken.IsCancellationRequested)
        {
            return Task.FromCanceled<T>(cancellationToken);
        }

        var work = new WorkItem<T>(action, cancellationToken);
        lock (gate)
        {
            if (disposed)
            {
                work.Fail(new ObjectDisposedException(nameof(QueuedMainThreadActionScheduler)));
                return work.Task;
            }

            if (pending.Count >= capacity)
            {
                work.Fail(new InvalidOperationException(
                    $"Valheim main-thread action queue reached its capacity of {capacity}."));
                return work.Task;
            }

            pending.Enqueue(work);
        }

        return work.Task;
    }

    public int Drain(int maxActions = DefaultMaxActionsPerDrain)
    {
        if (maxActions <= 0)
        {
            throw new ArgumentOutOfRangeException(nameof(maxActions), "Drain limit must be positive.");
        }

        var processed = 0;
        while (processed < maxActions)
        {
            IWorkItem? work;
            lock (gate)
            {
                if (disposed)
                {
                    throw new ObjectDisposedException(nameof(QueuedMainThreadActionScheduler));
                }

                if (pending.Count == 0)
                {
                    break;
                }

                work = pending.Dequeue();
            }

            work.Execute();
            processed++;
        }

        return processed;
    }

    public void Dispose()
    {
        List<IWorkItem> abandoned;
        lock (gate)
        {
            if (disposed)
            {
                return;
            }

            disposed = true;
            abandoned = pending.ToList();
            pending.Clear();
        }

        var error = new ObjectDisposedException(
            nameof(QueuedMainThreadActionScheduler),
            "Valheim plugin stopped before the queued main-thread action could execute.");
        foreach (var work in abandoned)
        {
            work.Fail(error);
        }
    }

    private interface IWorkItem
    {
        void Execute();
        void Fail(Exception error);
    }

    private sealed class WorkItem<T> : IWorkItem
    {
        private readonly Func<T> action;
        private readonly CancellationToken cancellationToken;
        private readonly TaskCompletionSource<T> completion = new(
            TaskCreationOptions.RunContinuationsAsynchronously);
        private readonly CancellationTokenRegistration cancellationRegistration;
        private int state;

        public WorkItem(Func<T> action, CancellationToken cancellationToken)
        {
            this.action = action;
            this.cancellationToken = cancellationToken;
            cancellationRegistration = cancellationToken.CanBeCanceled
                ? cancellationToken.Register(Cancel)
                : default;
        }

        public Task<T> Task => completion.Task;

        public void Execute()
        {
            if (Interlocked.CompareExchange(ref state, 1, 0) != 0)
            {
                cancellationRegistration.Dispose();
                return;
            }

            try
            {
                cancellationToken.ThrowIfCancellationRequested();
                var result = action();
                if (cancellationToken.IsCancellationRequested)
                {
                    completion.TrySetCanceled(cancellationToken);
                }
                else
                {
                    completion.TrySetResult(result);
                }
            }
            catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
            {
                completion.TrySetCanceled(cancellationToken);
            }
            catch (Exception ex)
            {
                completion.TrySetException(ex);
            }
            finally
            {
                Volatile.Write(ref state, 2);
                cancellationRegistration.Dispose();
            }
        }

        public void Fail(Exception error)
        {
            if (Interlocked.CompareExchange(ref state, 2, 0) != 0)
            {
                cancellationRegistration.Dispose();
                return;
            }

            completion.TrySetException(error);
            cancellationRegistration.Dispose();
        }

        private void Cancel()
        {
            if (Interlocked.CompareExchange(ref state, 2, 0) == 0)
            {
                completion.TrySetCanceled(cancellationToken);
            }
        }
    }
}
