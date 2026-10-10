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

        // Protocol v1 failure; omitted from every other frame.
        [JsonProperty("error", NullValueHandling = NullValueHandling.Ignore)]
        public object Error { get; set; }

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
            return new WebSocketMessage
            {
                Type = MessageTypes.Response,
                Payload = data,
                RequestId = requestId,
            };
        }

        /// <summary>
        /// Protocol v0 reports a failed request as an "error" frame; v1 answers
        /// with the "response" frame and a coded error and a null payload.
        /// </summary>
        public static WebSocketMessage CreateErrorResponse(
            string requestId,
            string errorMessage,
            string code = ProtocolErrorCodes.GameError,
            int protocolVersion = 0
        )
        {
            if (protocolVersion == 0)
            {
                return new WebSocketMessage(
                    MessageTypes.Error,
                    new Dictionary<string, object> { { "error", errorMessage } },
                    requestId
                );
            }

            return new WebSocketMessage
            {
                Type = MessageTypes.Response,
                Payload = null,
                RequestId = requestId,
                Error = new Dictionary<string, object>
                {
                    { "code", code },
                    { "message", errorMessage },
                },
            };
        }

        // Common message type constants - for consistency and to avoid string typos
        public static class MessageTypes
        {
            // Basic types
            public const string Ping = "ping";
            public const string Request = "request";
            public const string Identify = "identify";
            public const string Response = "response";
            public const string Error = "error";
            public const string GameEvent = "gameEvent";
            public const string IdentifyResponse = "identifyResponse";
            public const string EventAck = "eventAck";
            public const string EventError = "eventError";
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
                ProtocolHandshake.BuildIdentifyPayload(registrationToken, identityToken)
            );
        }
    }
}
