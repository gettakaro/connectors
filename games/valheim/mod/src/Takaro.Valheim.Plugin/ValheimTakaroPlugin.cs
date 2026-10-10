using Takaro.Valheim.Core;

#if TAKARO_VALHEIM_PLUGIN
using BepInEx;
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

    // Often enough that a pasted token feels immediate; the files are tiny.
    private static readonly TimeSpan ConfigCheckInterval = TimeSpan.FromSeconds(5);

    private TakaroWebSocketRunner? runner;
    private ConnectorConfigStore? configStore;
    private System.Threading.Timer? configWatch;
    private readonly object configCheckGate = new();
    private InventoryCompanionBridge? inventoryCompanion;
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

        // The user file ships in the plugin folder; the saved copy lives where releases up to 4.1
        // kept the whole config, outside the folder an upgrade replaces. Settings are read by
        // ConnectorConfigStore rather than bound through BepInEx, so BepInEx never rewrites either file.
        configStore = new ConnectorConfigStore(
            Path.Combine(Path.GetDirectoryName(Info.Location) ?? Paths.PluginPath, ConnectorConfigFiles.UserFileName),
            Path.Combine(Paths.ConfigPath, ConnectorConfigFiles.SavedFileName),
            message => Logger.LogInfo(message),
            message => Logger.LogWarning(message),
            hostServerName: ValheimRuntimePolicy.ServerNameArgument(Environment.GetCommandLineArgs()));
        if (!configStore.Load(out var config, out var error) || config is null)
        {
            Logger.LogWarning($"Takaro Valheim connector disabled: {error}. Fix it in {configStore.UserPath} and restart the server.");
            return;
        }

        mainThreadActions = new QueuedMainThreadActionScheduler();
        var knownPlayerNames = new KnownPlayerNames(Path.Combine(Paths.ConfigPath, "com.takaro.valheim.known-players.json"));
        try
        {
            knownPlayerNames.Load();
        }
        catch (Exception exception)
        {
            Logger.LogWarning($"Takaro Valheim could not read known player names: {exception.Message}");
        }

        var playerResolver = new ValheimPlayerResolver(Logger, knownPlayerNames);
        companionInventory = new CompanionInventoryCache();
        var adapter = new ValheimServerAdapter(Logger, config, RequestShutdown, playerResolver, companionInventory);
        runner = new TakaroWebSocketRunner(
            config,
            adapter,
            message => Logger.LogInfo(message),
            mainThreadActions,
            message => Logger.LogWarning(message),
            configStore.UserPath);
        TakaroChatParticipant.Initialize(config.ChatSenderName, Logger.LogInfo);
        ValheimServerEventBridge.Initialize(runner, playerResolver, Logger.LogInfo);
        // Optional: only players who install the inventory companion ever answer it.
        inventoryCompanion = new InventoryCompanionBridge(playerResolver, companionInventory, Logger.LogInfo);
        harmony = new Harmony(PluginGuid);
        harmony.PatchAll(typeof(ValheimServerEventBridge).Assembly);
        _ = runner.StartAsync();
        configWatch = new System.Threading.Timer(_ => CheckConfig(), null, ConfigCheckInterval, ConfigCheckInterval);

        Logger.LogInfo("Takaro Valheim connector started.");
    }

    private void Update()
    {
        ValheimServerAdapter.RefreshReadiness();
        mainThreadActions?.Drain();
        ValheimServerEventBridge.Update();
        inventoryCompanion?.Update();

        if (shutdownRequested && Time.realtimeSinceStartup >= shutdownRequestedAt)
        {
            shutdownRequested = false;
            Logger.LogInfo("Takaro Valheim executing scheduled shutdown on the Unity main thread.");
            Application.Quit();
        }
    }

    private void OnDestroy()
    {
        configWatch?.Dispose();
        configWatch = null;
        harmony?.UnpatchSelf();
        ValheimServerEventBridge.Shutdown();
        inventoryCompanion?.Dispose();
        inventoryCompanion = null;
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

    // Runs on a timer thread: file reads only, never Unity. Checks never overlap, so an older
    // read can never be applied after a newer one.
    private void CheckConfig()
    {
        if (!Monitor.TryEnter(configCheckGate))
        {
            return;
        }

        try
        {
            var change = configStore?.CheckForChanges();
            if (change is { ConnectionChanged: true })
            {
                runner?.UpdateSettings(change.Config);
            }
        }
        catch (Exception exception)
        {
            Logger.LogWarning($"Takaro Valheim could not check its config for changes: {exception.Message}");
        }
        finally
        {
            Monitor.Exit(configCheckGate);
        }
    }

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
