namespace Takaro.Valheim.Core;

public sealed record ConnectorConfig(
    string RegistrationToken,
    string ServerName,
    string? IdentityToken,
    string TakaroWsUrl,
    string LogLevel,
    bool EnableLogEvents,
    IReadOnlyList<string> CommandAllowlistExact,
    IReadOnlyList<string> CommandAllowlistPrefixes)
{
    public static bool TryFromDictionary(
        IReadOnlyDictionary<string, string> values,
        out ConnectorConfig? config,
        out string error)
    {
        try
        {
            config = FromDictionary(values);
            error = string.Empty;
            return true;
        }
        catch (ArgumentException ex)
        {
            config = null;
            error = ex.Message;
            return false;
        }
    }

    public static ConnectorConfig FromDictionary(IReadOnlyDictionary<string, string> values)
    {
        var missing = new List<string>();
        var registrationToken = Required(values, "registrationToken", missing);
        var serverName = Required(values, "serverName", missing);

        if (missing.Count > 0)
        {
            throw new ArgumentException($"Missing required Valheim Takaro config values: {string.Join(", ", missing)}");
        }

        return new ConnectorConfig(
            RegistrationToken: registrationToken!,
            ServerName: serverName!,
            IdentityToken: Optional(values, "identityToken") ?? serverName!,
            TakaroWsUrl: Optional(values, "takaroWsUrl") ?? "wss://connect.takaro.io/",
            LogLevel: Optional(values, "logLevel") ?? "Information",
            EnableLogEvents: ParseBool(Optional(values, "enableLogEvents"), defaultValue: true),
            CommandAllowlistExact: ParseList(Optional(values, "commandAllowlistExact"), defaultValues: new[] { "help" }),
            CommandAllowlistPrefixes: ParseList(Optional(values, "commandAllowlistPrefixes"), defaultValues: Array.Empty<string>()));
    }

    private static string? Required(IReadOnlyDictionary<string, string> values, string key, List<string> missing)
    {
        var value = Optional(values, key);
        if (value is null)
        {
            missing.Add(key);
        }

        return value;
    }

    private static string? Optional(IReadOnlyDictionary<string, string> values, string key)
    {
        if (!values.TryGetValue(key, out var value))
        {
            return null;
        }

        value = value?.Trim();
        return string.IsNullOrEmpty(value) ? null : value;
    }

    private static bool ParseBool(string? value, bool defaultValue)
    {
        if (value is null)
        {
            return defaultValue;
        }

        return value.Equals("1", StringComparison.OrdinalIgnoreCase)
            || value.Equals("true", StringComparison.OrdinalIgnoreCase)
            || value.Equals("yes", StringComparison.OrdinalIgnoreCase)
            || value.Equals("on", StringComparison.OrdinalIgnoreCase);
    }

    private static IReadOnlyList<string> ParseList(string? value, IReadOnlyList<string> defaultValues)
    {
        if (value is null)
        {
            return defaultValues.ToArray();
        }

        return value
            .Split(new[] { ';', ',' }, StringSplitOptions.RemoveEmptyEntries)
            .Select(entry => entry.Trim())
            .Where(entry => !string.IsNullOrWhiteSpace(entry))
            .ToArray();
    }
}
