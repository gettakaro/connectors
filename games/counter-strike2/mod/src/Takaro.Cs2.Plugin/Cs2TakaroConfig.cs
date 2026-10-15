using CounterStrikeSharp.API.Core;
using System.Text.Json.Serialization;

namespace Takaro.Cs2.Plugin;

public sealed class Cs2TakaroConfig : BasePluginConfig
{
    [JsonPropertyName("ConfigVersion")]
    public override int Version { get; set; } = 1;

    public string TakaroWsUrl { get; set; } = "wss://connect.takaro.io/";
    public string ServerName { get; set; } = "Counter-Strike 2";
    public string? IdentityToken { get; set; }
    public string? RegistrationToken { get; set; }
    public bool IncludeBots { get; set; }
    public bool AllowShutdown { get; set; }
    public List<string> CommandAllowlistExact { get; set; } = ["sv_cheats"];
    public List<string> CommandAllowlistPrefixes { get; set; } = [];

    public Takaro.Cs2.Core.ConnectorConfig ToCoreConfig() => new()
    {
        TakaroWsUrl = TakaroWsUrl,
        ServerName = ServerName,
        IdentityToken = IdentityToken,
        RegistrationToken = RegistrationToken,
        IncludeBots = IncludeBots,
        AllowShutdown = AllowShutdown,
        CommandAllowlistExact = CommandAllowlistExact,
        CommandAllowlistPrefixes = CommandAllowlistPrefixes
    };
}
