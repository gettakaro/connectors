namespace Takaro.Cs2.Core;

public sealed class ConnectorConfig
{
    public string TakaroWsUrl { get; init; } = "wss://connect.takaro.io/";
    public string ServerName { get; init; } = "Counter-Strike 2";
    public string? IdentityToken { get; init; }
    public string? RegistrationToken { get; init; }
    public bool IncludeBots { get; init; }
    public bool AllowShutdown { get; init; }
    public IReadOnlyList<string> CommandAllowlistExact { get; init; } = ["sv_cheats"];
    public IReadOnlyList<string> CommandAllowlistPrefixes { get; init; } = [];

    public IReadOnlyList<string> Validate()
    {
        var errors = new List<string>();
        if (string.IsNullOrWhiteSpace(TakaroWsUrl))
        {
            errors.Add("TakaroWsUrl is required.");
        }
        else if (!Uri.TryCreate(TakaroWsUrl, UriKind.Absolute, out var uri)
                 || (uri.Scheme != "wss" && uri.Scheme != "ws"))
        {
            errors.Add("TakaroWsUrl must be an absolute ws or wss URL.");
        }

        if (string.IsNullOrWhiteSpace(ServerName))
        {
            errors.Add("ServerName is required.");
        }

        if (string.IsNullOrWhiteSpace(IdentityToken))
        {
            errors.Add("IdentityToken is required and must be stable for this server.");
        }

        if (IncludeBots)
        {
            errors.Add("IncludeBots is unavailable: CS2 bots have no authenticated SteamID64.");
        }

        return errors;
    }

    public CommandAllowlist CreateCommandAllowlist() =>
        new(CommandAllowlistExact, CommandAllowlistPrefixes);
}
