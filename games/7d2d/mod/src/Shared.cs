using System;
using System.Collections.Generic;
using Newtonsoft.Json;
using Takaro.Persistence;

namespace Takaro
{
    public class TakaroPlayer
    {
        [JsonProperty("gameId")]
        public string GameId { get; set; }

        [JsonProperty("name")]
        public string Name { get; set; }

        [JsonProperty("platformId", NullValueHandling = NullValueHandling.Ignore)]
        public string PlatformId { get; set; }

        // Takaro matches players on these columns, never on platformId alone. Profiles
        // created by Takaro's built-in 7 Days to Die integration carry only these, so
        // omitting them made Takaro create a second profile for the same person.
        [JsonProperty("steamId", NullValueHandling = NullValueHandling.Ignore)]
        public string SteamId { get; set; }

        [JsonProperty("epicOnlineServicesId", NullValueHandling = NullValueHandling.Ignore)]
        public string EpicOnlineServicesId { get; set; }

        [JsonProperty("xboxLiveId", NullValueHandling = NullValueHandling.Ignore)]
        public string XboxLiveId { get; set; }

        [JsonProperty("ip", NullValueHandling = NullValueHandling.Ignore)]
        public string Ip { get; set; }

        [JsonProperty("ping", NullValueHandling = NullValueHandling.Ignore)]
        public int? Ping { get; set; }
    }

    public class TakaroItem
    {
        [JsonProperty("name")]
        public string Name { get; set; }

        [JsonProperty("code")]
        public string Code { get; set; }

        [JsonProperty("description")]
        public string Description { get; set; }

        [JsonProperty("amount")]
        public int Amount { get; set; }

        [JsonProperty("quality")]
        public string Quality { get; set; }
    }

    public class TakaroEntity
    {
        [JsonProperty("code")]
        public string Code { get; set; }

        [JsonProperty("name")]
        public string Name { get; set; }

        [JsonProperty("description", NullValueHandling = NullValueHandling.Ignore)]
        public string Description { get; set; }

        [JsonProperty("type", NullValueHandling = NullValueHandling.Ignore)]
        public string Type { get; set; }

        [JsonProperty("metadata", NullValueHandling = NullValueHandling.Ignore)]
        public Dictionary<string, object> Metadata { get; set; }
    }

    public class TakaroPosition
    {
        [JsonProperty("x")]
        public int X { get; set; }

        [JsonProperty("y")]
        public int Y { get; set; }

        [JsonProperty("z")]
        public int Z { get; set; }

        [JsonProperty("dimension", NullValueHandling = NullValueHandling.Ignore)]
        public string Dimension { get; set; }
    }

    public class TakaroLocation
    {
        [JsonProperty("name")]
        public string Name { get; set; }

        [JsonProperty("code", NullValueHandling = NullValueHandling.Ignore)]
        public string Code { get; set; }

        [JsonProperty("position")]
        public TakaroPosition Position { get; set; }

        [JsonProperty("radius", NullValueHandling = NullValueHandling.Ignore)]
        public int? Radius { get; set; }

        [JsonProperty("sizeX", NullValueHandling = NullValueHandling.Ignore)]
        public int? SizeX { get; set; }

        [JsonProperty("sizeY", NullValueHandling = NullValueHandling.Ignore)]
        public int? SizeY { get; set; }

        [JsonProperty("sizeZ", NullValueHandling = NullValueHandling.Ignore)]
        public int? SizeZ { get; set; }

        [JsonProperty("metadata", NullValueHandling = NullValueHandling.Ignore)]
        public Dictionary<string, object> Metadata { get; set; }
    }

    public class TakaroBan
    {
        [JsonProperty("player")]
        public TakaroPlayer Player { get; set; }

        [JsonProperty("reason")]
        public string Reason { get; set; }

        [JsonProperty("expiresAt")]
        public string ExpiresAt { get; set; }
    }

    public static class Shared
    {
        // Takaro gameId is the EOS ID (CrossPlatform ID) without the EOS_ prefix
        public static string GameIdFromClientInfo(ClientInfo clientInfo)
        {
            return clientInfo.CrossplatformId.CombinedString.Replace("EOS_", "");
        }

        public static PlayerRecord BuildPlayerRecord(ClientInfo cInfo)
        {
            var record = new PlayerRecord
            {
                GameId = GameIdFromClientInfo(cInfo),
                Name = cInfo.playerName,
                Ip = cInfo.ip,
                Ping = cInfo.ping,
                EntityId = cInfo.entityId,
                EpicOnlineServicesId = GameIdFromClientInfo(cInfo),
                LastSeenUtc = DateTime.UtcNow,
            };

            if (cInfo.PlatformId != null && cInfo.PlatformId.CombinedString != null)
            {
                if (cInfo.PlatformId.CombinedString.StartsWith("Steam_"))
                    record.SteamId = cInfo.PlatformId.CombinedString.Replace("Steam_", "");
                else if (cInfo.PlatformId.CombinedString.StartsWith("XBL_"))
                    record.XboxLiveId = cInfo.PlatformId.CombinedString.Replace("XBL_", "");
            }

            return record;
        }

        public static string EntityKillName(string entityClassName, string entityType)
        {
            if (!string.IsNullOrEmpty(entityClassName))
                return entityClassName;
            return entityType.ToLower();
        }

        public static TakaroPlayer TransformPlayerRecordToTakaroPlayer(PlayerRecord record)
        {
            return WithIdentifiers(
                new TakaroPlayer
                {
                    GameId = record.GameId,
                    Name = record.Name,
                    Ip = record.Ip,
                    Ping = record.Ping,
                },
                record.SteamId,
                record.XboxLiveId,
                record.EpicOnlineServicesId
            );
        }

        public static TakaroBan TransformBanRecordToTakaroBan(BanRecord record)
        {
            return new TakaroBan
            {
                Player = WithIdentifiers(
                    new TakaroPlayer { GameId = record.GameId, Name = record.Name },
                    record.SteamId,
                    record.XboxLiveId,
                    record.EpicOnlineServicesId
                ),
                Reason = record.Reason,
                ExpiresAt = record.ExpiresAt,
            };
        }

        public static ClientInfo GetClientInfoFromGameId(string gameId)
        {
            PlatformUserIdentifierAbs userId = PlatformUserIdentifierAbs.FromCombinedString(
                $"EOS_{gameId}"
            );
            ClientInfo cInfo = ConnectionManager.Instance.Clients.ForUserId(userId);
            return cInfo;
        }

        public static TakaroPlayer TransformClientInfoToTakaroPlayer(ClientInfo clientInfo)
        {
            if (clientInfo == null)
                return null;

            string steamId = null;
            string xboxLiveId = null;
            string epicOnlineServicesId = clientInfo.CrossplatformId.CombinedString.Replace(
                "EOS_",
                ""
            );
            TakaroPlayer player = new TakaroPlayer
            {
                // Takaro gameId is the EOS ID (CrossPlatform ID) without the EOS_ prefix
                GameId = clientInfo.CrossplatformId.CombinedString.Replace("EOS_", ""),
                Name = clientInfo.playerName,
                Ip = clientInfo.ip,
                Ping = clientInfo.ping,
            };

            if (clientInfo.PlatformId != null && clientInfo.PlatformId.CombinedString != null)
            {
                if (clientInfo.PlatformId.CombinedString.StartsWith("Steam_"))
                {
                    steamId = clientInfo.PlatformId.CombinedString.Replace("Steam_", "");
                }
                else if (clientInfo.PlatformId.CombinedString.StartsWith("XBL_"))
                {
                    xboxLiveId = clientInfo.PlatformId.CombinedString.Replace("XBL_", "");
                }
            }

            return WithIdentifiers(player, steamId, xboxLiveId, epicOnlineServicesId);
        }

        public static TakaroPlayer TransformClientInfoToTakaroPlayerIdentity(ClientInfo clientInfo)
        {
            if (clientInfo == null)
                return null;

            string crossplatformId = clientInfo.CrossplatformId?.CombinedString;
            if (string.IsNullOrEmpty(crossplatformId))
                return null;

            string steamId = null;
            string xboxLiveId = null;
            string platformId = clientInfo.PlatformId?.CombinedString;
            if (!string.IsNullOrEmpty(platformId))
            {
                if (platformId.StartsWith("Steam_"))
                    steamId = platformId.Replace("Steam_", "");
                else if (platformId.StartsWith("XBL_"))
                    xboxLiveId = platformId.Replace("XBL_", "");
            }

            string epicOnlineServicesId = crossplatformId.Replace("EOS_", "");
            return WithIdentifiers(
                new TakaroPlayer { GameId = epicOnlineServicesId, Name = clientInfo.playerName },
                steamId,
                xboxLiveId,
                epicOnlineServicesId
            );
        }

        /// <summary>
        /// Sends every identifier the game knows: the dedicated steamId /
        /// epicOnlineServicesId / xboxLiveId fields (what Takaro's built-in 7D2D
        /// integration stored) plus platformId. Empty values stay off the wire.
        /// </summary>
        public static TakaroPlayer WithIdentifiers(
            TakaroPlayer player,
            string steamId,
            string xboxLiveId,
            string epicOnlineServicesId
        )
        {
            player.SteamId = NullIfEmpty(steamId);
            player.XboxLiveId = NullIfEmpty(xboxLiveId);
            player.EpicOnlineServicesId = NullIfEmpty(epicOnlineServicesId);
            player.PlatformId = PlatformIdFromIdentifiers(
                steamId,
                xboxLiveId,
                epicOnlineServicesId
            );
            return player;
        }

        private static string NullIfEmpty(string value)
        {
            return string.IsNullOrEmpty(value) ? null : value;
        }

        public static string PlatformIdFromIdentifiers(
            string steamId,
            string xboxLiveId,
            string epicOnlineServicesId
        )
        {
            if (!string.IsNullOrEmpty(steamId))
                return $"steam:{steamId}";
            if (!string.IsNullOrEmpty(xboxLiveId))
                return $"xbox:{xboxLiveId}";
            if (!string.IsNullOrEmpty(epicOnlineServicesId))
                return $"eos:{epicOnlineServicesId}";
            return null;
        }

        /// <summary>
        /// The game's localisation returns the key itself when it has no text
        /// for it ("driftwoodDesc"), which is not something to show a player.
        /// </summary>
        public static string LocalizedOrNull(string key, string localized)
        {
            if (string.IsNullOrWhiteSpace(localized) || localized == key)
                return null;
            return localized;
        }

        public static string ItemDescription(string itemCode)
        {
            string key = itemCode + "Desc";
            return LocalizedOrNull(key, Localization.Get(key, true));
        }

        public static TakaroItem TransformItemToTakaroItem(ItemClass itemClass)
        {
            string Description = ItemDescription(itemClass.GetItemName());

            TakaroItem takaroItem = new TakaroItem
            {
                Name = itemClass.GetLocalizedItemName(),
                Code = itemClass.GetItemName(),
                Description = Description,
            };

            return takaroItem;
        }
    }
}
