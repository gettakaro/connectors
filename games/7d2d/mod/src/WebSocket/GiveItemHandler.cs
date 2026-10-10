using System;
using System.Threading.Tasks;
using Takaro.Services;

namespace Takaro.WebSocket
{
    /// <summary>
    /// Validates and executes one giveItem request on the game main thread.
    /// Every handled request emits exactly one correlated terminal response.
    /// </summary>
    public static class GiveItemHandler
    {
        public static async Task Handle(string requestId, TakaroGiveItemArgs args)
        {
            if (
                args == null
                || args.Player == null
                || string.IsNullOrEmpty(args.Player.GameId)
                || string.IsNullOrEmpty(args.Item)
            )
            {
                SendError(
                    requestId,
                    "Invalid or missing parameters",
                    ProtocolErrorCodes.InvalidArgs
                );
                return;
            }

            await MainThreadDispatcher.Instance.Run(() =>
            {
                ClientInfo cInfo = Shared.GetClientInfoFromGameId(args.Player.GameId);
                if (cInfo == null)
                {
                    SendError(requestId, "Player not found", ProtocolErrorCodes.NotFound);
                    return;
                }

                ItemValue itemValue = ItemClass.GetItem(args.Item);
                if (itemValue == null || itemValue.type == ItemValue.None.type)
                {
                    SendError(requestId, "Item not found", ProtocolErrorCodes.NotFound);
                    return;
                }

                if (
                    !GameManager.Instance.World.Players.dict.TryGetValue(
                        cInfo.entityId,
                        out EntityPlayer player
                    )
                )
                {
                    SendError(requestId, "Player entity not found", ProtocolErrorCodes.NotFound);
                    return;
                }

                if (!player.IsSpawned())
                {
                    SendError(requestId, "Player is not spawned");
                    return;
                }

                if (player.IsDead())
                {
                    SendError(requestId, "Player is dead");
                    return;
                }

                if (args.Amount <= 0)
                {
                    SendError(requestId, "Invalid item amount", ProtocolErrorCodes.InvalidArgs);
                    return;
                }

                ushort quality = Constants.cItemMaxQuality;
                if (!string.IsNullOrEmpty(args.Quality))
                {
                    if (
                        ushort.TryParse(args.Quality, out ushort parsedQuality)
                        && parsedQuality <= Constants.cItemMaxQuality
                    )
                    {
                        quality = parsedQuality;
                    }
                    else
                    {
                        SendError(
                            requestId,
                            "Invalid quality value",
                            ProtocolErrorCodes.InvalidArgs
                        );
                        return;
                    }
                }

                ItemValue iv = new ItemValue(itemValue.type, true);

                if (ItemClass.list[iv.type].HasSubItems)
                {
#if SEVEND2D_V3_3
                    for (int i = 0; i < iv.ModificationCount; i++)
                    {
                        ItemValue tmp = iv.GetModification(i);
                        if (tmp == null)
                            continue;
                        tmp.Quality = quality;
                        iv.SetModification(i, tmp);
                    }
#else
                    for (int i = 0; i < iv.Modifications.Length; i++)
                    {
                        ItemValue tmp = iv.Modifications[i];
                        tmp.Quality = quality;
                        iv.Modifications[i] = tmp;
                    }
#endif
                }
                else if (ItemClass.list[iv.type].HasQuality)
                {
                    iv.Quality = quality;
                }

                PlayerProximateItemDelivery.Deliver(iv, args.Amount, player, cInfo);

                Send(WebSocketMessage.CreateResponse(requestId, null));
            });
        }

        private static void Send(WebSocketMessage message)
        {
            WebSocketTransport.Instance.Send(message);
        }

        private static void SendError(
            string requestId,
            string errorMessage,
            string code = ProtocolErrorCodes.GameError
        )
        {
            WebSocketTransport.Instance.SendErrorResponse(requestId, errorMessage, code);
        }
    }
}
