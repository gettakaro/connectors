using Microsoft.VisualStudio.TestTools.UnitTesting;
using Takaro.Valheim.Core;

namespace Takaro.Valheim.Core.Tests;

/// <summary>Upgrading from an older connector version keeps the settings the server ran with.</summary>
[TestClass]
public sealed class LegacyConfigMigrationTests
{
    // The com.takaro.valheim.cfg 4.2.0 creates: only the two tokens are real, the rest are defaults.
    private const string Saved42 =
        "## Written by the Takaro Valheim connector: the token and identity it uses, kept outside\n"
        + "## BepInEx/plugins so that upgrading the connector keeps them. Edit\n"
        + "## BepInEx/plugins/TakaroValheim/takaro.cfg instead; a registrationToken set there takes\n"
        + "## precedence over this file.\n\n[Takaro]\n\n"
        + "registrationToken = reg-token\n\nserverName = Valheim Server\n\nidentityToken = 0f3e9a1c-0000-4000-8000-000000000001\n\n"
        + "takaroWsUrl = wss://connect.takaro.io/\n\nlogLevel = Information\n\nenableLogEvents = true\n\n"
        + "commandAllowlistExact = help\n\ncommandAllowlistPrefixes = \n\nchatSenderName = Takaro\n";

    // The takaro.cfg 4.2.0 ships, after the owner pasted the token and changed four settings
    // (4.2.0 also writes the identity into it).
    private const string User42 =
        "## Takaro Valheim connector settings.\n\n[Takaro]\n\n"
        + "## Takaro registration token.\nregistrationToken = reg-token\n\n"
        + "# serverName = Valheim Server\n\nidentityToken = 0f3e9a1c-0000-4000-8000-000000000001\n\n"
        + "takaroWsUrl = wss://connect.example.test/\n\nlogLevel = Debug\n\n# enableLogEvents = true\n\n"
        + "commandAllowlistExact = help;save\n\ncommandAllowlistPrefixes = ban\n\nchatSenderName = Herald\n";

    // A com.takaro.valheim.cfg written by BepInEx for 4.1.x, which never had an identityToken.
    private const string Saved41 =
        "## Settings file was created by plugin Takaro Valheim v4.1.0\n## Plugin GUID: com.takaro.valheim\n\n[Takaro]\n\n"
        + "registrationToken = reg-token\n\nserverName = Odin's Hall\n\nidentityToken = \n\nchatSenderName = Raven\n";

    private string dir = "";
    private string configPath = "";
    private string userPath = "";
    private readonly List<string> info = new();
    private int generated;

    [TestInitialize]
    public void SetUp()
    {
        dir = Path.Combine(Path.GetTempPath(), "takaro-valheim-migrate-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(Path.Combine(dir, "config"));
        Directory.CreateDirectory(Path.Combine(dir, "plugins", "TakaroValheim"));
        configPath = Path.Combine(dir, "config", ConnectorConfigFiles.FileName);
        userPath = Path.Combine(dir, "plugins", "TakaroValheim", LegacyConfigMigration.LegacyUserFileName);
    }

    [TestCleanup]
    public void TearDown() => Directory.Delete(dir, recursive: true);

    [TestMethod]
    public void UpgradeFrom42KeepsEverySettingOfTheUserFile()
    {
        File.WriteAllText(configPath, Saved42);
        File.WriteAllText(userPath, User42);

        var config = Load();

        Assert.AreEqual("reg-token", config.RegistrationToken);
        Assert.AreEqual("0f3e9a1c-0000-4000-8000-000000000001", config.IdentityToken);
        Assert.AreEqual("wss://connect.example.test/", config.TakaroWsUrl.ToString());
        Assert.AreEqual("Herald", config.ChatSenderName);
        Assert.AreEqual("Odin's Hall (0f3e9a1c)", config.ServerName, "The name 4.2.0 announced, not \"Valheim Server\".");
        var file = Read();
        Assert.AreEqual("Debug", file["logLevel"]);
        Assert.AreEqual("help;save", file["commandAllowlistExact"]);
        Assert.AreEqual("ban", file["commandAllowlistPrefixes"]);
        Assert.AreEqual("Odin's Hall (0f3e9a1c)", file["serverName"]);
        Assert.IsFalse(LegacyConfigMigration.IsLegacy(File.ReadAllText(configPath)), "The file is in the current format.");
        Assert.IsFalse(File.Exists(userPath), "takaro.cfg is renamed so nobody edits a file that is not read.");
        Assert.AreEqual(User42, File.ReadAllText(userPath + LegacyConfigMigration.MigratedSuffix));
        Assert.AreEqual(0, generated);
    }

    [TestMethod]
    public void UpgradeFrom42WithThePluginFolderDeletedKeepsTokenIdentityAndName()
    {
        File.WriteAllText(configPath, Saved42);

        var config = Load();

        Assert.AreEqual("reg-token", config.RegistrationToken);
        Assert.AreEqual("0f3e9a1c-0000-4000-8000-000000000001", config.IdentityToken);
        Assert.AreEqual("Odin's Hall (0f3e9a1c)", config.ServerName);
        Assert.AreEqual("wss://connect.takaro.io/", config.TakaroWsUrl.ToString());
    }

    [TestMethod]
    public void ExplicitServerNameIn42IsKept()
    {
        File.WriteAllText(configPath, Saved42);
        File.WriteAllText(userPath, User42.Replace("# serverName = Valheim Server", "serverName = My Hall"));

        Assert.AreEqual("My Hall", Load().ServerName);
    }

    [TestMethod]
    public void SavedFileMissingButUserFilePresentIsCarriedOver()
    {
        File.WriteAllText(userPath, User42);

        var config = Load();

        Assert.AreEqual("0f3e9a1c-0000-4000-8000-000000000001", config.IdentityToken);
        Assert.AreEqual("Herald", config.ChatSenderName);
        Assert.IsTrue(File.Exists(configPath));
        Assert.IsFalse(File.Exists(userPath));
    }

    [TestMethod]
    public void UpgradeFrom41KeepsTheServerNameAsIdentity()
    {
        File.WriteAllText(configPath, Saved41);

        var config = Load();

        Assert.AreEqual("Odin's Hall", config.IdentityToken, "Up to 4.1 Takaro knew this server by its serverName.");
        Assert.AreEqual("Odin's Hall", config.ServerName);
        Assert.AreEqual("Raven", config.ChatSenderName);
        Assert.AreEqual(0, generated);
        Assert.AreEqual("Odin's Hall", Read()["identityToken"]);
    }

    [TestMethod]
    public void CurrentFileIsNeverMigratedAgain()
    {
        File.WriteAllText(configPath, Saved42);
        File.WriteAllText(userPath, User42);
        Load();
        var migrated = File.ReadAllText(configPath);
        Set("chatSenderName", "Changed Later");
        File.WriteAllText(userPath, User42);

        var config = Load();

        Assert.AreEqual("Changed Later", config.ChatSenderName, "An edit after the upgrade wins over a takaro.cfg put back.");
        Assert.AreNotEqual(migrated, File.ReadAllText(configPath));
        Assert.IsTrue(File.Exists(userPath), "A takaro.cfg next to a current file is left alone.");
    }

    [TestMethod]
    public void FreshInstallIsUnchanged()
    {
        var config = Load();

        Assert.AreEqual("generated-1", config.IdentityToken);
        Assert.AreEqual("Odin's Hall (generate)", config.ServerName);
        Assert.IsFalse(info.Any(line => line.Contains("older connector version", StringComparison.Ordinal)));
    }

    [TestMethod]
    public void MigrationThatCannotBeWrittenIsUsedAndRetried()
    {
        File.WriteAllText(configPath, Saved42);
        File.WriteAllText(userPath, User42);
        File.SetAttributes(configPath, FileAttributes.ReadOnly);
        Directory.CreateDirectory(configPath + ".tmp");
        try
        {
            var store = Store();
            Assert.IsTrue(store.Load(out var config, out var error), error);
            Assert.AreEqual("Herald", config!.ChatSenderName);
            Assert.IsTrue(File.Exists(userPath), "takaro.cfg stays until the migration is saved.");

            Directory.Delete(configPath + ".tmp");
            File.SetAttributes(configPath, FileAttributes.Normal);
            store.CheckForChanges();

            Assert.IsFalse(LegacyConfigMigration.IsLegacy(File.ReadAllText(configPath)));
            Assert.AreEqual("Herald", Read()["chatSenderName"]);
            Assert.IsFalse(File.Exists(userPath));
        }
        finally
        {
            if (Directory.Exists(configPath + ".tmp"))
            {
                Directory.Delete(configPath + ".tmp");
            }

            File.SetAttributes(configPath, FileAttributes.Normal);
        }
    }

    [TestMethod]
    public void ResolveIgnoresCurrentFiles()
    {
        Assert.IsNull(LegacyConfigMigration.Resolve(ConnectorConfigFiles.NewFile("id", "name"), User42));
        Assert.IsNull(LegacyConfigMigration.Resolve(null, null));
    }

    private ConnectorConfigStore Store() =>
        new(configPath, info.Add, _ => { }, () => $"generated-{++generated}", TimeSpan.FromMilliseconds(10),
            hostServerName: "Odin's Hall", legacyUserFilePath: userPath);

    private ConnectorConfig Load()
    {
        Assert.IsTrue(Store().Load(out var config, out var error), error);
        return config!;
    }

    private void Set(string key, string value) =>
        File.WriteAllText(configPath, ConnectorConfigFiles.SetValues(File.ReadAllText(configPath), new Dictionary<string, string> { [key] = value }));

    private Dictionary<string, string> Read() => ConnectorConfigFiles.Parse(File.ReadAllText(configPath));
}
