#if TAKARO_VALHEIM_COMPANION
using HarmonyLib;

namespace Takaro.Valheim.Companion;

internal sealed class CompanionChatHookState
{
    public static readonly CompanionChatHookState None = new(false, false, null);

    public CompanionChatHookState(
        bool suppressOriginal,
        bool reportAfterOriginal,
        string? message)
    {
        SuppressOriginal = suppressOriginal;
        ReportAfterOriginal = reportAfterOriginal;
        Message = message;
    }

    public bool SuppressOriginal { get; }

    public bool ReportAfterOriginal { get; }

    public string? Message { get; }
}

internal static class CompanionClientHooks
{
    private static CompanionClientBridge? bridge;
    private static CompanionChatPolicy policy = new(Array.Empty<string>());
    private static Action<string> log = _ => { };

    public static void Initialize(
        CompanionClientBridge clientBridge,
        string? commandPrefixes,
        Action<string>? logger)
    {
        bridge = clientBridge ?? throw new ArgumentNullException(nameof(clientBridge));
        var prefixes = CompanionChatPolicy.ParsePrefixes(commandPrefixes);
        policy = new CompanionChatPolicy(prefixes);
        log = logger ?? (_ => { });
        log($"Takaro Valheim Companion chat hooks initialized with {prefixes.Count} command prefix(es).");
    }

    public static void Shutdown()
    {
        bridge = null;
        policy = new CompanionChatPolicy(Array.Empty<string>());
        log = _ => { };
    }

    public static CompanionChatHookState BeforeTalkerSay(
        Talker __instance,
        string text)
    {
        var activeBridge = bridge;
        var isLocalPlayer = __instance.GetComponent<Player>() == Player.m_localPlayer;
        var decision = policy.Evaluate(isLocalPlayer, text);
        if (activeBridge is null || decision.Message is null)
        {
            return CompanionChatHookState.None;
        }

        if (decision.ShouldAttemptCommandReport)
        {
            var commandAccepted = activeBridge.TrySendChat(decision.Message);
            return new CompanionChatHookState(
                suppressOriginal: commandAccepted,
                reportAfterOriginal: false,
                decision.Message);
        }

        return decision.ShouldReportAfterOriginal
            ? new CompanionChatHookState(
                suppressOriginal: false,
                reportAfterOriginal: true,
                decision.Message)
            : CompanionChatHookState.None;
    }

    public static void AfterTalkerSay(CompanionChatHookState state)
    {
        var activeBridge = bridge;
        if (state.ReportAfterOriginal
            && state.Message is not null
            && activeBridge is not null)
        {
            _ = activeBridge.TrySendChat(state.Message);
        }
    }
}

[HarmonyPatch(typeof(Talker), "Say")]
internal static class CompanionTalkerSayPatch
{
    private static bool Prefix(
        Talker __instance,
        string text,
        out CompanionChatHookState __state)
    {
        __state = CompanionClientHooks.BeforeTalkerSay(__instance, text);
        return !__state.SuppressOriginal;
    }

    private static void Postfix(CompanionChatHookState __state)
    {
        if (__state.ReportAfterOriginal)
        {
            CompanionClientHooks.AfterTalkerSay(__state);
        }
    }
}
#endif
