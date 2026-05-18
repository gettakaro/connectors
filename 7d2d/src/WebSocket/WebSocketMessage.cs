using System;
using System.Collections.Generic;
using Newtonsoft.Json;

namespace Takaro.WebSocket
{
    [Serializable]
    public class WebSocketMessage
    {
        [JsonProperty("type")]
        public string Type { get; set; }

        [JsonProperty("payload")]
        public object Payload { get; set; }

        [JsonProperty("requestId")]
        public string RequestId { get; set; }

        public WebSocketMessage() { }

        public WebSocketMessage(string type)
        {
            Type = type;
            Payload = new Dictionary<string, object>();
        }

        public WebSocketMessage(
            string type,
            Dictionary<string, object> data,
            string requestId = null
        )
        {
            Type = type;
            RequestId = requestId;
            Payload = data;
        }

        public WebSocketMessage(string type, object[] objectArray, string requestId = null)
        {
            Type = type;
            RequestId = requestId;
            Payload = objectArray;
        }

        // Generic create method - primary method to replace most specific methods
        public static WebSocketMessage Create(
            string type,
            Dictionary<string, object> payload = null,
            string requestId = null
        )
        {
            return new WebSocketMessage(
                type,
                payload ?? new Dictionary<string, object>(),
                requestId
            );
        }

        // Generic create method for array payloads
        public static WebSocketMessage Create(
            string type,
            object[] payload,
            string requestId = null
        )
        {
            return new WebSocketMessage(type, payload, requestId);
        }

        // Standard response methods
        public static WebSocketMessage CreateResponse(string requestId, object data)
        {
            // Handle dictionary data
            if (data is Dictionary<string, object> dictData)
            {
                return new WebSocketMessage("response", dictData, requestId);
            }

            // Handle array data
            if (data is object[] listData)
            {
                return new WebSocketMessage("response", listData, requestId);
            }

            var payload = new Dictionary<string, object>();

            if (data != null)
            {
                // For complex objects, extract properties
                foreach (var prop in data.GetType().GetProperties())
                {
                    payload[prop.Name] = prop.GetValue(data);
                }

                // If no properties were found, serialize the entire object as a single value
                if (payload.Count == 0)
                {
                    payload["value"] = data;
                }
            }

            return new WebSocketMessage("response", payload, requestId);
        }

        public static WebSocketMessage CreateErrorResponse(string requestId, string errorMessage)
        {
            return new WebSocketMessage(
                MessageTypes.Error,
                new Dictionary<string, object>
                {
                    { "error", errorMessage }
                },
                requestId
            );
        }

        // Common message type constants - for consistency and to avoid string typos
        public static class MessageTypes
        {
            // Basic types
            public const string Ping = "ping";
            public const string Identify = "identify";
            public const string Response = "response";
            public const string Error = "error";
            public const string GameEvent = "gameEvent";
        }

        // A few common message factory methods for convenience
        public static WebSocketMessage CreateHeartbeat()
        {
            return Create(
                MessageTypes.Ping,
                new Dictionary<string, object> { { "timestamp", DateTime.UtcNow.ToString("o") } }
            );
        }

        public static WebSocketMessage CreateIdentify(
            string registrationToken,
            string identityToken
        )
        {
            return Create(
                MessageTypes.Identify,
                new Dictionary<string, object>
                {
                    { "registrationToken", registrationToken },
                    { "identityToken", identityToken }
                }
            );
        }
    }
}
