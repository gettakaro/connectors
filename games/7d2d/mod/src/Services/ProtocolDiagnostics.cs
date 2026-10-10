using System;
using System.Collections.Generic;
using System.Text;
using Newtonsoft.Json.Linq;

namespace Takaro.Services
{
    public static class ProtocolDiagnostics
    {
        public const int MaxMessageLength = 512;

        public static string ExtractErrorMessage(object payload)
        {
            object message = null;
            if (payload is JObject jObject)
                message = jObject["message"];
            else if (payload is Dictionary<string, object> dictionary)
                dictionary.TryGetValue("message", out message);

            string text = message is JValue jValue ? jValue.Value?.ToString() : message?.ToString();
            if (string.IsNullOrWhiteSpace(text))
                return "Takaro reported an unspecified protocol error";

            return NormalizeAndBound(text, MaxMessageLength);
        }

        public static bool TryGetIdentifyRejection(string json, out string reason)
        {
            return TryGetIdentifyRejection(json, out _, out reason);
        }

        /// <summary>
        /// Takaro rejects an identify in one of two shapes and returns true for
        /// either. Protocol v0: payload.error (no code), socket left open.
        /// Protocol v1: a top-level error {code, message}, socket closed by
        /// Takaro. The reason is bounded; the code is null for v0.
        /// </summary>
        public static bool TryGetIdentifyRejection(string json, out string code, out string reason)
        {
            code = null;
            reason = null;
            if (string.IsNullOrEmpty(json))
                return false;

            JObject frame;
            try
            {
                frame = JObject.Parse(json);
            }
            catch (Exception)
            {
                return false;
            }

            if ((string)frame["type"] != "identifyResponse")
                return false;

            JToken error = frame["error"];
            if (error == null || error.Type == JTokenType.Null)
                error = (frame["payload"] as JObject)?["error"];
            if (error == null || error.Type == JTokenType.Null)
                return false;

            if (error is JObject errorObject)
            {
                code = (string)errorObject["code"];
                reason = ExtractErrorMessage(errorObject);
            }
            else
            {
                reason = NormalizeAndBound(error.ToString(), MaxMessageLength);
            }
            return true;
        }

        private static string NormalizeAndBound(string value, int maximumLength)
        {
            var result = new StringBuilder(Math.Min(value.Length, maximumLength));
            bool pendingSpace = false;
            foreach (char character in value)
            {
                if (char.IsWhiteSpace(character))
                {
                    pendingSpace = result.Length > 0;
                    continue;
                }

                if (pendingSpace && result.Length < maximumLength)
                    result.Append(' ');
                pendingSpace = false;
                if (result.Length >= maximumLength)
                    break;
                result.Append(character);
            }

            return result.ToString();
        }
    }
}
