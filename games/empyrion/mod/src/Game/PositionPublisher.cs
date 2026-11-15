using System;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using Newtonsoft.Json.Linq;
using TakaroEmpyrion.Protocol;

namespace TakaroEmpyrion.Game
{
    /// <summary>Publishes complete, freshly read position snapshots on an identified v1 connection.</summary>
    public sealed class PositionPublisher
    {
        readonly Actions _actions;
        readonly TakaroClient _client;
        readonly ILog _log;
        readonly CancellationTokenSource _stop = new CancellationTokenSource();
        Task _loop;

        public PositionPublisher(Actions actions, TakaroClient client, ILog log)
        { _actions = actions; _client = client; _log = log; }

        public void Start() { _loop = Task.Run(Run); }
        public void Stop() { _stop.Cancel(); }

        async Task Run()
        {
            while (!_stop.IsCancellationRequested)
            {
                await PublishOnce();
                try { await Task.Delay(TimeSpan.FromSeconds(Capabilities.PositionIntervalSeconds), _stop.Token); }
                catch (TaskCanceledException) { break; }
            }
        }

        public async Task<bool> PublishOnce()
        {
            if (!_client.Identified || _client.ProtocolVersion < 1) return false;
            try
            {
                JArray positions = await _actions.CurrentPositions();
                if (!_client.Identified || _client.ProtocolVersion < 1 || _stop.IsCancellationRequested) return false;
                for (int offset = 0; offset < positions.Count || (offset == 0 && positions.Count == 0); offset += 1000)
                {
                    if (offset > 0)
                        await Task.Delay(TimeSpan.FromSeconds(Capabilities.PositionIntervalSeconds), _stop.Token);
                    if (!_client.Identified || _client.ProtocolVersion < 1 || _stop.IsCancellationRequested) return false;
                    _client.SendPositionSnapshot(new JObject
                    {
                        ["timestamp"] = DateTime.UtcNow.ToString("o"),
                        ["positions"] = new JArray(positions.Skip(offset).Take(1000).Select(p => p.DeepClone())),
                    });
                }
                return true;
            }
            catch (TaskCanceledException) { return false; }
            catch (Exception e)
            {
                _log.Warn("position snapshot failed: " + e.Message);
                return false;
            }
        }
    }
}
