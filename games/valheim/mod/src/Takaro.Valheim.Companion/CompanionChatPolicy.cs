using Takaro.Valheim.Companion.Protocol;

namespace Takaro.Valheim.Companion;

public enum CompanionChatDisposition
{
    Ignore,
    Ordinary,
    Command
}

public sealed class CompanionChatDecision
{
    internal CompanionChatDecision(
        CompanionChatDisposition disposition,
        string? message)
    {
        Disposition = disposition;
        Message = message;
    }

    public CompanionChatDisposition Disposition { get; }

    public string? Message { get; }

    public bool ShouldAttemptCommandReport =>
        Disposition == CompanionChatDisposition.Command;

    public bool ShouldReportAfterOriginal =>
        Disposition == CompanionChatDisposition.Ordinary;
}

public sealed class CompanionChatPolicy
{
    private const int MaximumPrefixCharacters = 32;
    private const int MaximumPrefixes = 16;
    private static readonly CompanionChatDecision Ignore = new(
        CompanionChatDisposition.Ignore,
        null);

    private readonly string[] commandPrefixes;

    public CompanionChatPolicy(IEnumerable<string>? commandPrefixes)
    {
        this.commandPrefixes = NormalizePrefixes(commandPrefixes).ToArray();
    }

    public CompanionChatDecision Evaluate(
        bool isLocalPlayer,
        string? message)
    {
        if (!isLocalPlayer
            || string.IsNullOrWhiteSpace(message)
            || message!.Length > CompanionProtocol.MaximumChatCharacters)
        {
            return Ignore;
        }

        return commandPrefixes.Any(prefix =>
                message.StartsWith(prefix, StringComparison.Ordinal))
            ? new CompanionChatDecision(CompanionChatDisposition.Command, message)
            : new CompanionChatDecision(CompanionChatDisposition.Ordinary, message);
    }

    public static IReadOnlyList<string> ParsePrefixes(string? value) =>
        NormalizePrefixes(value?.Split(
            new[] { ';', ',' },
            StringSplitOptions.RemoveEmptyEntries)).ToArray();

    private static IEnumerable<string> NormalizePrefixes(
        IEnumerable<string>? prefixes) =>
        (prefixes ?? Array.Empty<string>())
        .Select(prefix => prefix?.Trim())
        .Where(prefix => !string.IsNullOrEmpty(prefix))
        .Select(prefix => prefix!)
        .Where(prefix => prefix.Length <= MaximumPrefixCharacters)
        .Distinct(StringComparer.Ordinal)
        .Take(MaximumPrefixes);
}
