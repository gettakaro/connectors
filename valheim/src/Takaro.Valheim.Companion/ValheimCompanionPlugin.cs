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

        try
        {
            var commandPrefixes = Config.Bind(
                "Takaro",
                "companionCommandPrefixes",
                "$",
                "Semicolon-separated chat prefixes handled as Takaro commands.").Value;
            harmony = new Harmony(PluginGuid);
            clientBridge = new CompanionClientBridge(Logger.LogInfo);
            clientBridge.Initialize();
            CompanionClientHooks.Initialize(
                clientBridge,
                commandPrefixes,
                Logger.LogInfo);
            harmony.PatchAll(typeof(ValheimCompanionPlugin).Assembly);
            Logger.LogInfo($"Takaro Valheim Companion {ProductVersion} started with protocol {ProtocolVersion}.");
        }
        catch (Exception ex)
        {
            CompanionClientHooks.Shutdown();
            clientBridge?.Dispose();
            clientBridge = null;
            harmony?.UnpatchSelf();
            harmony = null;
            enabled = false;
            Logger.LogError($"Takaro Valheim Companion startup failed and was rolled back: {ex.Message}");
        }
    }

    private void Update()
    {
        clientBridge?.Update();
    }

    private void OnDestroy()
    {
        CompanionClientHooks.Shutdown();
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
