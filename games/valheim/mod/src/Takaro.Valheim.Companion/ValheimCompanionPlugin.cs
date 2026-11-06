#if TAKARO_VALHEIM_COMPANION
using BepInEx;
using HarmonyLib;
using System.Diagnostics;
using Takaro.Valheim.Companion.Protocol;
using UnityEngine;

namespace Takaro.Valheim.Companion;

[BepInPlugin(PluginGuid, PluginName, PluginVersion)]
public sealed class ValheimCompanionPlugin : BaseUnityPlugin
{
    public const string PluginGuid = "com.takaro.valheim.companion";
    public const string PluginName = "Takaro Valheim Inventory Companion";
    public const string PluginVersion = TakaroCompanionBuildVersion.BepInExVersion;
    public const string ProductVersion = TakaroCompanionBuildVersion.ProductVersion;
    public const int ProtocolVersion = TakaroCompanionBuildVersion.ProtocolVersion;

    private CompanionClientBridge? clientBridge;
    private CompanionCommandProbe? commandProbe;
    private Harmony? harmony;

    private void Awake()
    {
        if (!IsGraphicalValheimClient())
        {
            Logger.LogWarning("Takaro Valheim Inventory Companion only runs in the graphical Valheim client; dedicated server process detected, mod disabled.");
            enabled = false;
            return;
        }

        try
        {
            harmony = new Harmony(PluginGuid);
            clientBridge = new CompanionClientBridge(Logger.LogInfo);
            clientBridge.Initialize();
            CompanionClientHooks.Initialize(clientBridge, Logger.LogInfo);
            harmony.PatchAll(typeof(ValheimCompanionPlugin).Assembly);
            // Research probe for native commands (#200): off unless the marker file exists.
            commandProbe = CompanionCommandProbe.TryCreate(
                Path.Combine(Paths.ConfigPath, CommandProbeProtocol.MarkerFileName),
                clientBridge,
                harmony,
                Logger.LogInfo);
            Logger.LogInfo($"Takaro Valheim Inventory Companion {ProductVersion} started with protocol {ProtocolVersion}. Optional: it only reports this character's inventory (and kill verdicts) to a Takaro server; it holds no Takaro credentials.");
        }
        catch (Exception ex)
        {
            CompanionClientHooks.Shutdown();
            clientBridge?.Dispose();
            clientBridge = null;
            harmony?.UnpatchSelf();
            harmony = null;
            enabled = false;
            Logger.LogError($"Takaro Valheim Inventory Companion startup failed and was rolled back: {ex.Message}");
        }
    }

    private void Update()
    {
        clientBridge?.Update();
    }

    private void OnDestroy()
    {
        commandProbe?.Dispose();
        commandProbe = null;
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
    public const string PluginName = "Takaro Valheim Inventory Companion";
    public const string PluginVersion = TakaroCompanionBuildVersion.BepInExVersion;
    public const string ProductVersion = TakaroCompanionBuildVersion.ProductVersion;
    public const int ProtocolVersion = TakaroCompanionBuildVersion.ProtocolVersion;

    public static string BuildMode =>
        "Reference-free scaffold. Build with EnableValheimCompanionBuild=true and Valheim/BepInEx references for the real client plugin.";
}
#endif
