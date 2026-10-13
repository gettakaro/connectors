using Microsoft.VisualStudio.TestTools.UnitTesting;
using Takaro.Valheim.Core;

namespace Takaro.Valheim.Core.Tests;

/// <summary>The one config file, BepInEx/config/com.takaro.valheim.cfg.</summary>
[TestClass]
public sealed class ConnectorConfigStoreTests
{
    private string dir = "";
    private string path = "";
    private readonly List<string> warnings = new();
    private int generated;

    [TestInitialize]
    public void SetUp()
    {
        dir = Path.Combine(Path.GetTempPath(), "takaro-valheim-config-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(Path.Combine(dir, "config"));
        path = Path.Combine(dir, "config", "com.takaro.valheim.cfg");
    }

    [TestCleanup]
    public void TearDown() => Directory.Delete(dir, recursive: true);

    [TestMethod]
    public void MissingFileIsCreatedWithEveryKeyAnEmptyTokenAndANewIdentity()
    {
        var config = Load();

        Assert.IsFalse(config.HasRegistrationToken);
        Assert.AreEqual("generated-1", config.IdentityToken);
        Assert.AreEqual("Valheim Server (generate)", config.ServerName);
        var text = File.ReadAllText(path);
        var file = ConnectorConfigFiles.Parse(text);
        CollectionAssert.AreEquivalent(ConnectorConfigFiles.Defaults.Keys.ToArray(), file.Keys.ToArray());
        Assert.AreEqual("", file["registrationToken"]);
        Assert.AreEqual("generated-1", file["identityToken"]);
        Assert.AreEqual("Valheim Server (generate)", file["serverName"]);
        StringAssert.Contains(text, "## Takaro registration token");
    }

    [TestMethod]
    public void RealIdentityIsAUuid()
    {
        Assert.IsTrue(new ConnectorConfigStore(path).Load(out var config, out _));

        Assert.IsTrue(Guid.TryParse(config!.IdentityToken, out _));
        Assert.AreEqual(config.IdentityToken, Read()["identityToken"]);
    }

    [TestMethod]
    public void CreatedFileUsesTheDedicatedServerName()
    {
        var store = new ConnectorConfigStore(path, newIdentity: () => "abcd-1234-ef", hostServerName: "Odin's Hall");
        Assert.IsTrue(store.Load(out var config, out _));

        Assert.AreEqual("Odin's Hall (abcd1234)", config!.ServerName);
        Assert.AreEqual("Odin's Hall (abcd1234)", Read()["serverName"]);
    }

    [TestMethod]
    public void ReadmeShowsTheFileTheConnectorCreates()
    {
        var readme = File.ReadAllText(Path.Combine(ValheimRoot(), "README.md")).Replace("\r\n", "\n");
        foreach (var line in ConnectorConfigFiles.NewFile("", "").Split('\n').Where(l => l.Length > 0 && !l.StartsWith("registrationToken", StringComparison.Ordinal)))
        {
            StringAssert.Contains(readme, line + "\n", line);
        }
    }

    [TestMethod]
    public void FileWrittenBeforeTheFirstStartGetsAnIdentityAndKeepsEverythingElse()
    {
        File.WriteAllText(path, "# mine\n[Takaro]\nregistrationToken = pasted\nidentityToken =\nfuture = kept\n");

        var config = Load();

        Assert.AreEqual("pasted", config.RegistrationToken);
        Assert.AreEqual("generated-1", config.IdentityToken);
        Assert.AreEqual("Valheim Server (generate)", config.ServerName, "An empty serverName is announced unique.");
        Assert.AreEqual("# mine\n[Takaro]\nregistrationToken = pasted\nidentityToken = generated-1\nfuture = kept\n", File.ReadAllText(path));
    }

    [TestMethod]
    public void IdentityInTheFileIsNeverReplaced()
    {
        File.WriteAllText(path, "[Takaro]\nregistrationToken = t\nidentityToken = takaro-dev-valheim\nserverName = Takaro Dev Valheim\n");
        var before = File.ReadAllText(path);

        var config = Load();

        Assert.AreEqual("takaro-dev-valheim", config.IdentityToken);
        Assert.AreEqual("Takaro Dev Valheim", config.ServerName);
        Assert.AreEqual(0, generated);
        Assert.AreEqual(before, File.ReadAllText(path), "Nothing is written when the identity is set.");
    }

    [TestMethod]
    public void RestartKeepsTheCreatedIdentity()
    {
        Load();
        var config = Load();

        Assert.AreEqual("generated-1", config.IdentityToken);
        Assert.AreEqual(1, generated);
    }

    [TestMethod]
    public void TokenSavedWhileRunningIsAConnectionChange()
    {
        var store = Store();
        Assert.IsTrue(store.Load(out var first, out _));
        Assert.IsNull(store.CheckForChanges(), "The connector's own write is not a change.");

        Set("registrationToken", "pasted-token");
        var change = store.CheckForChanges();

        Assert.IsNotNull(change);
        Assert.IsTrue(change.ConnectionChanged);
        Assert.AreEqual("pasted-token", change.Config.RegistrationToken);
        Assert.AreEqual(first!.IdentityToken, change.Config.IdentityToken);
        Assert.IsNull(store.CheckForChanges());
    }

    [TestMethod]
    public void EmptiedTokenWhileRunningDisconnects()
    {
        var store = Store();
        store.Load(out _, out _);
        Set("registrationToken", "t");
        store.CheckForChanges();

        Set("registrationToken", "");
        var change = store.CheckForChanges();

        Assert.IsNotNull(change);
        Assert.IsTrue(change.ConnectionChanged);
        Assert.IsFalse(change.Config.HasRegistrationToken);
    }

    [TestMethod]
    public void SameLengthEditIsSeen()
    {
        var store = Store();
        store.Load(out _, out _);
        Set("registrationToken", "aaaa");
        store.CheckForChanges();

        Set("registrationToken", "bbbb");

        Assert.AreEqual("bbbb", store.CheckForChanges()?.Config.RegistrationToken);
    }

    [TestMethod]
    public void HalfSavedFileKeepsTheCurrentSettingsIsNeverOverwrittenAndIsRetried()
    {
        var store = Store();
        store.Load(out _, out _);
        var good = File.ReadAllText(path);

        File.WriteAllText(path, "");
        Assert.IsNull(store.CheckForChanges());
        File.WriteAllText(path, "## Takaro Valheim connector settings.\nregistrationTok");
        Assert.IsNull(store.CheckForChanges());
        Assert.AreEqual("## Takaro Valheim connector settings.\nregistrationTok", File.ReadAllText(path));
        Assert.IsTrue(warnings.Any(w => w.Contains("could not read", StringComparison.Ordinal)));

        File.WriteAllText(path, ConnectorConfigFiles.SetValues(good, new Dictionary<string, string> { ["registrationToken"] = "better" }));
        var change = store.CheckForChanges();
        Assert.AreEqual("better", change?.Config.RegistrationToken);
        Assert.AreEqual("generated-1", change?.Config.IdentityToken);
    }

    [TestMethod]
    public void UnreadableFileAtStartupIsLeftAlone()
    {
        File.WriteAllText(path, "garbage without a section");

        var config = Load();

        Assert.AreEqual("garbage without a section", File.ReadAllText(path));
        Assert.IsFalse(config.HasRegistrationToken);
    }

    [TestMethod]
    public void DeletedFileWhileRunningKeepsTheCurrentSettings()
    {
        var store = Store();
        store.Load(out _, out _);

        File.Delete(path);

        Assert.IsNull(store.CheckForChanges());
        Assert.IsFalse(File.Exists(path));
    }

    [TestMethod]
    public void IdentityRemovedWhileRunningIsWrittenBackUnchanged()
    {
        var store = Store();
        store.Load(out var first, out _);

        Set("identityToken", "");
        var change = store.CheckForChanges();

        Assert.IsNotNull(change);
        Assert.IsFalse(change.ConnectionChanged);
        Assert.AreEqual(first!.IdentityToken, Read()["identityToken"]);
        Assert.AreEqual(1, generated);
    }

    [TestMethod]
    public void IdentityWriteBackNeverOverwritesAnEditSavedMeanwhile()
    {
        File.WriteAllText(path, "[Takaro]\nregistrationToken = old\n");
        var store = new ConnectorConfigStore(path, warn: warnings.Add, newIdentity: () =>
        {
            // An operator saves while the connector is about to write the identity back.
            File.WriteAllText(path, "[Takaro]\nregistrationToken = new\n");
            return $"generated-{++generated}";
        }, settleDelay: TimeSpan.FromMilliseconds(10));

        Assert.IsTrue(store.Load(out _, out _));
        Assert.AreEqual("[Takaro]\nregistrationToken = new\n", File.ReadAllText(path));

        var change = store.CheckForChanges();
        Assert.AreEqual("new", change?.Config.RegistrationToken);
        Assert.AreEqual("generated-1", Read()["identityToken"]);
        Assert.AreEqual("new", Read()["registrationToken"]);
    }

    [TestMethod]
    public void OtherSettingsNeedARestartAndSaySo()
    {
        var store = Store();
        store.Load(out _, out _);

        Set("chatSenderName", "Odin");
        var change = store.CheckForChanges();

        Assert.IsNotNull(change);
        Assert.IsFalse(change.ConnectionChanged);
        CollectionAssert.AreEqual(new[] { "chatSenderName" }, change.RestartKeys.ToArray());
        Assert.IsTrue(warnings.Any(w => w.Contains("restart the server", StringComparison.Ordinal)));
    }

    [TestMethod]
    public void UrlAndServerNameChangesReconnect()
    {
        var store = Store();
        store.Load(out _, out _);

        Set("serverName", "Ashlands");
        Assert.IsTrue(store.CheckForChanges()!.ConnectionChanged);
        Set("takaroWsUrl", "ws://127.0.0.1:1/");
        Assert.IsTrue(store.CheckForChanges()!.ConnectionChanged);
    }

    [TestMethod]
    public void InvalidSettingsWhileRunningKeepTheCurrentOnes()
    {
        var store = Store();
        store.Load(out _, out _);

        Set("chatSenderName", new string('n', ConnectorConfig.MaximumChatSenderNameCharacters + 1));

        Assert.IsNull(store.CheckForChanges());
        Assert.IsTrue(warnings.Any(w => w.Contains("chatSenderName", StringComparison.Ordinal)));
    }

    [TestMethod]
    public void UnwritableFileKeepsTheIdentityInMemoryForTheRun()
    {
        File.WriteAllText(path, "[Takaro]\nregistrationToken = t\n");
        File.SetAttributes(path, FileAttributes.ReadOnly);
        Directory.CreateDirectory(path + ".tmp"); // the temp file cannot be created either
        try
        {
            var store = Store();
            Assert.IsTrue(store.Load(out var config, out _));

            Assert.AreEqual("generated-1", config!.IdentityToken);
            Assert.IsTrue(warnings.Any(w => w.Contains("could not write", StringComparison.Ordinal)));
            store.CheckForChanges();
            Assert.AreEqual(1, generated, "A retry keeps the identity this process announced.");
        }
        finally
        {
            File.SetAttributes(path, FileAttributes.Normal);
            Directory.Delete(path + ".tmp");
        }
    }

    [TestMethod]
    public void WriteBackKeepsTheFileMode()
    {
        if (OperatingSystem.IsWindows())
        {
            return;
        }

        File.WriteAllText(path, "[Takaro]\nregistrationToken = t\nidentityToken =\n");
        File.SetUnixFileMode(path, UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.GroupRead);

        Load();

        Assert.AreEqual("generated-1", Read()["identityToken"]);
        Assert.AreEqual(UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.GroupRead, File.GetUnixFileMode(path));
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
    public void SetValuesReplacesEveryDuplicateOfAKey()
    {
        var text = "[Takaro]\nidentityToken = old\nidentityToken = stale\n";

        var updated = ConnectorConfigFiles.SetValues(text, new Dictionary<string, string> { ["identityToken"] = "new" });

        Assert.AreEqual("new", ConnectorConfigFiles.Parse(updated)["identityToken"]);
        Assert.IsFalse(updated.Contains("stale", StringComparison.Ordinal));
    }

    private ConnectorConfigStore Store() =>
        new(path, _ => { }, warnings.Add, () => $"generated-{++generated}", TimeSpan.FromMilliseconds(10));

    private ConnectorConfig Load()
    {
        Assert.IsTrue(Store().Load(out var config, out var error), error);
        return config!;
    }

    private void Set(string key, string value) =>
        File.WriteAllText(path, ConnectorConfigFiles.SetValues(File.ReadAllText(path), new Dictionary<string, string> { [key] = value }));

    private Dictionary<string, string> Read() => ConnectorConfigFiles.Parse(File.ReadAllText(path));

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
