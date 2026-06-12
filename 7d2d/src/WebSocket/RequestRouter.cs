using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using Newtonsoft.Json;
using Takaro.Services;

namespace Takaro.WebSocket
{
    public class WebSocketArgs<T>
    {
        public static T Parse(object argsObject)
        {
            try
            {
                if (argsObject == null)
                    return default;

                if (argsObject is string argsString)
                {
                    return JsonConvert.DeserializeObject<T>(argsString);
                }

                if (argsObject is Newtonsoft.Json.Linq.JObject jObject)
                {
                    return jObject.ToObject<T>();
                }

                if (argsObject is Dictionary<string, object> dict)
                {
                    string json = JsonConvert.SerializeObject(dict);
                    return JsonConvert.DeserializeObject<T>(json);
                }

                return (T)Convert.ChangeType(argsObject, typeof(T));
            }
            catch (Exception ex)
            {
                LogService.Instance.Error($"Error parsing WebSocket args: {ex.Message}");
                return default;
            }
        }
    }

    public class TakaroPlayerReferenceArgs
    {
        public string GameId { get; set; }
    }

    public class TakaroGiveItemArgs
    {
        public string GameId { get; set; }
        public string Item { get; set; }
        public int Amount { get; set; }
        public string Quality { get; set; }
    }

    public class TakaroExecuteCommandArgs
    {
        public string Command { get; set; }
    }

    public class TakaroSendMessageArgs
    {
        public string Message { get; set; }
        public TakaroSendMessageRecipientArgs Recipient { get; set; }
    }

    public class TakaroSendMessageRecipientArgs
    {
        public string GameId { get; set; }
    }

    public class TakaroPlayerReference
    {
        public string GameId { get; set; }
    }

    public class TakaroKickPlayerArgs
    {
        public TakaroPlayerReference Player { get; set; }
        public string Reason { get; set; }
    }

    public class TakaroBanPlayerArgs
    {
        public TakaroPlayerReference Player { get; set; }
        public string Reason { get; set; }
        public string ExpiresAt { get; set; }
    }

    public class TakaroUnbanPlayerArgs
    {
        public TakaroPlayerReference Player { get; set; }
    }

    public class TakaroTeleportPlayerArgs
    {
        public TakaroPlayerReference Player { get; set; }
        public float X { get; set; }
        public float Y { get; set; }
        public float Z { get; set; }
    }

    /// <summary>
    /// Parses incoming WebSocket messages and dispatches them to the read/action
    /// handlers. Runs on the WebSocket receive thread; reads are served from the
    /// state mirror, actions are awaited through the MainThreadDispatcher. The
    /// single error boundary here turns any handler exception into a protocol
    /// error response.
    /// </summary>
    public static class RequestRouter
    {
        public static void Route(string message)
        {
            try
            {
                LogService.Instance.Debug($"Received WebSocket message: {message}");
                var webSocketMessage = JsonConvert.DeserializeObject<WebSocketMessage>(message);

                if (webSocketMessage == null || webSocketMessage.Payload == null)
                    return;

                string requestId = webSocketMessage.RequestId;
                if (string.IsNullOrEmpty(requestId))
                {
                    LogService.Instance.Warn("Received message without requestId");
                    return;
                }

                Dictionary<string, object> payloadDict =
                    webSocketMessage.Payload as Dictionary<string, object>;

                if (payloadDict == null)
                {
                    if (webSocketMessage.Payload is Newtonsoft.Json.Linq.JObject jObject)
                    {
                        payloadDict = jObject.ToObject<Dictionary<string, object>>();
                    }
                    else
                    {
                        LogService.Instance.Warn(
                            "Received message with payload that is not a dictionary"
                        );
                        return;
                    }
                }

                if (!payloadDict.ContainsKey("action"))
                    return;

                string action = payloadDict["action"].ToString();
                object args = payloadDict.ContainsKey("args") ? payloadDict["args"] : null;

                _ = Dispatch(action, requestId, args);
            }
            catch (Exception ex)
            {
                LogService.Instance.Error($"Error handling WebSocket message: {ex.Message}");
                Log.Exception(ex);
            }
        }

        private static async Task Dispatch(string action, string requestId, object args)
        {
            try
            {
                switch (action)
                {
                    case "testReachability":
                        ReadHandlers.TestReachability(requestId);
                        break;
                    case "getPlayers":
                        ReadHandlers.GetPlayers(requestId);
                        break;
                    case "getPlayer":
                    {
                        var playerArgs = WebSocketArgs<TakaroPlayerReferenceArgs>.Parse(args);
                        if (playerArgs == null || string.IsNullOrEmpty(playerArgs.GameId))
                        {
                            SendError(requestId, "Invalid or missing gameId parameter");
                            return;
                        }
                        ReadHandlers.GetPlayer(requestId, playerArgs.GameId);
                        break;
                    }
                    case "getPlayerLocation":
                    {
                        var locationArgs = WebSocketArgs<TakaroPlayerReferenceArgs>.Parse(args);
                        if (locationArgs == null || string.IsNullOrEmpty(locationArgs.GameId))
                        {
                            SendError(requestId, "Invalid or missing gameId parameter");
                            return;
                        }
                        ReadHandlers.GetPlayerLocation(requestId, locationArgs.GameId);
                        break;
                    }
                    case "getPlayerInventory":
                    {
                        var inventoryArgs = WebSocketArgs<TakaroPlayerReferenceArgs>.Parse(args);
                        if (inventoryArgs == null || string.IsNullOrEmpty(inventoryArgs.GameId))
                        {
                            SendError(requestId, "Invalid or missing gameId parameter");
                            return;
                        }
                        ReadHandlers.GetPlayerInventory(requestId, inventoryArgs.GameId);
                        break;
                    }
                    case "listItems":
                        ReadHandlers.ListItems(requestId);
                        break;
                    case "listBans":
                        ReadHandlers.ListBans(requestId);
                        break;
                    case "giveItem":
                        await ActionHandlers.GiveItem(
                            requestId,
                            WebSocketArgs<TakaroGiveItemArgs>.Parse(args)
                        );
                        break;
                    case "executeConsoleCommand":
                        await ActionHandlers.ExecuteCommand(
                            requestId,
                            WebSocketArgs<TakaroExecuteCommandArgs>.Parse(args)
                        );
                        break;
                    case "sendMessage":
                        await ActionHandlers.SendChatMessage(
                            requestId,
                            WebSocketArgs<TakaroSendMessageArgs>.Parse(args)
                        );
                        break;
                    case "kickPlayer":
                        await ActionHandlers.KickPlayer(
                            requestId,
                            WebSocketArgs<TakaroKickPlayerArgs>.Parse(args)
                        );
                        break;
                    case "banPlayer":
                        await ActionHandlers.BanPlayer(
                            requestId,
                            WebSocketArgs<TakaroBanPlayerArgs>.Parse(args)
                        );
                        break;
                    case "unbanPlayer":
                        await ActionHandlers.UnbanPlayer(
                            requestId,
                            WebSocketArgs<TakaroUnbanPlayerArgs>.Parse(args)
                        );
                        break;
                    case "teleportPlayer":
                        await ActionHandlers.TeleportPlayer(
                            requestId,
                            WebSocketArgs<TakaroTeleportPlayerArgs>.Parse(args)
                        );
                        break;
                    case "shutdown":
                        await ActionHandlers.Shutdown(requestId);
                        break;
                    default:
                        LogService.Instance.Warn($"Unknown message type: {action}");
                        SendError(requestId, $"Unknown message type: {action}");
                        break;
                }
            }
            catch (Exception ex)
            {
                LogService.Instance.Error($"Error processing '{action}': {ex.Message}");
                Log.Exception(ex);
                SendError(requestId, ex.Message);
            }
        }

        private static void SendError(string requestId, string errorMessage)
        {
            WebSocketTransport.Instance.SendErrorResponse(requestId, errorMessage);
        }
    }
}
