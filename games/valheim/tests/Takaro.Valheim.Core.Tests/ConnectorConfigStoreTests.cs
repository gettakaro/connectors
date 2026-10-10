using Microsoft.VisualStudio.TestTools.UnitTesting;
using Takaro.Valheim.Core;

namespace Takaro.Valheim.Core.Tests;

/// <summary>
/// The two config files: takaro.cfg in the plugin folder (shipped, edited, replaced by an
/// upgrade) and BepInEx/config/com.takaro.valheim.cfg (where releases up to 4.1 kept everything,
/// now the connector-kept copy of the token and identity).
/// </summary>
[TestClass]
public sealed class ConnectorConfigStoreTests
{
    private const string LegacyFile =
        "## Settings file was created by plugin Takaro Valheim v4.1.1\n"
        + "## Plugin GUID: com.takaro.valheim\n\n[Takaro]\n\n"
        + "## Takaro registration token.\n# Setting type: String\n# Default value: \nregistrationToken = legacy-token\n\n"
        + "serverName = My Meadows\n\nidentityToken = \n\ntakaroWsUrl = wss://connect.takaro.io/\n\n"
        + "commandAllowlistExact = help;save\n\ncompanionMode = optional\n";

    private string dir = "";
    private string userPath = "";
    private string savedPath = "";
    private readonly List<string> warnings = new();
    private int generated;

    [TestInitialize]
    public void SetUp()
    {
        dir = Path.Combine(Path.GetTempPath(), "takaro-valheim-config-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(Path.Combine(dir, "plugins", "TakaroValheim"));
        Directory.CreateDirectory(Path.Combine(dir, "config"));
        userPath = Path.Combine(dir, "plugins", "TakaroValheim", "takaro.cfg");
        savedPath = Path.Combine(dir, "config", "com.takaro.valheim.cfg");
    }

    [TestCleanup]
    public void TearDown() => Directory.Delete(dir, recursive: true);

    [TestMethod]
    public void ShippedFileHasEmptyTokenAndIdentityAndEveryDefault()
    {
        var text = ConnectorConfigFiles.ShippedUserFile();
        var shipped = ConnectorConfigFiles.Parse(text);

        Assert.AreEqual("", shipped["registrationToken"]);
        Assert.AreEqual("", shipped["identityToken"]);
        CollectionAssert.AreEquivalent(new[] { "registrationToken", "identityToken" }, shipped.Keys.ToArray(),
            "Every other key ships commented out, so a fresh copy never overrides the saved file.");
        foreach (var entry in ConnectorConfigFiles.Defaults.Where(e => !shipped.ContainsKey(e.Key)))
        {
            StringAssert.Contains(text, $"# {entry.Key} =", entry.Key);
        }
    }

    [TestMethod]
    public void ShippedFileOnDiskIsTheEmbeddedOne()
    {
        var onDisk = File.ReadAllText(Path.Combine(ValheimRoot(), "mod", "takaro.cfg"));
        Assert.AreEqual(onDisk, ConnectorConfigFiles.ShippedUserFile());
    }

    [TestMethod]
    public void FreshInstallWithoutTokenGeneratesAndKeepsAnIdentityButHasNoToken()
    {
        Ship();

        var config = Load();

        Assert.IsFalse(config.HasRegistrationToken);
        Assert.AreEqual("generated-1", config.IdentityToken);
        Assert.AreEqual("generated-1", Read(userPath)["identityToken"]);
        Assert.AreEqual("generated-1", Read(savedPath)["identityToken"]);
        Assert.AreEqual("", Read(savedPath)["registrationToken"]);
        Assert.AreEqual("", Read(userPath)["registrationToken"]);
    }

    [TestMethod]
    public void FreshInstallTokenPastedBeforeStartConnectsAsANewServer()
    {
        Ship(token: "fresh-token");

        var config = Load();

        Assert.AreEqual("fresh-token", config.RegistrationToken);
        Assert.AreEqual("generated-1", config.IdentityToken);
        Assert.AreEqual("Valheim Server (generate)", config.ServerName, "A fresh install never announces the shared default name.");
        Assert.IsFalse(Read(userPath).ContainsKey("serverName"), "The name is not written into the file.");
        Assert.AreEqual("fresh-token", Read(savedPath)["registrationToken"]);
        Assert.AreEqual("generated-1", Read(savedPath)["identityToken"]);
    }

    [TestMethod]
    public void MissingUserFileIsRecreatedFromTheShippedOne()
    {
        var config = Load();

        Assert.AreEqual("generated-1", config.IdentityToken);
        var recreated = File.ReadAllText(userPath);
        StringAssert.Contains(recreated, "Paste the registration token");
        Assert.AreEqual("generated-1", ConnectorConfigFiles.Parse(recreated)["identityToken"]);
    }

    [TestMethod]
    public void UpgradeThatReplacesThePluginFolderKeepsTokenAndIdentity()
    {
        Ship(token: "fresh-token");
        Load();

        // README upgrade: delete BepInEx/plugins/TakaroValheim/, unzip the new one.
        File.Delete(userPath);
        Ship();
        var config = Load();

        Assert.AreEqual("fresh-token", config.RegistrationToken);
        Assert.AreEqual("generated-1", config.IdentityToken);
        Assert.AreEqual(1, generated);
        Assert.AreEqual("generated-1", Read(userPath)["identityToken"]);
        Assert.AreEqual("", Read(userPath)["registrationToken"], "The token is never copied into the user file.");
    }

    [TestMethod]
    public void UpgradeFromALegacyInstallKeepsItsServerNameIdentity()
    {
        File.WriteAllText(savedPath, LegacyFile);
        Ship();

        var config = Load();

        Assert.AreEqual("legacy-token", config.RegistrationToken);
        Assert.AreEqual("My Meadows", config.IdentityToken);
        Assert.AreEqual("My Meadows", config.ServerName);
        Assert.AreEqual(0, generated);
        CollectionAssert.AreEqual(new[] { "help", "save" }, config.CommandAllowlistExact.ToArray());
        Assert.AreEqual("My Meadows", Read(userPath)["identityToken"]);
        var saved = File.ReadAllText(savedPath);
        Assert.AreEqual("My Meadows", ConnectorConfigFiles.Parse(saved)["identityToken"]);
        StringAssert.Contains(saved, "companionMode = optional", "Unknown keys and comments stay.");
        StringAssert.Contains(saved, "# Setting type: String");
    }

    [TestMethod]
    public void UpgradeFromALegacyInstallOnTheDefaultServerNameKeepsThatName()
    {
        File.WriteAllText(savedPath, LegacyFile.Replace("serverName = My Meadows\n", ""));
        Ship();

        Assert.AreEqual("Valheim Server", Load().IdentityToken);
    }

    [TestMethod]
    public void FreshInstallUsesTheDedicatedServerNameWhenServerNameIsUnset()
    {
        Ship(token: "t");

        var store = new ConnectorConfigStore(userPath, savedPath, newIdentity: () => "generated-1", settleDelay: TimeSpan.Zero, hostServerName: "Odin's Hall");
        Assert.IsTrue(store.Load(out var config, out _));

        Assert.AreEqual("Odin's Hall (generate)", config!.ServerName);
    }

    [TestMethod]
    public void LegacyDefaultNameIdentityKeepsTheDefaultNameAfterItWasWrittenBack()
    {
        File.WriteAllText(savedPath, LegacyFile.Replace("serverName = My Meadows\n", ""));
        Ship();
        Load();

        // Second start: the identity now comes from the files, not the legacy rule.
        var store = new ConnectorConfigStore(userPath, savedPath, hostServerName: "Odin's Hall");
        Assert.IsTrue(store.Load(out var config, out _));

        Assert.AreEqual("Valheim Server", config!.IdentityToken);
        Assert.AreEqual("Valheim Server", config.ServerName);
    }

    [TestMethod]
    public void ExplicitServerNameIsSentAsIs()
    {
        Ship(token: "t", extra: "serverName = Ashlands\n");

        var store = new ConnectorConfigStore(userPath, savedPath, newIdentity: () => "x", hostServerName: "Odin's Hall");
        Assert.IsTrue(store.Load(out var config, out _));

        Assert.AreEqual("Ashlands", config!.ServerName);
    }

    [TestMethod]
    public void LegacyInstallStillWorksWithoutTheNewUserFile()
    {
        File.WriteAllText(savedPath, LegacyFile);

        var config = Load();

        Assert.AreEqual("My Meadows", config.IdentityToken);
        Assert.AreEqual("My Meadows", Read(userPath)["identityToken"]);
    }

    [TestMethod]
    public void LegacyFileThatNeverHadATokenNeverReachedTakaroAndGetsANewIdentity()
    {
        File.WriteAllText(savedPath, LegacyFile.Replace("legacy-token", ""));
        Ship();

        Assert.AreEqual("generated-1", Load().IdentityToken);
    }

    [TestMethod]
    public void HandWrittenSavedFileIsNotMistakenForALegacyInstall()
    {
        File.WriteAllText(savedPath, "[Takaro]\nregistrationToken = hand-token\nserverName = Hand\n");
        Ship();

        var config = Load();

        Assert.AreEqual("hand-token", config.RegistrationToken);
        Assert.AreEqual("generated-1", config.IdentityToken);
    }

    [TestMethod]
    public void ExplicitIdentityInTheSavedFileIsKept()
    {
        File.WriteAllText(savedPath, LegacyFile.Replace("identityToken = \n", "identityToken = takaro-dev-valheim\n"));
        Ship();

        var config = Load();

        Assert.AreEqual("takaro-dev-valheim", config.IdentityToken);
        Assert.AreEqual("takaro-dev-valheim", Read(userPath)["identityToken"]);
    }

    [TestMethod]
    public void TokenInTheUserFileWinsAndIsSavedForTheNextUpgrade()
    {
        File.WriteAllText(savedPath, LegacyFile);
        Ship(token: "new-token");

        var config = Load();

        Assert.AreEqual("new-token", config.RegistrationToken);
        Assert.AreEqual("My Meadows", config.IdentityToken);
        Assert.AreEqual("new-token", Read(savedPath)["registrationToken"]);
    }

    [TestMethod]
    public void ShippedUserFileNeverUndoesSavedSettingsButAnExplicitOneAlwaysWins()
    {
        File.WriteAllText(savedPath, LegacyFile);
        Ship();

        CollectionAssert.AreEqual(new[] { "help", "save" }, Load().CommandAllowlistExact.ToArray(), "A fresh shipped copy keeps the saved setting.");

        // Narrowing back to the default from the user file must work (it is a permission).
        Ship(extra: "commandAllowlistExact = help\nchatSenderName = Odin\n");
        var config = Load();

        CollectionAssert.AreEqual(new[] { "help" }, config.CommandAllowlistExact.ToArray());
        Assert.AreEqual("Odin", config.ChatSenderName);
    }

    [TestMethod]
    public void IdentityInTheUserFileIsNeverReplaced()
    {
        Ship(token: "t", identity: "chosen-id");

        Assert.AreEqual("chosen-id", Load().IdentityToken);
        Assert.AreEqual(0, generated);
        Assert.AreEqual("chosen-id", Read(savedPath)["identityToken"]);
    }

    [TestMethod]
    public void TokenSavedWhileRunningIsAConnectionChange()
    {
        Ship();
        var store = Store();
        Assert.IsTrue(store.Load(out var first, out _));
        Assert.IsNull(store.CheckForChanges(), "The connector's own writes are not a change.");

        SetUserValue("registrationToken", "pasted-token");
        var change = store.CheckForChanges();

        Assert.IsNotNull(change);
        Assert.IsTrue(change.ConnectionChanged);
        Assert.AreEqual("pasted-token", change.Config.RegistrationToken);
        Assert.AreEqual(first!.IdentityToken, change.Config.IdentityToken);
        Assert.IsNull(store.CheckForChanges());
        Assert.AreEqual("pasted-token", Read(savedPath)["registrationToken"]);
    }

    [TestMethod]
    public void SameLengthEditIsSeen()
    {
        Ship(token: "aaaa");
        var store = Store();
        store.Load(out _, out _);

        SetUserValue("registrationToken", "bbbb");

        Assert.AreEqual("bbbb", store.CheckForChanges()?.Config.RegistrationToken);
    }

    [TestMethod]
    public void HalfSavedFileKeepsTheCurrentSettingsAndIsRetried()
    {
        Ship(token: "good");
        var store = Store();
        store.Load(out _, out _);

        File.WriteAllText(userPath, "");
        Assert.IsNull(store.CheckForChanges());
        File.WriteAllText(userPath, "## Takaro Valheim connector settings.\nregistrationTok");
        Assert.IsNull(store.CheckForChanges());
        Assert.IsTrue(warnings.Any(w => w.Contains("could not read", StringComparison.Ordinal)));

        Ship(token: "better");
        var change = store.CheckForChanges();
        Assert.AreEqual("better", change?.Config.RegistrationToken);
        Assert.AreEqual("generated-1", change?.Config.IdentityToken);
    }

    [TestMethod]
    public void HalfSavedFileIsNeverOverwritten()
    {
        Ship(token: "good");
        var store = Store();
        store.Load(out _, out _);

        File.WriteAllText(userPath, "registrationTok");
        store.CheckForChanges();

        Assert.AreEqual("registrationTok", File.ReadAllText(userPath));
    }

    [TestMethod]
    public void UnreadableUserFileAtStartupIsLeftAlone()
    {
        File.WriteAllText(userPath, "garbage without a section");

        var config = Load();

        Assert.AreEqual("garbage without a section", File.ReadAllText(userPath));
        Assert.AreEqual("generated-1", config.IdentityToken);
        Assert.AreEqual("generated-1", Read(savedPath)["identityToken"]);
    }

    [TestMethod]
    public void IdentityRemovedWhileRunningIsWrittenBackUnchanged()
    {
        Ship(token: "t");
        var store = Store();
        store.Load(out var first, out _);

        SetUserValue("identityToken", "");
        File.WriteAllText(savedPath, ConnectorConfigFiles.SetValues(File.ReadAllText(savedPath), new Dictionary<string, string> { ["identityToken"] = "" }));
        var change = store.CheckForChanges();

        Assert.IsNotNull(change);
        Assert.IsFalse(change.ConnectionChanged);
        Assert.AreEqual(first!.IdentityToken, change.Config.IdentityToken);
        Assert.AreEqual(first.IdentityToken, Read(userPath)["identityToken"]);
        Assert.AreEqual(1, generated);
    }

    [TestMethod]
    public void OtherSettingsNeedARestartAndSaySo()
    {
        Ship(token: "t");
        var store = Store();
        store.Load(out _, out _);

        SetUserValue("chatSenderName", "Odin");
        var change = store.CheckForChanges();

        Assert.IsNotNull(change);
        Assert.IsFalse(change.ConnectionChanged);
        CollectionAssert.AreEqual(new[] { "chatSenderName" }, change.RestartKeys.ToArray());
        Assert.IsTrue(warnings.Any(w => w.Contains("restart the server", StringComparison.Ordinal)));
    }

    [TestMethod]
    public void UrlAndServerNameChangesReconnect()
    {
        Ship(token: "t");
        var store = Store();
        store.Load(out _, out _);

        SetUserValue("serverName", "Ashlands");
        Assert.IsTrue(store.CheckForChanges()!.ConnectionChanged);
        SetUserValue("takaroWsUrl", "ws://127.0.0.1:1/");
        Assert.IsTrue(store.CheckForChanges()!.ConnectionChanged);
    }

    [TestMethod]
    public void InvalidSettingsWhileRunningKeepTheCurrentOnes()
    {
        Ship(token: "t");
        var store = Store();
        store.Load(out _, out _);

        SetUserValue("chatSenderName", new string('n', ConnectorConfig.MaximumChatSenderNameCharacters + 1));

        Assert.IsNull(store.CheckForChanges());
        Assert.IsTrue(warnings.Any(w => w.Contains("chatSenderName", StringComparison.Ordinal)));
    }

    [TestMethod]
    public void SetValuesKeepsLayoutAndAddsMissingKeysToTheSection()
    {
        var text = "# head\r\n[Other]\r\nregistrationToken = not-this\r\n[Takaro]\r\n## c\r\nserverName = A\r\n\r\n[Tail]\r\nx = 1\r\n";

        var updated = ConnectorConfigFiles.SetValues(text, new Dictionary<string, string>
        {
            ["serverName"] = "B",
            ["identityToken"] = "id\n"
        });

        Assert.AreEqual(
            "# head\r\n[Other]\r\nregistrationToken = not-this\r\n[Takaro]\r\n## c\r\nserverName = B\r\nidentityToken = id\r\n\r\n[Tail]\r\nx = 1\r\n",
            updated);
    }

    [TestMethod]
    public void ClearingTheTokenWhileRunningDisconnectsAndClearsTheSavedCopy()
    {
        Ship(token: "t");
        var store = Store();
        store.Load(out _, out _);

        SetUserValue("registrationToken", "");
        var change = store.CheckForChanges();

        Assert.IsNotNull(change);
        Assert.IsTrue(change.ConnectionChanged);
        Assert.IsFalse(change.Config.HasRegistrationToken);
        Assert.AreEqual("", Read(savedPath)["registrationToken"]);
        Assert.IsNull(store.CheckForChanges());
        Assert.IsFalse(Load().HasRegistrationToken, "A restart does not bring the cleared token back.");
    }

    [TestMethod]
    public void SetValuesReplacesEveryDuplicateOfAKey()
    {
        var text = "[Takaro]\nidentityToken = old\nidentityToken = stale\n";

        var updated = ConnectorConfigFiles.SetValues(text, new Dictionary<string, string> { ["identityToken"] = "new" });

        Assert.AreEqual("new", ConnectorConfigFiles.Parse(updated)["identityToken"]);
        Assert.IsFalse(updated.Contains("stale", StringComparison.Ordinal));
    }

    private ConnectorConfigStore Store() =>
        new(userPath, savedPath, _ => { }, warnings.Add, () => $"generated-{++generated}", TimeSpan.FromMilliseconds(10));

    private ConnectorConfig Load()
    {
        Assert.IsTrue(Store().Load(out var config, out var error), error);
        return config!;
    }

    private void Ship(string token = "", string identity = "", string extra = "")
    {
        var text = ConnectorConfigFiles.SetValues(ConnectorConfigFiles.ShippedUserFile(), new Dictionary<string, string>
        {
            ["registrationToken"] = token,
            ["identityToken"] = identity
        });
        File.WriteAllText(userPath, text + extra);
    }

    private void SetUserValue(string key, string value) =>
        File.WriteAllText(userPath, ConnectorConfigFiles.SetValues(File.ReadAllText(userPath), new Dictionary<string, string> { [key] = value }));

    private static Dictionary<string, string> Read(string path) => ConnectorConfigFiles.Parse(File.ReadAllText(path));

    private static string ValheimRoot()
    {
        var directory = new DirectoryInfo(AppContext.BaseDirectory);
        while (directory is not null && !File.Exists(Path.Combine(directory.FullName, "capabilities.json")))
        {
            directory = directory.Parent;
        }

        return directory?.FullName ?? throw new DirectoryNotFoundException("games/valheim");
    }
}
