using System;
using System.Collections.Generic;
using System.Diagnostics;
using Takaro.Interfaces;
using Takaro.Persistence;
using Takaro.WebSocket;

namespace Takaro.Services
{
    /// <summary>
    /// Periodic main-thread sampler for data that is not event-driven: player
    /// positions and ping every few seconds, plus a slower ban resync to catch
    /// bans issued from the server console.
    /// </summary>
    public class PositionSampler : IService
    {
        private static PositionSampler _instance;
        private static readonly object _lock = new object();

        private static readonly TimeSpan SampleInterval = TimeSpan.FromSeconds(3);
        private static readonly TimeSpan BanSyncInterval = TimeSpan.FromSeconds(60);

        private long _nextSampleTicks;
        private DateTime _nextBanSyncUtc = DateTime.MinValue;

        // GameUpdate runs before GameStartDone, when the database is not open yet.
        private volatile bool _started;

        public static PositionSampler Instance
        {
            get
            {
                if (_instance != null)
                    return _instance;
                lock (_lock)
                {
                    if (_instance == null)
                        _instance = new PositionSampler();
                }
                return _instance;
            }
        }

        /// <summary>Called at GameStartDone, right after the database open is queued.</summary>
        public void Start()
        {
            _started = true;
        }

        public void OnGameUpdate(ref ModEvents.SGameUpdateData data)
        {
            if (!_started)
                return;

            DateTime now = DateTime.UtcNow;

            long ticks = Stopwatch.GetTimestamp();
            if (ticks >= _nextSampleTicks)
            {
                _nextSampleTicks = ticks + (long)(SampleInterval.TotalSeconds * Stopwatch.Frequency);
                SamplePositions();
            }

            if (now >= _nextBanSyncUtc)
            {
                _nextBanSyncUtc = now + BanSyncInterval;
                StateMirror.Instance.RefreshBans();
            }
        }

        private static void SamplePositions()
        {
            World world = GameManager.Instance.World;
            if (world == null)
                return;

            var batch = new List<PositionSample>();
            var positions = new List<PlayerPositionEntry>();
            var seen = new HashSet<string>();
            foreach (EntityPlayer player in world.Players.list)
            {
                ClientInfo cInfo = ConnectionManager.Instance.Clients.ForEntityId(player.entityId);
                if (cInfo?.CrossplatformId == null)
                    continue;

                string gameId = Shared.GameIdFromClientInfo(cInfo);
                if (string.IsNullOrEmpty(gameId) || !seen.Add(gameId))
                    continue;

                UnityEngine.Vector3 position = player.GetPosition();
                Vector3i rounded = new Vector3i(position);
                batch.Add(
                    new PositionSample
                    {
                        GameId = gameId,
                        X = position.x,
                        Y = position.y,
                        Z = position.z,
                        Ping = cInfo.ping,
                    }
                );
                positions.Add(new PlayerPositionEntry
                {
                    GameId = gameId,
                    Position = new TakaroPosition { X = rounded.x, Y = rounded.y, Z = rounded.z },
                });
            }

            if (batch.Count > 0)
                StateMirror.Instance.UpdatePositions(batch);
            GameEventPublisher.SendPlayerPositions(positions);
        }

        public void OnInit() { }

        public void OnDestroy()
        {
            _started = false;
        }
    }
}
