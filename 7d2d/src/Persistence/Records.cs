using System;
using System.Collections.Generic;
using LiteDB;

namespace Takaro.Persistence
{
    public class PlayerRecord
    {
        [BsonId]
        public string GameId { get; set; }

        public string Name { get; set; }
        public string Ip { get; set; }
        public int Ping { get; set; }
        public string SteamId { get; set; }
        public string XboxLiveId { get; set; }
        public string EpicOnlineServicesId { get; set; }
        public int EntityId { get; set; }
        public bool Online { get; set; }
        public float X { get; set; }
        public float Y { get; set; }
        public float Z { get; set; }
        public DateTime LastSeenUtc { get; set; }
    }

    public class ItemSlot
    {
        public string Code { get; set; }
        public string Name { get; set; }
        public string Description { get; set; }
        public int Amount { get; set; }
        public string Quality { get; set; }
    }

    public class InventoryRecord
    {
        [BsonId]
        public string GameId { get; set; }

        public List<ItemSlot> Items { get; set; }
        public DateTime UpdatedUtc { get; set; }
    }

    public class BanRecord
    {
        // Combined platform identifier string (e.g. "EOS_xxx"), unique per ban entry.
        [BsonId]
        public string Id { get; set; }

        public string GameId { get; set; }
        public string Name { get; set; }
        public string SteamId { get; set; }
        public string XboxLiveId { get; set; }
        public string EpicOnlineServicesId { get; set; }
        public string Reason { get; set; }

        // ISO-8601 string or null for permanent — matches the wire format exactly.
        public string ExpiresAt { get; set; }
    }

    public class ItemRecord
    {
        [BsonId]
        public string Code { get; set; }

        public string Name { get; set; }
        public string Description { get; set; }
    }

    public class PositionSample
    {
        public string GameId { get; set; }
        public float X { get; set; }
        public float Y { get; set; }
        public float Z { get; set; }
        public int Ping { get; set; }
    }
}
