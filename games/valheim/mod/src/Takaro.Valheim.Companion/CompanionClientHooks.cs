#if TAKARO_VALHEIM_COMPANION
using HarmonyLib;
using Takaro.Valheim.Companion.Protocol;

namespace Takaro.Valheim.Companion;

/// <summary>
/// The one game hook: a creature this client owns is about to die. The prefix only reads a
/// few fields (cheap, no allocation beyond one small object); the postfix sends one
/// rate-limited verdict. Nothing waits for the server.
/// </summary>
internal static class CompanionClientHooks
{
    private static readonly System.Reflection.FieldInfo? LastHitField =
        AccessTools.Field(typeof(Character), "m_lastHit");

    private static CompanionClientBridge? bridge;
    private static Action<string> log = _ => { };
    private static bool loggedFailure;

    public static void Initialize(CompanionClientBridge clientBridge, Action<string>? logger)
    {
        bridge = clientBridge ?? throw new ArgumentNullException(nameof(clientBridge));
        log = logger ?? (_ => { });
        if (LastHitField is null)
        {
            log("Takaro Valheim Inventory Companion could not find Character.m_lastHit; kill verdicts will report lastHitAttackerKind=none.");
        }
    }

    public static void Shutdown()
    {
        bridge = null;
        log = _ => { };
    }

    public static CompanionKillObservation? Observe(Character character)
    {
        try
        {
            var localPlayer = Player.m_localPlayer;
            if (bridge is null
                || localPlayer == null
                || character == null
                || character is Player
                || character.GetComponent<Player>() != null)
            {
                return null;
            }

            var view = character.GetComponent<ZNetView>();
            if (view == null || !view.IsValid() || !view.IsOwner())
            {
                return null;
            }

            var zdo = view.GetZDO();
            if (zdo is null || !zdo.GetBool(ZDOVars.s_attackers + localPlayer.GetPlayerName()))
            {
                return null;
            }

            var prefab = ZNetScene.instance?.GetPrefab(zdo.GetPrefab());
            var prefabName = prefab != null
                ? prefab.name
                : character.gameObject.name.Replace("(Clone)", string.Empty);
            return new CompanionKillObservation(
                zdo.m_uid.UserID,
                zdo.m_uid.ID,
                prefabName,
                character.m_name,
                localPlayerMarkedAsAttacker: true,
                AttackerKind(character, localPlayer));
        }
        catch (Exception ex)
        {
            LogOnce($"Takaro Valheim Inventory Companion could not read a creature death: {ex.Message}");
            return null;
        }
    }

    public static void Report(CompanionKillObservation? observation)
    {
        var activeBridge = bridge;
        if (activeBridge is null || observation is null)
        {
            return;
        }

        try
        {
            var verdict = CompanionKillVerdictPolicy.ToVerdict(observation);
            if (verdict is not null)
            {
                _ = activeBridge.TrySendKillVerdict(verdict);
            }
        }
        catch (Exception ex)
        {
            LogOnce($"Takaro Valheim Inventory Companion could not send a kill verdict: {ex.Message}");
        }
    }

    private static string AttackerKind(Character character, Player localPlayer)
    {
        var attacker = (LastHitField?.GetValue(character) as HitData)?.GetAttacker();
        if (attacker == null)
        {
            return CompanionAttackerKind.None;
        }

        if (attacker == localPlayer)
        {
            return CompanionAttackerKind.LocalPlayer;
        }

        return attacker is Player ? CompanionAttackerKind.OtherPlayer : CompanionAttackerKind.Creature;
    }

    private static void LogOnce(string message)
    {
        if (!loggedFailure)
        {
            loggedFailure = true;
            log(message);
        }
    }
}

[HarmonyPatch(typeof(Character), "OnDeath")]
internal static class CompanionCharacterOnDeathPatch
{
    // Read before the death runs: OnDeath destroys the creature's network object.
    private static void Prefix(Character __instance, out CompanionKillObservation? __state) =>
        __state = CompanionClientHooks.Observe(__instance);

    private static void Postfix(CompanionKillObservation? __state) =>
        CompanionClientHooks.Report(__state);
}
#endif
