using System;
using System.Collections.Generic;
using Takaro.Interfaces;
using Takaro.Persistence;

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

        private DateTime _nextSampleUtc = DateTime.MinValue;
        private DateTime _nextBanSyncUtc = DateTime.MinValue;

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

        public void OnGameUpdate(ref ModEvents.SGameUpdateData data)
        {
            DateTime now = DateTime.UtcNow;

            if (now >= _nextSampleUtc)
            {
                _nextSampleUtc = now + SampleInterval;
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
            foreach (EntityPlayer player in world.Players.list)
            {
                ClientInfo cInfo = ConnectionManager.Instance.Clients.ForEntityId(player.entityId);
                if (cInfo?.CrossplatformId == null)
                    continue;

                UnityEngine.Vector3 position = player.GetPosition();
                batch.Add(
                    new PositionSample
                    {
                        GameId = Shared.GameIdFromClientInfo(cInfo),
                        X = position.x,
                        Y = position.y,
                        Z = position.z,
                        Ping = cInfo.ping,
                    }
                );
            }

            if (batch.Count > 0)
                StateMirror.Instance.UpdatePositions(batch);
        }

        public void OnInit() { }

        public void OnDestroy() { }
    }
}
