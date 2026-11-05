#if TAKARO_VALHEIM_COMPANION
using System.Diagnostics;
using System.Reflection;
using HarmonyLib;
using Takaro.Valheim.Companion.Protocol;

namespace Takaro.Valheim.Companion;

/// <summary>
/// Research probe for native Takaro commands (gettakaro/connectors#200). Created only when
/// <c>BepInEx/config/takaro-command-probe.txt</c> exists on this client. It registers one
/// root terminal command (default <c>/takaro</c>) after the terminal initialised, offers the
/// server's stub catalog as tab options, and forwards an invocation through the negotiated
/// companion session. Its private replies never become chat. Every game-version-sensitive
/// member is looked up first; if any is missing the probe does nothing (fails closed).
/// </summary>
internal sealed class CompanionCommandProbe : IDisposable
{
    private static readonly FieldInfo? CommandsField = AccessTools.Field(typeof(Terminal), "commands");
    private static readonly FieldInfo? TerminalInitializedField = AccessTools.Field(typeof(Terminal), "m_terminalInitialized");
    private static readonly MethodInfo? UpdateCommandListMethod = AccessTools.Method(typeof(Terminal), "updateCommandList");

    private static CompanionCommandProbe? active;

    private readonly CompanionClientBridge bridge;
    private readonly CommandProbeClientSettings settings;
    private readonly Action<string> log;
    private readonly Stopwatch clock = Stopwatch.StartNew();
    private readonly CommandProbeCatalogSnapshot catalog = new();
    private readonly CommandProbePendingInvocations pending = new();
    private readonly CommandProbeServerSequence serverSequence = new();
    private Terminal.ConsoleCommand? owned;
    private bool registrationSettled;
    private bool lostOwnershipLogged;

    // Audit counters for the current input line (main thread only).
    private int auditLine;
    private bool auditActive;
    private string auditContext = string.Empty;
    private int auditCallbacks;
    private int auditTalkerSay;
    private int auditChatSendText;
    private int routedChatRpcTotal;

    private CompanionCommandProbe(
        CompanionClientBridge bridge,
        CommandProbeClientSettings settings,
        Action<string> log)
    {
        this.bridge = bridge;
        this.settings = settings;
        this.log = log;
    }

    public static CompanionCommandProbe? TryCreate(
        string markerPath,
        CompanionClientBridge bridge,
        Harmony harmony,
        Action<string> log)
    {
        if (!File.Exists(markerPath))
        {
            return null;
        }

        CommandProbeClientSettings settings;
        try
        {
            settings = CommandProbeClientSettings.Parse(File.ReadAllText(markerPath));
        }
        catch (Exception ex)
        {
            log($"Takaro command probe (research): marker file unreadable ({ex.Message}); probe stays off.");
            return null;
        }

        if (CommandsField is null || TerminalInitializedField is null || UpdateCommandListMethod is null
            || CommandsField.GetValue(null) is not Dictionary<string, Terminal.ConsoleCommand>)
        {
            log($"Takaro command probe (research): this Valheim build does not have the expected terminal members (commands={CommandsField is not null}, m_terminalInitialized={TerminalInitializedField is not null}, updateCommandList={UpdateCommandListMethod is not null}); probe stays off.");
            return null;
        }

        var probe = new CompanionCommandProbe(bridge, settings, log);
        try
        {
            CommandProbeAuditPatches.Apply(harmony);
        }
        catch (Exception ex)
        {
            log($"Takaro command probe (research): audit patches failed ({ex.Message}); probe stays off.");
            return null;
        }

        if (settings.EnableConsole)
        {
            global::Console.SetConsoleEnabledForThisSession();
        }

        active = probe;
        bridge.AttachCommandProbe(probe);
        log($"Takaro command probe (research) armed: root '/{settings.RootCommand}', console {(settings.EnableConsole ? "enabled" : "unchanged")}. Valheim {(global::Version.CurrentVersion)}.");
        return probe;
    }

    private static Dictionary<string, Terminal.ConsoleCommand>? Commands =>
        CommandsField?.GetValue(null) as Dictionary<string, Terminal.ConsoleCommand>;

    public void Update()
    {
        if (!registrationSettled && TerminalInitializedField?.GetValue(null) is true)
        {
            registrationSettled = true;
            TryRegister();
        }

        if (owned is not null && !lostOwnershipLogged && Commands is { } commands
            && !CommandProbeRegistrationPolicy.StillOwned(commands, settings.RootCommand, owned))
        {
            lostOwnershipLogged = true;
            log($"Takaro command probe: '/{settings.RootCommand}' was replaced by another registration after ours; the probe no longer owns it and will not remove it.");
        }

        foreach (var expired in pending.Expire(clock.Elapsed))
        {
            Reply(expired.Context, $"Takaro: no receipt for '{expired.Command}' ({expired.InvocationId}) within {CommandProbePendingInvocations.ReceiptTimeout.TotalSeconds:0} s. It was not retried; it may or may not have arrived.");
            log($"Takaro command probe: invocation {expired.InvocationId} expired without a receipt; not retried.");
        }
    }

    public void Dispose()
    {
        if (ReferenceEquals(active, this))
        {
            active = null;
        }

        if (owned is not null && Commands is { } commands)
        {
            var removed = CommandProbeRegistrationPolicy.TryRemoveOwned(commands, settings.RootCommand, owned);
            log($"Takaro command probe teardown: '/{settings.RootCommand}' {(removed ? "removed (it was ours)" : "left in place (not ours any more)")}.");
            RefreshCommandLists();
        }

        owned = null;
    }

    private void TryRegister()
    {
        var commands = Commands;
        if (commands is null)
        {
            return;
        }

        if (!CommandProbeRegistrationPolicy.CanRegister(commands, settings.RootCommand))
        {
            var existing = commands.TryGetValue(settings.RootCommand, out var current) ? current.Description : "unknown owner";
            log($"Takaro command probe: '/{settings.RootCommand}' already exists ({existing}); refusing to replace it. Probe command not registered.");
            return;
        }

        try
        {
            var command = new Terminal.ConsoleCommand(
                settings.RootCommand,
                "Takaro commands (research probe)",
                (Terminal.ConsoleEvent)OnCommand,
                optionsFetcher: TabOptions,
                alwaysRefreshTabOptions: true);
            if (!CommandProbeRegistrationPolicy.StillOwned(commands, settings.RootCommand, command))
            {
                log("Takaro command probe: the terminal constructor did not register the command as expected; probe stays off.");
                return;
            }

            owned = command;
            RefreshCommandLists();
            log($"Takaro command probe registered '/{settings.RootCommand}' ({commands.Count} terminal commands now).");
        }
        catch (Exception ex)
        {
            log($"Takaro command probe: terminal command constructor failed ({ex.GetType().Name}: {ex.Message}); probe stays off.");
        }
    }

    // Terminal caches its root-command list per instance and only rebuilds it when empty.
    private static void RefreshCommandLists()
    {
        foreach (Terminal? terminal in new Terminal?[] { Chat.instance, global::Console.instance })
        {
            if (terminal != null)
            {
                UpdateCommandListMethod?.Invoke(terminal, null);
            }
        }
    }

    private List<string> TabOptions() => new(catalog.CommandsFor(bridge.ActiveSessionNonce));

    private void OnCommand(Terminal.ConsoleEventArgs args)
    {
        auditCallbacks++;
        var context = args.Context;
        var contextName = context is Chat ? "chat" : context is global::Console ? "console" : context?.GetType().Name ?? "none";
        var parse = CommandProbeLineParser.TryParse(args.FullLine, out var command, out var arguments);
        if (parse != CommandProbeParseResult.Ok)
        {
            Reply(context, parse == CommandProbeParseResult.MissingCommand
                ? $"Takaro: usage /{settings.RootCommand} <command> [arguments]. Press Tab after the space for suggestions."
                : $"Takaro: input refused ({parse}); at most {CommandProbeProtocol.MaximumArguments} arguments of {CommandProbeProtocol.MaximumArgumentCharacters} characters, {CommandProbeProtocol.MaximumLineCharacters} characters in all.");
            log($"Takaro command probe [{contextName}]: refused locally ({parse}).");
            return;
        }

        var nonce = bridge.ActiveSessionNonce;
        if (nonce is null)
        {
            Reply(context, "Takaro: not connected to a server with Takaro. The command was not sent and will not be retried.");
            log($"Takaro command probe [{contextName}]: '{command}' refused offline (no negotiated companion session).");
            return;
        }

        var commands = catalog.CommandsFor(nonce);
        if (catalog.Current is null
            || !string.Equals(catalog.Current.Nonce, nonce, StringComparison.Ordinal)
            || commands.Count == 0)
        {
            Reply(context, "Takaro: this server does not accept Takaro commands (no catalog). Nothing was sent.");
            log($"Takaro command probe [{contextName}]: '{command}' refused, no catalog for this session.");
            return;
        }

        if (!commands.Contains(command))
        {
            Reply(context, $"Takaro: unknown command '{command}'. Nothing was sent.");
            log($"Takaro command probe [{contextName}]: unknown command '{command}' refused locally (catalog revision {catalog.Current.Revision}).");
            return;
        }

        var invocationId = Guid.NewGuid().ToString("N").Substring(0, 16);
        var invocation = new CommandProbeInvocation(invocationId, command, arguments);
        if (!pending.TryAdd(nonce, invocationId, command, clock.Elapsed, context))
        {
            Reply(context, "Takaro: too many commands waiting for the server. Nothing was sent.");
            return;
        }

        if (!bridge.TrySendCommandProbeInvocation(invocation, nonce))
        {
            pending.TryComplete(nonce, invocationId, out _);
            Reply(context, "Takaro: could not send to the server. Nothing was sent and it will not be retried.");
            log($"Takaro command probe [{contextName}]: send of '{command}' failed.");
            return;
        }

        Reply(context, $"Takaro: sending '{command}'...");
        log($"Takaro command probe [{contextName}]: sent invocation {invocationId} command={command} arguments={arguments.Count} on session {Short(nonce)}.");
    }

    /// <summary>Called by the bridge for a probe envelope from the connected server peer.</summary>
    public void OnServerEnvelope(CompanionEnvelope envelope, string? activeNonce)
    {
        if (activeNonce is null
            || !string.Equals(envelope.SessionNonce, activeNonce, StringComparison.Ordinal)
            || !serverSequence.TryAccept(envelope.SessionNonce, envelope.Sequence))
        {
            log($"Takaro command probe ignored a {envelope.Type} for {(activeNonce is null ? "no session" : "another session or an old sequence")}.");
            return;
        }

        if (envelope.Type == CompanionMessageTypes.CommandProbeCatalog
            && CompanionEnvelopeCodec.TryDecodePayload<CommandProbeCatalog>(envelope, out var stub, out _)
            && stub is not null)
        {
            if (catalog.TryReplace(activeNonce, stub))
            {
                log($"Takaro command probe catalog revision {stub.Revision} applied: [{string.Join(", ", stub.Commands)}].");
            }

            return;
        }

        if (envelope.Type == CompanionMessageTypes.CommandProbeReceipt
            && CompanionEnvelopeCodec.TryDecodePayload<CommandProbeReceipt>(envelope, out var receipt, out _)
            && receipt is not null)
        {
            if (pending.TryComplete(activeNonce, receipt.InvocationId, out var completed) && completed is not null)
            {
                Reply(completed.Context, $"Takaro: '{completed.Command}' submitted to the server ({receipt.InvocationId}). Submitted is not executed.");
                log($"Takaro command probe receipt for {receipt.InvocationId}: {receipt.Status} (session {Short(activeNonce)}).");
            }
            else
            {
                log($"Takaro command probe ignored a receipt for unknown or finished invocation {receipt.InvocationId}.");
            }
        }
    }

    /// <summary>The companion session ended or was replaced: drop, never replay.</summary>
    public void OnSessionReset(string reason)
    {
        var dropped = pending.Clear();
        catalog.Clear();
        serverSequence.Reset();
        foreach (var lost in dropped)
        {
            Reply(lost.Context, $"Takaro: connection changed before '{lost.Command}' was confirmed. It was not retried.");
        }

        if (dropped.Count > 0)
        {
            log($"Takaro command probe dropped {dropped.Count} unconfirmed invocation(s) on session reset ({reason}); none will be replayed.");
        }
    }

    private static void Reply(object? context, string message)
    {
        if (context is Terminal terminal && terminal != null)
        {
            terminal.AddString(message);
        }
    }

    private static string Short(string nonce) => nonce.Length <= 8 ? nonce : nonce.Substring(0, 8);

    internal static void BeginAudit(Terminal terminal)
    {
        var probe = active;
        if (probe is null || terminal == null)
        {
            return;
        }

        var text = InputText(terminal);
        var isChat = terminal is Chat;
        if (isChat && !text.StartsWith("/", StringComparison.Ordinal))
        {
            return;
        }

        probe.auditActive = true;
        probe.auditLine++;
        probe.auditContext = isChat ? "chat" : "console";
        probe.auditCallbacks = 0;
        probe.auditTalkerSay = 0;
        probe.auditChatSendText = 0;
    }

    internal static void EndAudit(Terminal terminal)
    {
        var probe = active;
        if (probe is null || !probe.auditActive)
        {
            return;
        }

        probe.auditActive = false;
        var firstToken = InputText(terminal).Split(' ')[0];
        probe.log($"Takaro command probe audit line {probe.auditLine} [{probe.auditContext}] root='{firstToken}': probe callbacks={probe.auditCallbacks} Talker.Say={probe.auditTalkerSay} Chat.SendText={probe.auditChatSendText} routed Say/ChatMessage sent since start={probe.routedChatRpcTotal}.");
    }

    // m_input is a GuiInputField (gui_framework/TextMeshPro); read it without referencing those.
    private static string InputText(Terminal? terminal) =>
        terminal == null
            ? string.Empty
            : Traverse.Create(terminal).Field("m_input").Property("text").GetValue<string>() ?? string.Empty;

    internal static void CountTalkerSay()
    {
        if (active is { auditActive: true } probe)
        {
            probe.auditTalkerSay++;
        }
    }

    internal static void CountChatSendText()
    {
        if (active is { auditActive: true } probe)
        {
            probe.auditChatSendText++;
        }
    }

    internal static void CountRoutedRpc(string? methodName)
    {
        if (active is { } probe && (methodName == "Say" || methodName == "ChatMessage"))
        {
            probe.routedChatRpcTotal++;
        }
    }
}

/// <summary>
/// Probe-only instrumentation, patched by hand (not PatchAll) and only when the probe is armed.
/// It counts, per slash/console line, how often the probe callback ran and whether anything
/// tried to say the line as chat.
/// </summary>
internal static class CommandProbeAuditPatches
{
    public static void Apply(Harmony harmony)
    {
        var prefix = new HarmonyMethod(AccessTools.Method(typeof(CommandProbeAuditPatches), nameof(InputTextPrefix)));
        var finalizer = new HarmonyMethod(AccessTools.Method(typeof(CommandProbeAuditPatches), nameof(InputTextFinalizer)));
        harmony.Patch(Required(AccessTools.Method(typeof(Chat), "InputText")), prefix: prefix, finalizer: finalizer);
        harmony.Patch(Required(AccessTools.Method(typeof(Terminal), "InputText")), prefix: prefix, finalizer: finalizer);
        harmony.Patch(
            Required(AccessTools.Method(typeof(Talker), nameof(Talker.Say), new[] { typeof(Talker.Type), typeof(string) })),
            prefix: new HarmonyMethod(AccessTools.Method(typeof(CommandProbeAuditPatches), nameof(TalkerSayPrefix))));
        harmony.Patch(
            Required(AccessTools.Method(typeof(Chat), nameof(Chat.SendText), new[] { typeof(Talker.Type), typeof(string) })),
            prefix: new HarmonyMethod(AccessTools.Method(typeof(CommandProbeAuditPatches), nameof(ChatSendTextPrefix))));
        harmony.Patch(
            Required(AccessTools.Method(typeof(ZRoutedRpc), nameof(ZRoutedRpc.InvokeRoutedRPC), new[] { typeof(long), typeof(ZDOID), typeof(string), typeof(object[]) })),
            prefix: new HarmonyMethod(AccessTools.Method(typeof(CommandProbeAuditPatches), nameof(InvokeRoutedRpcPrefix))));
    }

    private static MethodInfo Required(MethodInfo? method) =>
        method ?? throw new MissingMethodException("audit target not found");

    private static void InputTextPrefix(Terminal __instance) => CompanionCommandProbe.BeginAudit(__instance);

    private static Exception? InputTextFinalizer(Terminal __instance, Exception? __exception)
    {
        CompanionCommandProbe.EndAudit(__instance);
        return __exception;
    }

    private static void TalkerSayPrefix() => CompanionCommandProbe.CountTalkerSay();

    private static void ChatSendTextPrefix() => CompanionCommandProbe.CountChatSendText();

    private static void InvokeRoutedRpcPrefix(string methodName) => CompanionCommandProbe.CountRoutedRpc(methodName);
}
#endif
