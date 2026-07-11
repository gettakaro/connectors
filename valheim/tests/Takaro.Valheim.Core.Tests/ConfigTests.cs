using Microsoft.VisualStudio.TestTools.UnitTesting;
using Takaro.Valheim.Core;

namespace Takaro.Valheim.Core.Tests;

[TestClass]
public sealed class ConfigTests
{
    [TestMethod]
    public void FromDictionaryRequiresRegistrationTokenAndServerName()
    {
        var error = Assert.ThrowsException<ArgumentException>(() =>
            ConnectorConfig.FromDictionary(new Dictionary<string, string>()));

        StringAssert.Contains(error.Message, "registrationToken");
        StringAssert.Contains(error.Message, "serverName");
    }

    [TestMethod]
    public void FromDictionaryAppliesDefaultsAndParsesBooleans()
    {
        var config = ConnectorConfig.FromDictionary(new Dictionary<string, string>
        {
            ["registrationToken"] = "reg-123",
            ["serverName"] = "Meadows",
            ["identityToken"] = "identity-456",
            ["enableLogEvents"] = "false"
        });

        Assert.AreEqual("reg-123", config.RegistrationToken);
        Assert.AreEqual("Meadows", config.ServerName);
        Assert.AreEqual("identity-456", config.IdentityToken);
        Assert.AreEqual("wss://connect.takaro.io/", config.TakaroWsUrl);
        Assert.AreEqual("Information", config.LogLevel);
        Assert.IsFalse(config.EnableLogEvents);
        CollectionAssert.AreEqual(new[] { "help" }, config.CommandAllowlistExact.ToArray());
        Assert.AreEqual(0, config.CommandAllowlistPrefixes.Count);
    }

    [TestMethod]
    public void TryFromDictionaryReturnsErrorInsteadOfThrowingForMissingCredentials()
    {
        var ok = ConnectorConfig.TryFromDictionary(new Dictionary<string, string>
        {
            ["serverName"] = "Meadows"
        }, out var config, out var error);

        Assert.IsFalse(ok);
        Assert.IsNull(config);
        StringAssert.Contains(error, "registrationToken");
    }

    [TestMethod]
    public void FromDictionaryDefaultsIdentityTokenToServerName()
    {
        var config = ConnectorConfig.FromDictionary(new Dictionary<string, string>
        {
            ["registrationToken"] = "reg-123",
            ["serverName"] = "Meadows"
        });

        Assert.AreEqual("Meadows", config.IdentityToken);
    }

    [TestMethod]
    public void FromDictionaryParsesCommandAllowlists()
    {
        var config = ConnectorConfig.FromDictionary(new Dictionary<string, string>
        {
            ["registrationToken"] = "reg-123",
            ["serverName"] = "Meadows",
            ["commandAllowlistExact"] = "help;players",
            ["commandAllowlistPrefixes"] = "say , broadcast "
        });

        CollectionAssert.AreEqual(new[] { "help", "players" }, config.CommandAllowlistExact.ToArray());
        CollectionAssert.AreEqual(new[] { "say", "broadcast" }, config.CommandAllowlistPrefixes.ToArray());
    }

    [TestMethod]
    public void FromDictionaryAppliesExactCompanionDefaults()
    {
        var config = ConnectorConfig.FromDictionary(new Dictionary<string, string>
        {
            ["registrationToken"] = "reg-123",
            ["serverName"] = "Meadows"
        });

        Assert.AreEqual("Required", ReadCompanionMode(config));
        CollectionAssert.AreEqual(new[] { "$" }, ReadCompanionCommandPrefixes(config));
    }

    [DataTestMethod]
    [DataRow("disabled", "Disabled")]
    [DataRow("oPtIoNaL", "Optional")]
    [DataRow("REQUIRED", "Required")]
    public void FromDictionaryParsesEveryCompanionModeCaseInsensitively(
        string configuredValue,
        string expectedMode)
    {
        var config = ConnectorConfig.FromDictionary(new Dictionary<string, string>
        {
            ["registrationToken"] = "reg-123",
            ["serverName"] = "Meadows",
            ["companionMode"] = configuredValue
        });

        Assert.AreEqual(expectedMode, ReadCompanionMode(config));
    }

    [TestMethod]
    public void FromDictionaryParsesCompanionCommandPrefixes()
    {
        var config = ConnectorConfig.FromDictionary(new Dictionary<string, string>
        {
            ["registrationToken"] = "reg-123",
            ["serverName"] = "Meadows",
            ["companionCommandPrefixes"] = "! ; /,  ? "
        });

        CollectionAssert.AreEqual(
            new[] { "!", "/", "?" },
            ReadCompanionCommandPrefixes(config));
    }

    [DataTestMethod]
    [DataRow("enabled")]
    [DataRow("1")]
    [DataRow("optional|required")]
    public void TryFromDictionaryRejectsEveryOtherNonblankCompanionMode(string configuredValue)
    {
        var ok = ConnectorConfig.TryFromDictionary(new Dictionary<string, string>
        {
            ["registrationToken"] = "reg-123",
            ["serverName"] = "Meadows",
            ["companionMode"] = configuredValue
        }, out var config, out var error);

        Assert.IsFalse(ok);
        Assert.IsNull(config);
        StringAssert.Contains(error, "companionMode");
    }

    private static string ReadCompanionMode(ConnectorConfig config)
    {
        var property = typeof(ConnectorConfig).GetProperty("CompanionMode")
            ?? throw new AssertFailedException("ConnectorConfig is missing CompanionMode.");

        return property.GetValue(config)?.ToString()
            ?? throw new AssertFailedException("ConnectorConfig.CompanionMode is null.");
    }

    private static string[] ReadCompanionCommandPrefixes(ConnectorConfig config)
    {
        var property = typeof(ConnectorConfig).GetProperty("CompanionCommandPrefixes")
            ?? throw new AssertFailedException("ConnectorConfig is missing CompanionCommandPrefixes.");
        var prefixes = property.GetValue(config) as IEnumerable<string>
            ?? throw new AssertFailedException("ConnectorConfig.CompanionCommandPrefixes is not a string list.");

        return prefixes.ToArray();
    }
}
