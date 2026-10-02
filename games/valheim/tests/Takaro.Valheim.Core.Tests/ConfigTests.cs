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
    public void FromDictionaryDefaultsChatSenderNameToTakaro()
    {
        var config = ConnectorConfig.FromDictionary(new Dictionary<string, string>
        {
            ["registrationToken"] = "reg-123",
            ["serverName"] = "Meadows"
        });

        Assert.AreEqual("Takaro", config.ChatSenderName);
        Assert.AreEqual(ConnectorConfig.DefaultChatSenderName, config.ChatSenderName);
    }

    [TestMethod]
    public void FromDictionaryTrimsConfiguredChatSenderName()
    {
        var config = ConnectorConfig.FromDictionary(new Dictionary<string, string>
        {
            ["registrationToken"] = "reg-123",
            ["serverName"] = "Meadows",
            ["chatSenderName"] = "  Server Bot  "
        });

        Assert.AreEqual("Server Bot", config.ChatSenderName);
    }

    [TestMethod]
    public void FromDictionaryDefaultsBlankOrNullChatSenderName()
    {
        foreach (var value in new string?[] { "", "   ", null })
        {
            var config = ConnectorConfig.FromDictionary(new Dictionary<string, string>
            {
                ["registrationToken"] = "reg-123",
                ["serverName"] = "Meadows",
                ["chatSenderName"] = value!
            });

            Assert.AreEqual("Takaro", config.ChatSenderName);
        }
    }

    [TestMethod]
    public void FromDictionaryAcceptsChatSenderNameAtTheLengthLimit()
    {
        var name = new string('n', ConnectorConfig.MaximumChatSenderNameCharacters);
        var config = ConnectorConfig.FromDictionary(new Dictionary<string, string>
        {
            ["registrationToken"] = "reg-123",
            ["serverName"] = "Meadows",
            ["chatSenderName"] = name
        });

        Assert.AreEqual(128, ConnectorConfig.MaximumChatSenderNameCharacters);
        Assert.AreEqual(name, config.ChatSenderName);
    }

    [TestMethod]
    public void TryFromDictionaryRejectsOverlongChatSenderName()
    {
        var ok = ConnectorConfig.TryFromDictionary(new Dictionary<string, string>
        {
            ["registrationToken"] = "reg-123",
            ["serverName"] = "Meadows",
            ["chatSenderName"] = new string('n', ConnectorConfig.MaximumChatSenderNameCharacters + 1)
        }, out var config, out var error);

        Assert.IsFalse(ok);
        Assert.IsNull(config);
        StringAssert.Contains(error, "chatSenderName");
    }

    [TestMethod]
    public void FromDictionaryIgnoresRetiredCompanionModeKey()
    {
        var config = ConnectorConfig.FromDictionary(new Dictionary<string, string>
        {
            ["registrationToken"] = "reg-123",
            ["serverName"] = "Meadows",
            ["companionMode"] = "required"
        });

        Assert.AreEqual("Meadows", config.ServerName);
        Assert.IsNull(typeof(ConnectorConfig).GetProperty("CompanionMode"));
    }
}
