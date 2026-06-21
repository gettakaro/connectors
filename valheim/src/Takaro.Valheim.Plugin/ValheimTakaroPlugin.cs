using Takaro.Valheim.Core;

#if TAKARO_VALHEIM_PLUGIN
using BepInEx;
using BepInEx.Configuration;
using HarmonyLib;
using System.Reflection;
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
        harmony = new Harmony(PluginGuid);
        harmony.PatchAll(typeof(ValheimChatEventBridge).Assembly);
        LogHarmonyPatchState();

        if (!IsDedicatedServerProcess())
        {
            ValheimChatEventBridge.Initialize(null, Logger.LogInfo);
            Logger.LogInfo("Takaro Valheim client bridge started.");
            return;
        }

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

    private static bool IsDedicatedServerProcess() =>
        Application.isBatchMode
        || Environment.GetCommandLineArgs().Any(arg => arg.IndexOf("valheim_server", StringComparison.OrdinalIgnoreCase) >= 0);

    private void LogHarmonyPatchState()
    {
        var targets = new (string Name, MethodBase? Method)[]
        {
            ("Chat.RPC_ChatMessage", AccessTools.Method(typeof(Chat), "RPC_ChatMessage")),
            ("Talker.RPC_Say", AccessTools.Method(typeof(Talker), "RPC_Say")),
            ("ZRoutedRpc.RPC_RoutedRPC", AccessTools.Method(typeof(ZRoutedRpc), "RPC_RoutedRPC")),
            ("ZRoutedRpc.RouteRPC", AccessTools.Method(typeof(ZRoutedRpc), "RouteRPC")),
            ("ZRoutedRpc.HandleRoutedRPC", AccessTools.Method(typeof(ZRoutedRpc), "HandleRoutedRPC")),
            ("Chat.SendText", AccessTools.Method(typeof(Chat), "SendText")),
            ("Player.Update", AccessTools.Method(typeof(Player), "Update")),
            ("Player.OnDeath", AccessTools.Method(typeof(Player), "OnDeath")),
            ("Character.OnDeath", AccessTools.Method(typeof(Character), "OnDeath"))
        };

        foreach (var target in targets)
        {
            if (target.Method is null)
            {
                Logger.LogWarning($"Takaro Valheim Harmony target missing: {target.Name}.");
                continue;
            }

            var patchInfo = Harmony.GetPatchInfo(target.Method);
            var ownerCount = patchInfo is null
                ? 0
                : patchInfo.Prefixes.Concat(patchInfo.Postfixes).Concat(patchInfo.Transpilers).Concat(patchInfo.Finalizers)
                    .Count(patch => patch.owner == PluginGuid);
            Logger.LogInfo($"Takaro Valheim Harmony target {target.Name}: method={target.Method.DeclaringType?.FullName}.{target.Method.Name}, takaroPatchCount={ownerCount}.");
        }
    }
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
