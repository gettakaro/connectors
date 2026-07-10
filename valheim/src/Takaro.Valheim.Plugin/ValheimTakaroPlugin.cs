using Takaro.Valheim.Core;

#if TAKARO_VALHEIM_PLUGIN
using BepInEx;
using BepInEx.Configuration;
using HarmonyLib;
using System.Diagnostics;
using UnityEngine;

namespace Takaro.Valheim.Plugin;

[BepInPlugin(PluginGuid, PluginName, PluginVersion)]
public sealed class ValheimTakaroPlugin : BaseUnityPlugin
{
    public const string PluginGuid = "com.takaro.valheim";
    public const string PluginName = "Takaro Valheim";
    public const string PluginVersion = TakaroBuildVersion.BepInExVersion;
    public const string ReleaseVersion = TakaroBuildVersion.ReleaseVersion;

    private TakaroWebSocketRunner? runner;
    private Harmony? harmony;

    private void Awake()
    {
        if (!IsDedicatedServerProcess())
        {
            Logger.LogWarning("Takaro Valheim only runs on dedicated Valheim servers; client process detected, plugin disabled.");
            enabled = false;
            return;
        }

        harmony = new Harmony(PluginGuid);
        harmony.PatchAll(typeof(ValheimChatEventBridge).Assembly);

        var values = new Dictionary<string, string>
        {
            ["registrationToken"] = Bind("Takaro", "registrationToken", "", "Takaro registration token.").Value,
            ["serverName"] = Bind("Takaro", "serverName", "Valheim Server", "Human-readable server name.").Value,
            ["identityToken"] = Bind("Takaro", "identityToken", "", "Takaro identity token after first registration.").Value,
            ["takaroWsUrl"] = Bind("Takaro", "takaroWsUrl", "wss://connect.takaro.io/", "Takaro connector WebSocket URL.").Value,
            ["logLevel"] = Bind("Takaro", "logLevel", "Information", "Connector log level.").Value,
            ["enableLogEvents"] = Bind("Takaro", "enableLogEvents", "true", "Forward connector log events to Takaro.").Value,
            ["commandAllowlistExact"] = Bind("Takaro", "commandAllowlistExact", "help", "Semicolon-separated exact console commands allowed for executeConsoleCommand.").Value,
            ["commandAllowlistPrefixes"] = Bind("Takaro", "commandAllowlistPrefixes", "", "Semicolon-separated console command prefixes allowed for executeConsoleCommand.").Value
        };

        if (!ConnectorConfig.TryFromDictionary(values, out var config, out var error) || config is null)
        {
            Logger.LogWarning($"Takaro Valheim connector disabled: {error}");
            return;
        }

        var adapter = new ValheimServerAdapter(Logger, config);
        runner = new TakaroWebSocketRunner(config, adapter, message => Logger.LogInfo(message));
        ValheimChatEventBridge.Initialize(runner, Logger.LogInfo);
        _ = runner.StartAsync();

        Logger.LogInfo("Takaro Valheim connector started.");
    }

    private void Update() =>
        ValheimChatEventBridge.Update();

    private void OnDestroy()
    {
        harmony?.UnpatchSelf();
        ValheimChatEventBridge.Shutdown();
        runner?.Dispose();
    }

    private ConfigEntry<string> Bind(string section, string key, string defaultValue, string description) =>
        Config.Bind(section, key, defaultValue, description);

    private static bool IsDedicatedServerProcess()
    {
        using var process = Process.GetCurrentProcess();
        return ValheimRuntimePolicy.IsDedicatedServerProcess(
            Application.isBatchMode,
            process.ProcessName,
            Environment.GetCommandLineArgs().FirstOrDefault());
    }
}
#else
namespace Takaro.Valheim.Plugin;

public sealed class ValheimTakaroPlugin
{
    public const string PluginGuid = "com.takaro.valheim";
    public const string PluginName = "Takaro Valheim";
    public const string PluginVersion = TakaroBuildVersion.BepInExVersion;
    public const string ReleaseVersion = TakaroBuildVersion.ReleaseVersion;

    public static string BuildMode =>
        "Reference-free scaffold. Build with EnableValheimPluginBuild=true and Valheim/BepInEx references for the real plugin.";
}
#endif
