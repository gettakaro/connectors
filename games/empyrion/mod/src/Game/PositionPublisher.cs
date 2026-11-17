using System;
using System.Threading;
using System.Threading.Tasks;
using Newtonsoft.Json.Linq;
using TakaroEmpyrion.Protocol;

namespace TakaroEmpyrion.Game
{
    /// <summary>Publishes complete, freshly read position snapshots on an identified v1 connection.</summary>
    public sealed class PositionPublisher
    {
        readonly Func<Task<JArray>> _readPositions;
        readonly TakaroClient _client;
        readonly ILog _log;
        readonly CancellationTokenSource _stop = new CancellationTokenSource();
        Task _loop;

        public PositionPublisher(Actions actions, TakaroClient client, ILog log)
            : this(actions.CurrentPositions, client, log) { }

        public PositionPublisher(Func<Task<JArray>> readPositions, TakaroClient client, ILog log)
        { _readPositions = readPositions; _client = client; _log = log; }

        public void Start() { _loop = Task.Run(Run); }
        public void Stop() { _stop.Cancel(); }

        async Task Run()
        {
            while (!_stop.IsCancellationRequested)
            {
                await PublishOnce();
                try { await Task.Delay(TimeSpan.FromMilliseconds(250), _stop.Token); }
                catch (TaskCanceledException) { break; }
            }
        }

        public async Task<bool> PublishOnce()
        {
            if (_stop.IsCancellationRequested || !_client.TryBeginPositionSnapshot(out long token)) return false;
            try
            {
                JArray positions = await _readPositions();
                if (_stop.IsCancellationRequested) return false;
                if (positions == null || positions.Count > 1000)
                {
                    _log.Warn("position snapshot exceeds 1000 players or is unavailable; dropping the complete sample");
                    return false;
                }
                return _client.SendPositionSnapshot(token, new JObject
                {
                    ["timestamp"] = DateTime.UtcNow.ToString("o"),
                    ["positions"] = positions,
                });
            }
            catch (TaskCanceledException) { return false; }
            catch (Exception e)
            {
                _log.Warn("position snapshot failed: " + e.Message);
                return false;
            }
            finally { _client.CancelPositionSnapshot(token); }
        }
    }
}
