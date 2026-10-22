using Takaro.Valheim.Companion.Protocol;

namespace Takaro.Valheim.Companion;

// Reference-free parts of the native-command research probe (gettakaro/connectors#200). The
// game-bound half is CompanionCommandProbe; nothing here runs unless the marker file exists.

public sealed record CommandProbeClientSettings(string RootCommand, bool EnableConsole)
{
    public static readonly CommandProbeClientSettings Default = new(CommandProbeProtocol.DefaultRootCommand, false);

    /// <summary>
    /// <c>root = takaro</c> (any other valid name, e.g. <c>say</c>, exercises the collision
    /// refusal) and <c>console = on</c> (enables the F5 console for this session).
    /// </summary>
    public static CommandProbeClientSettings Parse(string? markerText)
    {
        var root = CommandProbeProtocol.DefaultRootCommand;
        var console = false;
        foreach (var rawLine in (markerText ?? string.Empty).Split('\n'))
        {
            var line = rawLine.Trim();
            var separator = line.IndexOf('=');
            if (line.StartsWith("#", StringComparison.Ordinal) || separator <= 0)
            {
                continue;
            }

            var key = line.Substring(0, separator).Trim().ToLowerInvariant();
            var value = line.Substring(separator + 1).Trim();
            if (key == "root" && CommandProbeProtocol.IsCommandName(value.ToLowerInvariant()))
            {
                root = value.ToLowerInvariant();
            }
            else if (key == "console")
            {
                console = string.Equals(value, "on", StringComparison.OrdinalIgnoreCase);
            }
        }

        return new CommandProbeClientSettings(root, console);
    }
}

/// <summary>
/// The stub catalog the server pushed for the current session. Replaced as one immutable view
/// (a single reference write), so the tab-option fetcher never sees a half-applied catalog.
/// A view is bound to one session nonce; an older revision for the same nonce is refused.
/// </summary>
public sealed class CommandProbeCatalogSnapshot
{
    private volatile View? current;

    public View? Current => current;

    public bool TryReplace(string nonce, CommandProbeCatalog catalog)
    {
        if (string.IsNullOrEmpty(nonce) || catalog is null)
        {
            return false;
        }

        var existing = current;
        if (existing is not null
            && string.Equals(existing.Nonce, nonce, StringComparison.Ordinal)
            && catalog.Revision <= existing.Revision)
        {
            return false;
        }

        current = new View(nonce, catalog.Revision, catalog.Commands.ToArray());
        return true;
    }

    public void Clear() => current = null;

    /// <summary>Commands of the view for <paramref name="nonce"/>; empty for any other session.</summary>
    public IReadOnlyList<string> CommandsFor(string? nonce)
    {
        var view = current;
        return view is not null && string.Equals(view.Nonce, nonce, StringComparison.Ordinal)
            ? view.Commands
            : Array.Empty<string>();
    }

    public sealed record View(string Nonce, long Revision, IReadOnlyList<string> Commands);
}

public enum CommandProbeParseResult
{
    Ok,
    MissingCommand,
    LineTooLong,
    InvalidCommand,
    TooManyArguments,
    InvalidArgument
}

public static class CommandProbeLineParser
{
    /// <summary>
    /// Parses the terminal's full line (<c>takaro ping a b</c>, the slash already stripped).
    /// The root token is whatever the terminal matched; the command is lower-cased.
    /// </summary>
    public static CommandProbeParseResult TryParse(
        string? fullLine,
        out string command,
        out IReadOnlyList<string> arguments)
    {
        command = string.Empty;
        arguments = Array.Empty<string>();
        if (fullLine is null)
        {
            return CommandProbeParseResult.MissingCommand;
        }

        if (fullLine.Length > CommandProbeProtocol.MaximumLineCharacters)
        {
            return CommandProbeParseResult.LineTooLong;
        }

        var tokens = fullLine.Split(new[] { ' ' }, StringSplitOptions.RemoveEmptyEntries);
        if (tokens.Length < 2)
        {
            return CommandProbeParseResult.MissingCommand;
        }

        var name = tokens[1].ToLowerInvariant();
        if (!CommandProbeProtocol.IsCommandName(name))
        {
            return CommandProbeParseResult.InvalidCommand;
        }

        if (tokens.Length - 2 > CommandProbeProtocol.MaximumArguments)
        {
            return CommandProbeParseResult.TooManyArguments;
        }

        var rest = tokens.Skip(2).ToArray();
        if (!rest.All(CommandProbeProtocol.IsArgument))
        {
            return CommandProbeParseResult.InvalidArgument;
        }

        command = name;
        arguments = rest;
        return CommandProbeParseResult.Ok;
    }
}

/// <summary>
/// Invocations sent and waiting for a receipt. Bounded, keyed by session nonce + id. A reset
/// (disconnect, new session) drops everything: pending input is reported as lost, never replayed.
/// </summary>
public sealed class CommandProbePendingInvocations
{
    public const int Capacity = 8;
    public static readonly TimeSpan ReceiptTimeout = TimeSpan.FromSeconds(10);

    private readonly List<Pending> pending = new();

    public int Count => pending.Count;

    public bool TryAdd(string nonce, string invocationId, string command, TimeSpan now, object? context)
    {
        if (pending.Count >= Capacity
            || pending.Any(p => p.Nonce == nonce && p.InvocationId == invocationId))
        {
            return false;
        }

        pending.Add(new Pending(nonce, invocationId, command, now + ReceiptTimeout, context));
        return true;
    }

    /// <summary>True exactly once per sent invocation of the current session.</summary>
    public bool TryComplete(string nonce, string invocationId, out Pending? completed)
    {
        var index = pending.FindIndex(p => p.Nonce == nonce && p.InvocationId == invocationId);
        completed = index >= 0 ? pending[index] : null;
        if (index >= 0)
        {
            pending.RemoveAt(index);
        }

        return completed is not null;
    }

    public IReadOnlyList<Pending> Expire(TimeSpan now)
    {
        var expired = pending.Where(p => now >= p.Deadline).ToArray();
        pending.RemoveAll(p => now >= p.Deadline);
        return expired;
    }

    public IReadOnlyList<Pending> Clear()
    {
        var dropped = pending.ToArray();
        pending.Clear();
        return dropped;
    }

    public sealed record Pending(string Nonce, string InvocationId, string Command, TimeSpan Deadline, object? Context);
}

/// <summary>
/// The terminal command table is one dictionary and its constructor overwrites an existing
/// name, so the probe checks first and only ever removes the exact object it installed.
/// </summary>
public static class CommandProbeRegistrationPolicy
{
    public static bool CanRegister<T>(IDictionary<string, T> commands, string root) =>
        commands is not null
        && CommandProbeProtocol.IsCommandName(root)
        && !commands.ContainsKey(root.ToLowerInvariant());

    public static bool TryRemoveOwned<T>(IDictionary<string, T> commands, string root, T owned)
        where T : class
    {
        var key = root.ToLowerInvariant();
        if (commands is null
            || owned is null
            || !commands.TryGetValue(key, out var current)
            || !ReferenceEquals(current, owned))
        {
            return false;
        }

        return commands.Remove(key);
    }

    public static bool StillOwned<T>(IDictionary<string, T> commands, string root, T owned)
        where T : class =>
        commands is not null
        && commands.TryGetValue(root.ToLowerInvariant(), out var current)
        && ReferenceEquals(current, owned);
}

/// <summary>Server-to-client probe envelopes must count up within a session.</summary>
public sealed class CommandProbeServerSequence
{
    private string? nonce;
    private long last;

    public bool TryAccept(string envelopeNonce, long sequence)
    {
        if (!string.Equals(nonce, envelopeNonce, StringComparison.Ordinal))
        {
            nonce = envelopeNonce;
            last = 1; // the hello
        }

        if (sequence <= last)
        {
            return false;
        }

        last = sequence;
        return true;
    }

    public void Reset()
    {
        nonce = null;
        last = 0;
    }
}
