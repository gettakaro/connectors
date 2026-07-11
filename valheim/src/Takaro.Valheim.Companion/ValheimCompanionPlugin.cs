#if TAKARO_VALHEIM_COMPANION
using BepInEx;
using HarmonyLib;
using System.Diagnostics;
using UnityEngine;

namespace Takaro.Valheim.Companion;

[BepInPlugin(PluginGuid, PluginName, PluginVersion)]
public sealed class ValheimCompanionPlugin : BaseUnityPlugin
{
    public const string PluginGuid = "com.takaro.valheim.companion";
    public const string PluginName = "Takaro Valheim Companion";
    public const string PluginVersion = TakaroCompanionBuildVersion.BepInExVersion;
    public const string ProductVersion = TakaroCompanionBuildVersion.ProductVersion;
    public const int ProtocolVersion = TakaroCompanionBuildVersion.ProtocolVersion;

    private CompanionClientBridge? clientBridge;
    private Harmony? harmony;

    private void Awake()
    {
        if (!IsGraphicalValheimClient())
        {
            Logger.LogWarning("Takaro Valheim Companion only runs in the graphical Valheim client; plugin disabled.");
            enabled = false;
            return;
        }

        harmony = new Harmony(PluginGuid);
        harmony.PatchAll(typeof(ValheimCompanionPlugin).Assembly);
        clientBridge = new CompanionClientBridge(Logger.LogInfo);
        clientBridge.Initialize();
        Logger.LogInfo($"Takaro Valheim Companion {ProductVersion} started with protocol {ProtocolVersion}.");
    }

    private void Update()
    {
        clientBridge?.Update();
    }

    private void OnDestroy()
    {
        clientBridge?.Dispose();
        clientBridge = null;
        harmony?.UnpatchSelf();
        harmony = null;
    }

    private static bool IsGraphicalValheimClient()
    {
        using var process = Process.GetCurrentProcess();
        return CompanionRuntimePolicy.IsGraphicalValheimClient(
            Application.isBatchMode,
            process.ProcessName,
            Environment.GetCommandLineArgs().FirstOrDefault());
    }
}
#else
namespace Takaro.Valheim.Companion;

public sealed class ValheimCompanionPlugin
{
    public const string PluginGuid = "com.takaro.valheim.companion";
    public const string PluginName = "Takaro Valheim Companion";
    public const string PluginVersion = TakaroCompanionBuildVersion.BepInExVersion;
    public const string ProductVersion = TakaroCompanionBuildVersion.ProductVersion;
    public const int ProtocolVersion = TakaroCompanionBuildVersion.ProtocolVersion;

    public static string BuildMode =>
        "Reference-free scaffold. Build with EnableValheimCompanionBuild=true and Valheim/BepInEx references for the real client plugin.";
}
#endif
