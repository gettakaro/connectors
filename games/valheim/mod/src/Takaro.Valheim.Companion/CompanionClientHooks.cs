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
    private static readonly CompanionCombatReader combatReader = new();
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
        combatReader.Reset();
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

    public static void OnLocalPlayerDeath(Player __instance)
    {
        var activeBridge = bridge;
        if (activeBridge is null
            || __instance != Player.m_localPlayer)
        {
            return;
        }

        try
        {
            if (combatReader.TryCreateLocalPlayerDeath(
                    __instance,
                    MonotonicNow(),
                    DateTimeOffset.UtcNow,
                    out var report)
                && report is not null)
            {
                _ = activeBridge.TrySendPlayerDeath(report);
            }
        }
        catch (Exception ex)
        {
            log($"Takaro Valheim Companion could not report local player death: {ex.Message}");
        }
    }

    public static void OnCharacterDeath(Character character)
    {
        var activeBridge = bridge;
        if (activeBridge is null
            || character is Player
            || character.GetComponent<Player>() != null)
        {
            return;
        }

        try
        {
            var hit = CompanionCombatReader.GetLastHit(character);
            if (hit?.GetAttacker() != Player.m_localPlayer)
            {
                return;
            }

            if (combatReader.TryCreateEntityKilled(
                    character,
                    hit,
                    MonotonicNow(),
                    DateTimeOffset.UtcNow,
                    out var report)
                && report is not null)
            {
                _ = activeBridge.TrySendEntityKilled(report);
            }
        }
        catch (Exception ex)
        {
            log($"Takaro Valheim Companion could not report local entity kill: {ex.Message}");
        }
    }

    private static TimeSpan MonotonicNow() =>
        UnityEngine.Time.realtimeSinceStartup > 0
            ? TimeSpan.FromSeconds(UnityEngine.Time.realtimeSinceStartup)
            : TimeSpan.Zero;
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

[HarmonyPatch(typeof(Player), "OnDeath")]
internal static class CompanionPlayerOnDeathPatch
{
    private static void Postfix(Player __instance) =>
        CompanionClientHooks.OnLocalPlayerDeath(__instance);
}

[HarmonyPatch(typeof(Character), "OnDeath")]
internal static class CompanionCharacterOnDeathPatch
{
    private static void Postfix(Character __instance) =>
        CompanionClientHooks.OnCharacterDeath(__instance);
}
#endif
