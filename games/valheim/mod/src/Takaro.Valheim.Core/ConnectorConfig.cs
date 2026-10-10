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
    public const string DefaultChatSenderName = "Takaro";
    public const int MaximumChatSenderNameCharacters = 128;

    public string ChatSenderName { get; init; } = DefaultChatSenderName;

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

    /// <summary>
    /// Builds the settings from resolved key/value pairs. An empty registrationToken is valid: the
    /// connector then waits for one instead of connecting.
    /// </summary>
    public static ConnectorConfig FromDictionary(IReadOnlyDictionary<string, string> values)
    {
        var serverName = Optional(values, "serverName") ?? ConnectorConfigFiles.DefaultServerName;
        return new ConnectorConfig(
            RegistrationToken: Optional(values, "registrationToken") ?? string.Empty,
            ServerName: serverName,
            IdentityToken: Optional(values, "identityToken") ?? serverName,
            TakaroWsUrl: Optional(values, "takaroWsUrl") ?? ConnectorConfigFiles.DefaultTakaroWsUrl,
            LogLevel: Optional(values, "logLevel") ?? "Information",
            EnableLogEvents: ParseBool(Optional(values, "enableLogEvents"), defaultValue: true),
            CommandAllowlistExact: ParseList(Optional(values, "commandAllowlistExact"), defaultValues: new[] { "help" }),
            CommandAllowlistPrefixes: ParseList(Optional(values, "commandAllowlistPrefixes"), defaultValues: Array.Empty<string>()))
        {
            ChatSenderName = ParseChatSenderName(Optional(values, "chatSenderName"))
        };
    }

    public bool HasRegistrationToken => !string.IsNullOrWhiteSpace(RegistrationToken);

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

    private static string ParseChatSenderName(string? value)
    {
        if (value is null)
        {
            return DefaultChatSenderName;
        }

        if (value.Length > MaximumChatSenderNameCharacters)
        {
            throw new ArgumentException(
                $"Valheim Takaro chatSenderName must contain at most {MaximumChatSenderNameCharacters} characters.");
        }

        return value;
    }

    private static IReadOnlyList<string> ParseList(string? value, IReadOnlyList<string> defaultValues)
    {
        if (value is null)
        {
            return defaultValues.Distinct(StringComparer.Ordinal).ToArray();
        }

        return value
            .Split(new[] { ';', ',' }, StringSplitOptions.RemoveEmptyEntries)
            .Select(entry => entry.Trim())
            .Where(entry => !string.IsNullOrWhiteSpace(entry))
            .Distinct(StringComparer.Ordinal)
            .ToArray();
    }
}
