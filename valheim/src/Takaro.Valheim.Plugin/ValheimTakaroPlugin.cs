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
    private CompanionServerBridge? companionBridge;
    private CompanionInventoryCache? companionInventory;
    private QueuedMainThreadActionScheduler? mainThreadActions;
    private Harmony? harmony;
    private bool shutdownRequested;
    private float shutdownRequestedAt;

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
            ["commandAllowlistPrefixes"] = Bind("Takaro", "commandAllowlistPrefixes", "", "Semicolon-separated console command prefixes allowed for executeConsoleCommand.").Value,
            ["companionMode"] = Bind("Takaro", "companionMode", "required", "Client companion policy: disabled, optional, or required.").Value,
            ["companionCommandPrefixes"] = Bind("Takaro", "companionCommandPrefixes", "$", "Semicolon-separated client companion command prefixes.").Value
        };

        if (!ConnectorConfig.TryFromDictionary(values, out var config, out var error) || config is null)
        {
            Logger.LogWarning($"Takaro Valheim connector disabled: {error}");
            return;
        }

        mainThreadActions = new QueuedMainThreadActionScheduler();
        companionInventory = new CompanionInventoryCache();
        var playerResolver = new ValheimPlayerResolver(Logger);
        var adapter = new ValheimServerAdapter(
            Logger,
            config,
            RequestShutdown,
            companionInventory,
            playerResolver);
        runner = new TakaroWebSocketRunner(
            config,
            adapter,
            message => Logger.LogInfo(message),
            mainThreadActions);
        companionBridge = config.CompanionMode == CompanionMode.Disabled
            ? null
            : new CompanionServerBridge(
                runner,
                playerResolver,
                companionInventory,
                config.CompanionMode,
                Logger.LogInfo);
        ValheimChatEventBridge.Initialize(runner, Logger.LogInfo);
        _ = runner.StartAsync();

        Logger.LogInfo("Takaro Valheim connector started.");
    }

    private void Update()
    {
        mainThreadActions?.Drain();
        companionBridge?.Update();
        ValheimChatEventBridge.Update();

        if (shutdownRequested && Time.realtimeSinceStartup >= shutdownRequestedAt)
        {
            shutdownRequested = false;
            Logger.LogInfo("Takaro Valheim executing scheduled shutdown on the Unity main thread.");
            Application.Quit();
        }
    }

    private void OnDestroy()
    {
        harmony?.UnpatchSelf();
        companionBridge?.Dispose();
        ValheimChatEventBridge.Shutdown();
        runner?.Dispose();
        companionInventory?.Clear();
        mainThreadActions?.Dispose();
    }

    private void RequestShutdown()
    {
        shutdownRequested = true;
        shutdownRequestedAt = Time.realtimeSinceStartup + 1f;
        Logger.LogInfo("Takaro Valheim shutdown requested; scheduling Application.Quit after response flush.");
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
