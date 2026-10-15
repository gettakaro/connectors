namespace Takaro.Cs2.Core;

public sealed record CommandAllowlist(
    IReadOnlyList<string> ExactCommands,
    IReadOnlyList<string> Prefixes)
{
    public bool IsAllowed(string? command)
    {
        var normalized = command?.Trim();
        if (string.IsNullOrWhiteSpace(normalized))
        {
            return false;
        }

        if (ExactCommands.Any(allowed => normalized.Equals(allowed.Trim(), StringComparison.OrdinalIgnoreCase)))
        {
            return true;
        }

        return Prefixes.Any(prefix =>
            !string.IsNullOrWhiteSpace(prefix)
            && normalized.StartsWith(prefix, StringComparison.OrdinalIgnoreCase));
    }
}
