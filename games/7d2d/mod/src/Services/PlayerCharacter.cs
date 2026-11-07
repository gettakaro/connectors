using System.Collections.Generic;
using Newtonsoft.Json;

namespace Takaro.Services
{
    public class TakaroCharacter
    {
        [JsonProperty("id")]
        public string Id { get; set; }

        [JsonProperty("name")]
        public string Name { get; set; }
    }

    /// <summary>
    /// A 7 Days to Die account has one avatar per save, so Takaro's implicit Character fits
    /// every server except one set to the Permanent Death penalty: there a death resets the
    /// player to a new character under the same account. Only those servers report characters.
    /// The game has no character id, so the id is the account plus the number of lives ended.
    /// </summary>
    public static class PlayerCharacter
    {
        public const string Capability = "players.characters";

        public static string Id(string gameId, int life) => $"{gameId}-{life + 1}";

        public static Dictionary<string, object> BuildCapability() =>
            new Dictionary<string, object>
            {
                { "name", Capability },
                {
                    "parameters",
                    new Dictionary<string, object>
                    {
                        { "held", "server" },
                        { "ends", new[] { "death" } },
                    }
                },
            };
    }
}
