using Takaro.Valheim.Core;

#if TAKARO_VALHEIM_PLUGIN
using BepInEx;
using BepInEx.Configuration;
using HarmonyLib;
using UnityEngine;

namespace Takaro.Valheim.Plugin;

[BepInPlugin(PluginGuid, PluginName, PluginVersion)]
[BepInDependency(Jotunn.Main.ModGuid)]
public sealed class ValheimTakaroPlugin : BaseUnityPlugin
{
    public const string PluginGuid = "com.takaro.valheim";
    public const string PluginName = "Takaro Valheim";
    public const string PluginVersion = "0.1.0";

    private TakaroWebSocketRunner? runner;
    private Harmony? harmony;

    private void Awake()
    {
        if (!IsDedicatedServerProcess())
        {
            Logger.LogInfo("Takaro Valheim connector only runs on dedicated Valheim servers; skipping client process.");
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

    private void Update()
    {
        if (runner is not null)
        {
            ValheimChatEventBridge.Update();
        }
    }

    private void OnDestroy()
    {
        harmony?.UnpatchSelf();
        ValheimChatEventBridge.Shutdown();
        runner?.Dispose();
    }

    private ConfigEntry<string> Bind(string section, string key, string defaultValue, string description) =>
        Config.Bind(section, key, defaultValue, description);

    private static bool IsDedicatedServerProcess() =>
        Application.isBatchMode
        || Environment.GetCommandLineArgs().Any(arg => arg.IndexOf("valheim_server", StringComparison.OrdinalIgnoreCase) >= 0);
}
#else
namespace Takaro.Valheim.Plugin;

public sealed class ValheimTakaroPlugin
{
    public const string PluginGuid = "com.takaro.valheim";
    public const string PluginName = "Takaro Valheim";
    public const string PluginVersion = "0.1.0";

    public static string BuildMode =>
        "Reference-free scaffold. Build with EnableValheimPluginBuild=true and Valheim/BepInEx/Jotunn references for the real plugin.";
}
#endif
