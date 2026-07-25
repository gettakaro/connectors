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
