namespace Takaro.Valheim.Companion.Protocol;

/// <summary>
/// Research probe for native Takaro commands (gettakaro/connectors#200). Off by default on both
/// sides: the server only answers and the companion only registers <c>/takaro</c> when the
/// <see cref="MarkerFileName"/> file exists in <c>BepInEx/config</c>. Not a production contract:
/// there is no capability bit, no Takaro catalog and no command result, and a receipt only
/// means the server accepted the envelope, never that Takaro executed anything.
/// </summary>
public static class CommandProbeProtocol
{
    public const string MarkerFileName = "takaro-command-probe.txt";
    public const string DefaultRootCommand = "takaro";
    public const string SubmittedStatus = "submitted";

    public const int MaximumInvocationIdCharacters = 32;
    public const int MaximumCommandCharacters = 32;
    public const int MaximumArguments = 8;
    public const int MaximumArgumentCharacters = 64;
    public const int MaximumCatalogCommands = 64;
    public const int MaximumLineCharacters = 256;

    /// <summary>Lower-case command names: <c>[a-z0-9_-]{1,32}</c>.</summary>
    public static bool IsCommandName(string? value) =>
        IsToken(value, MaximumCommandCharacters, allowUpper: false);

    /// <summary>Invocation ids: <c>[A-Za-z0-9-]{1,32}</c>.</summary>
    public static bool IsInvocationId(string? value) =>
        IsToken(value, MaximumInvocationIdCharacters, allowUpper: true, allowUnderscore: false);

    public static bool IsArgument(string? value) =>
        !string.IsNullOrEmpty(value)
        && value!.Length <= MaximumArgumentCharacters
        && value.All(c => !char.IsControl(c) && !char.IsWhiteSpace(c));

    private static bool IsToken(string? value, int maximum, bool allowUpper, bool allowUnderscore = true)
    {
        if (string.IsNullOrEmpty(value) || value!.Length > maximum)
        {
            return false;
        }

        foreach (var c in value)
        {
            var ok = (c >= 'a' && c <= 'z')
                || (c >= '0' && c <= '9')
                || c == '-'
                || (allowUnderscore && c == '_')
                || (allowUpper && c >= 'A' && c <= 'Z');
            if (!ok)
            {
                return false;
            }
        }

        return true;
    }
}

/// <summary>Server to client: the whole stub catalog, replacing any older revision.</summary>
public sealed record CommandProbeCatalog(long Revision, IReadOnlyList<string> Commands);

/// <summary>
/// Client to server: one invocation. Carries no identity, role or permission; the server binds
/// it to the peer and session it arrived on.
/// </summary>
public sealed record CommandProbeInvocation(
    string InvocationId,
    string Command,
    IReadOnlyList<string> Arguments);

/// <summary>Server to client: the envelope with this invocation id was accepted.</summary>
public sealed record CommandProbeReceipt(string InvocationId, string Status);
