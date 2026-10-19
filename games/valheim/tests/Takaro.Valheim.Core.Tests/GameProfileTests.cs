using System.Security.Cryptography;
using System.Text.Json;
using System.Text.RegularExpressions;
using Microsoft.VisualStudio.TestTools.UnitTesting;
using Takaro.Valheim.Core;
using Takaro.Valheim.Plugin;

namespace Takaro.Valheim.Core.Tests;

/// <summary>The identify fields, the game.profile capability and the getGameIcon answer, against Takaro's limits.</summary>
[TestClass]
public sealed class GameProfileTests
{
    private static readonly byte[] PngSignature = [0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A];

    [TestMethod]
    public void ReportedConnectorVersionIsVersionTxt()
    {
        var versionTxt = ReadValheimFile("version.txt").Trim();

        Assert.AreEqual(versionTxt, ValheimTakaroPlugin.ReleaseVersion);
        Assert.AreEqual(versionTxt, Profile().ConnectorVersion);
    }

    [TestMethod]
    public void IdentifyNamesTheGameAndTheConnector()
    {
        var payload = JsonDocument.Parse(TakaroProtocol.CreateIdentify(Config(), Profile())).RootElement.GetProperty("payload");
        var slug = new Regex("^[a-z0-9][a-z0-9-]*$");

        Assert.AreEqual("valheim", payload.GetProperty("game").GetString());
        Assert.AreEqual("takaro-valheim", payload.GetProperty("connector").GetProperty("name").GetString());
        Assert.AreEqual(ValheimTakaroPlugin.ReleaseVersion, payload.GetProperty("connector").GetProperty("version").GetString());
        StringAssert.Matches(payload.GetProperty("game").GetString(), slug);
        StringAssert.Matches(payload.GetProperty("connector").GetProperty("name").GetString(), slug);
    }

    [TestMethod]
    public void IdentifyWithoutAProfileStaysAsBefore()
    {
        var payload = JsonDocument.Parse(TakaroProtocol.CreateIdentify(Config())).RootElement.GetProperty("payload");

        Assert.IsFalse(payload.TryGetProperty("game", out _));
        Assert.IsFalse(payload.TryGetProperty("connector", out _));
        Assert.IsFalse(Names(payload).Contains("game.profile"));
    }

    [TestMethod]
    public void ManifestDeclaresTheGameProfileWithinTakarosLimits()
    {
        var payload = JsonDocument.Parse(TakaroProtocol.CreateIdentify(Config(), Profile("1.0.17"))).RootElement.GetProperty("payload");
        var entry = payload.GetProperty("capabilities").EnumerateArray().Single(c => c.GetProperty("name").GetString() == "game.profile");
        var parameters = entry.GetProperty("parameters");
        var icon = parameters.GetProperty("icon");
        var shipped = File.ReadAllBytes(ValheimPath("assets/icon-256.png"));

        Assert.AreEqual("Valheim", parameters.GetProperty("name").GetString());
        Assert.AreEqual("1.0.17", parameters.GetProperty("version").GetString());
        Assert.AreEqual(shipped.Length, icon.GetProperty("size").GetInt32());
        Assert.AreEqual(Sha256Prefix(shipped), icon.GetProperty("revision").GetString());
        StringAssert.Matches(icon.GetProperty("revision").GetString(), new Regex("^[\\x21-\\x7E]{1,128}$"));
    }

    [TestMethod]
    public void ShippedIconIsA256PngWithinTheSizeLimit()
    {
        var shipped = File.ReadAllBytes(ValheimPath("assets/icon-256.png"));

        CollectionAssert.AreEqual(PngSignature, shipped.Take(8).ToArray());
        Assert.AreEqual("IHDR", System.Text.Encoding.ASCII.GetString(shipped, 12, 4));
        Assert.AreEqual(256, BigEndian(shipped, 16), "width");
        Assert.AreEqual(256, BigEndian(shipped, 20), "height");
        Assert.IsTrue(shipped.Length <= 128 * 1024, $"{shipped.Length} bytes");
        CollectionAssert.AreEqual(shipped, GameProfile.LoadEmbeddedIcon(), "the plugin serves exactly the committed file");
    }

    [TestMethod]
    public void GetGameIconServesTheWholeFileUnderTheDeclaredRevision()
    {
        var profile = Profile();

        var result = profile.GetGameIcon();

        Assert.IsTrue(result.Success);
        using var payload = JsonDocument.Parse(JsonSerializer.Serialize(result.Payload));
        var bytes = Convert.FromBase64String(payload.RootElement.GetProperty("data").GetString()!);
        Assert.AreEqual(profile.IconRevision, payload.RootElement.GetProperty("revision").GetString());
        Assert.AreEqual(profile.IconSize, bytes.Length);
        CollectionAssert.AreEqual(GameProfile.LoadEmbeddedIcon(), bytes);
    }

    [TestMethod]
    public void ChangingTheCallersArrayCannotChangeTheServedIcon()
    {
        var bytes = GameProfile.LoadEmbeddedIcon()!;
        var profile = new GameProfile("4.2.0", null, bytes);

        bytes[100] ^= 0xFF;

        using var payload = JsonDocument.Parse(JsonSerializer.Serialize(profile.GetGameIcon().Payload));
        Assert.AreEqual(profile.IconRevision, GameProfile.RevisionOf(Convert.FromBase64String(payload.RootElement.GetProperty("data").GetString()!)));
    }

    [TestMethod]
    public void WithoutIconBytesNoIconIsDeclaredAndGetGameIconIsAGameError()
    {
        var profile = new GameProfile("4.2.0", "1.0.17", icon: null);

        Assert.IsNull(profile.IconRevision);
        Assert.IsFalse(profile.ToCapability()["parameters"]!.AsObject().ContainsKey("icon"));
        Assert.IsFalse(ProtocolHandshake.IsDeclared(TakaroActionNames.GetGameIcon, profile));
        Assert.AreEqual(ProtocolErrorCodes.GameError, profile.GetGameIcon().ErrorCode);
    }

    [TestMethod]
    public void AnIconOverTheLimitIsNeverDeclared()
    {
        var profile = new GameProfile("4.2.0", null, new byte[128 * 1024 + 1]);

        Assert.IsNull(profile.IconRevision);
        Assert.IsTrue(ProtocolHandshake.IsDeclared(TakaroActionNames.GetGameIcon, Profile()));
    }

    [TestMethod]
    [DataRow("1.0.17", "1.0.17")]
    [DataRow("  1.0.17\n", "1.0.17")]
    [DataRow("", null)]
    [DataRow("   ", null)]
    [DataRow(null, null)]
    [DataRow("1.0.17-ö", null)]
    [DataRow("1.0.17\nrev", null)]
    [DataRow("123456789012345678901234567890123", null)]
    public void GameBuildIsOmittedRatherThanCutOrMangled(string? raw, string? expected)
    {
        Assert.AreEqual(expected, GameProfile.NormalizeGameVersion(raw));
        Assert.AreEqual(expected is not null, new GameProfile("4.2.0", raw, null).ToCapability()["parameters"]!.AsObject().ContainsKey("version"));
    }

    [TestMethod]
    [DataRow("4.2.0")]
    [DataRow("4.3.0-dev.1")]
    [DataRow("4.2.0+abc1234")]
    public void StrictSemanticVersionsAreAccepted(string version)
    {
        Assert.AreEqual(version, new GameProfile(version, null, null).ConnectorVersion);
    }

    [TestMethod]
    [DataRow("4.2")]
    [DataRow("v4.2.0")]
    [DataRow("4.2.0-01")]
    [DataRow("4.2.0.0")]
    public void AnythingElseNeverReachesIdentify(string version)
    {
        Assert.ThrowsException<ArgumentException>(() => new GameProfile(version, null, null));
    }

    private static GameProfile Profile(string? gameVersion = "1.0.17") =>
        new(ValheimTakaroPlugin.ReleaseVersion, gameVersion, GameProfile.LoadEmbeddedIcon());

    private static ConnectorConfig Config() =>
        ConnectorConfig.FromDictionary(new Dictionary<string, string>
        {
            ["registrationToken"] = "token",
            ["identityToken"] = "identity",
            ["serverName"] = "Valheim Test"
        });

    private static string[] Names(JsonElement payload) =>
        payload.GetProperty("capabilities").EnumerateArray().Select(c => c.GetProperty("name").GetString()!).ToArray();

    private static string Sha256Prefix(byte[] bytes) =>
        Convert.ToHexString(SHA256.HashData(bytes)).Substring(0, 12).ToLowerInvariant();

    private static int BigEndian(byte[] bytes, int offset) =>
        (bytes[offset] << 24) | (bytes[offset + 1] << 16) | (bytes[offset + 2] << 8) | bytes[offset + 3];

    private static string ValheimPath(string relativePath) =>
        Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, "../../../../../", relativePath));

    private static string ReadValheimFile(string relativePath) => File.ReadAllText(ValheimPath(relativePath));
}
