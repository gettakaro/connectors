using Takaro.Valheim.Companion.Protocol;

namespace Takaro.Valheim.Core;

public enum CommandProbeAcceptance
{
    Accepted,
    Duplicate,
    NotInCatalog
}

/// <summary>
/// Server half of the native-command research probe (gettakaro/connectors#200). Disabled unless
/// the marker file exists. It owns the stub catalog read from that file and remembers recent
/// invocation ids per peer session, so a repeated id is counted and not receipted twice. It
/// never talks to Takaro: a receipt means "this server accepted the envelope", nothing more.
/// </summary>
public sealed class CommandProbeServer
{
    public const int RememberedInvocationsPerSession = 64;

    private readonly object syncRoot = new();
    private readonly Dictionary<long, PeerInvocations> invocations = new();
    private string? appliedMarker;
    private bool hasAppliedMarker;
    private long revision;

    public bool Enabled { get; private set; }

    /// <summary>Probe-only switch to exercise the client's no-receipt path (<c>receipts = off</c>).</summary>
    public bool ReceiptsEnabled { get; private set; } = true;

    public CommandProbeCatalog? Catalog { get; private set; }

    public int DuplicateInvocations { get; private set; }

    /// <summary>
    /// Applies the marker file text (<c>null</c> when the file is missing). Returns true when the
    /// effective settings changed; a changed catalog gets a new, higher revision.
    /// </summary>
    public bool ApplyMarker(string? markerText)
    {
        lock (syncRoot)
        {
            if (hasAppliedMarker && string.Equals(appliedMarker, markerText, StringComparison.Ordinal))
            {
                return false;
            }

            hasAppliedMarker = true;
            appliedMarker = markerText;
            if (markerText is null)
            {
                var wasEnabled = Enabled;
                Enabled = false;
                invocations.Clear();
                if (wasEnabled)
                {
                    // Push an empty catalog so connected clients stop offering commands.
                    revision = revision == long.MaxValue ? 1 : revision + 1;
                    Catalog = new CommandProbeCatalog(revision, Array.Empty<string>());
                }

                return wasEnabled;
            }

            var settings = ParseMarker(markerText);
            var commandsChanged = Catalog is null || !Catalog.Commands.SequenceEqual(settings.Commands, StringComparer.Ordinal);
            var changed = !Enabled || commandsChanged || ReceiptsEnabled != settings.ReceiptsEnabled;
            Enabled = true;
            ReceiptsEnabled = settings.ReceiptsEnabled;
            if (commandsChanged)
            {
                revision = revision == long.MaxValue ? 1 : revision + 1;
                Catalog = new CommandProbeCatalog(revision, settings.Commands);
            }

            return changed;
        }
    }

    public CommandProbeAcceptance Accept(long peerId, string sessionNonce, CommandProbeInvocation invocation)
    {
        lock (syncRoot)
        {
            if (Catalog is null || !Catalog.Commands.Contains(invocation.Command, StringComparer.Ordinal))
            {
                return CommandProbeAcceptance.NotInCatalog;
            }

            if (!invocations.TryGetValue(peerId, out var peer)
                || !string.Equals(peer.Nonce, sessionNonce, StringComparison.Ordinal))
            {
                peer = new PeerInvocations(sessionNonce);
                invocations[peerId] = peer;
            }

            if (!peer.Ids.Add(invocation.InvocationId))
            {
                DuplicateInvocations++;
                return CommandProbeAcceptance.Duplicate;
            }

            peer.Order.Enqueue(invocation.InvocationId);
            while (peer.Order.Count > RememberedInvocationsPerSession)
            {
                peer.Ids.Remove(peer.Order.Dequeue());
            }

            return CommandProbeAcceptance.Accepted;
        }
    }

    public void RemovePeer(long peerId)
    {
        lock (syncRoot)
        {
            invocations.Remove(peerId);
        }
    }

    public void Clear()
    {
        lock (syncRoot)
        {
            invocations.Clear();
        }
    }

    /// <summary>
    /// <c>catalog = ping, balance</c> (names outside <c>[a-z0-9_-]{1,32}</c> are dropped, at most
    /// 64) and <c>receipts = off</c>. Anything else is ignored.
    /// </summary>
    public static CommandProbeMarkerSettings ParseMarker(string markerText)
    {
        var commands = new List<string>();
        var receipts = true;
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
            if (key == "catalog")
            {
                foreach (var name in value.Split(new[] { ',', ';', ' ' }, StringSplitOptions.RemoveEmptyEntries))
                {
                    var normalized = name.Trim().ToLowerInvariant();
                    if (CommandProbeProtocol.IsCommandName(normalized)
                        && !commands.Contains(normalized)
                        && commands.Count < CommandProbeProtocol.MaximumCatalogCommands)
                    {
                        commands.Add(normalized);
                    }
                }
            }
            else if (key == "receipts")
            {
                receipts = !string.Equals(value, "off", StringComparison.OrdinalIgnoreCase);
            }
        }

        return new CommandProbeMarkerSettings(commands, receipts);
    }

    private sealed class PeerInvocations
    {
        public PeerInvocations(string nonce) => Nonce = nonce;

        public string Nonce { get; }

        public HashSet<string> Ids { get; } = new(StringComparer.Ordinal);

        public Queue<string> Order { get; } = new();
    }
}

public sealed record CommandProbeMarkerSettings(IReadOnlyList<string> Commands, bool ReceiptsEnabled);
