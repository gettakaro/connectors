using System.Reflection;
using Microsoft.VisualStudio.TestTools.UnitTesting;

namespace Takaro.Valheim.Core.Tests;

[TestClass]
public sealed class CompanionProtocolTests
{
    private const string ProtocolNamespace = "Takaro.Valheim.Companion.Protocol";

    private static readonly string[] ReportTypeNames =
    [
        "CompanionChatReport",
        "CompanionInventoryReport",
        "CompanionPlayerDeathReport",
        "CompanionEntityKilledReport"
    ];

    [TestMethod]
    public void VersionOneProtocolConstantsAreStable()
    {
        Assert.AreEqual("TakaroCompanionV1", GetConstant<string>("CompanionProtocol", "RpcName"));
        Assert.AreEqual(1, GetConstant<int>("CompanionProtocol", "CurrentVersion"));
        Assert.AreEqual(1, GetConstant<int>("CompanionProtocol", "MinimumVersion"));
    }

    [TestMethod]
    public void VersionOneCapabilityValuesAreStable()
    {
        var capabilityType = RequireType("CompanionCapability");

        Assert.IsTrue(capabilityType.IsEnum);
        Assert.IsTrue(capabilityType.IsDefined(typeof(FlagsAttribute), inherit: false));
        Assert.AreEqual(0, GetEnumValue(capabilityType, "None"));
        Assert.AreEqual(1, GetEnumValue(capabilityType, "Chat"));
        Assert.AreEqual(2, GetEnumValue(capabilityType, "Inventory"));
        Assert.AreEqual(4, GetEnumValue(capabilityType, "PlayerDeath"));
        Assert.AreEqual(8, GetEnumValue(capabilityType, "EntityKilled"));
    }

    [TestMethod]
    public void VersionOneBoundsAreStableAndFinite()
    {
        Assert.AreEqual(64 * 1024, GetConstant<int>("CompanionProtocol", "MaximumEnvelopeUtf8Bytes"));
        Assert.AreEqual(512, GetConstant<int>("CompanionProtocol", "MaximumChatCharacters"));
        Assert.AreEqual(256, GetConstant<int>("CompanionProtocol", "MaximumInventoryStacks"));
        Assert.AreEqual(128, GetConstant<int>("CompanionProtocol", "MaximumCodeCharacters"));
        Assert.AreEqual(64, GetConstant<int>("CompanionProtocol", "MaximumEventIdCharacters"));
    }

    [TestMethod]
    public void VersionOneMessageTypesAreStable()
    {
        Assert.AreEqual("hello", GetConstant<string>("CompanionMessageTypes", "Hello"));
        Assert.AreEqual("hello-ack", GetConstant<string>("CompanionMessageTypes", "HelloAck"));
        Assert.AreEqual("heartbeat", GetConstant<string>("CompanionMessageTypes", "Heartbeat"));
        Assert.AreEqual("chat", GetConstant<string>("CompanionMessageTypes", "Chat"));
        Assert.AreEqual("inventory-snapshot", GetConstant<string>("CompanionMessageTypes", "InventorySnapshot"));
        Assert.AreEqual("player-death", GetConstant<string>("CompanionMessageTypes", "PlayerDeath"));
        Assert.AreEqual("entity-killed", GetConstant<string>("CompanionMessageTypes", "EntityKilled"));
    }

    [TestMethod]
    public void VersionOneEnvelopeHasOnlyTransportMetadataAndPayload()
    {
        AssertPublicProperties(
            RequireType("CompanionEnvelope"),
            "ProtocolVersion",
            "SessionNonce",
            "Sequence",
            "MessageId",
            "Type",
            "Payload");
    }

    [TestMethod]
    public void VersionOneDefinesRequiredControlAndReportPayloads()
    {
        foreach (var typeName in new[]
                 {
                     "CompanionHello",
                     "CompanionHelloAck",
                     "CompanionHeartbeat",
                     "CompanionChatReport",
                     "CompanionInventoryReport",
                     "CompanionPlayerDeathReport",
                     "CompanionEntityKilledReport"
                 })
        {
            Assert.IsNotNull(RequireType(typeName), typeName);
        }
    }

    [TestMethod]
    public void VersionOneInventoryReportsDescribeOnlyInventoryContent()
    {
        AssertPublicProperties(RequireType("CompanionInventoryReport"), "Stacks");
        AssertPublicProperties(
            RequireType("CompanionInventoryStack"),
            "Code",
            "Name",
            "Amount",
            "Quality",
            "Durability",
            "Equipped",
            "Slot");
    }

    [TestMethod]
    public void VersionOneReportsContainNoClaimedPlayerIdentity()
    {
        foreach (var typeName in ReportTypeNames.Append("CompanionEnvelope"))
        {
            foreach (var property in EnumerateProtocolProperties(RequireType(typeName)))
            {
                Assert.IsFalse(
                    IsClaimedPlayerIdentity(property.Name),
                    $"{property.DeclaringType!.Name}.{property.Name} must not claim player identity; the server derives identity from the RPC peer/session.");
            }
        }
    }

    private static Assembly ProtocolAssembly => Assembly.Load(ProtocolNamespace);

    private static Type RequireType(string typeName)
    {
        var type = ProtocolAssembly.GetType($"{ProtocolNamespace}.{typeName}");
        Assert.IsNotNull(type, $"Expected protocol type {typeName}.");
        return type!;
    }

    private static T GetConstant<T>(string typeName, string fieldName)
    {
        var field = RequireType(typeName).GetField(fieldName, BindingFlags.Public | BindingFlags.Static);
        Assert.IsNotNull(field, $"Expected public constant {typeName}.{fieldName}.");
        Assert.IsTrue(field!.IsLiteral && !field.IsInitOnly, $"{typeName}.{fieldName} must be a constant.");
        return (T)field.GetRawConstantValue()!;
    }

    private static int GetEnumValue(Type enumType, string name)
    {
        Assert.IsTrue(Enum.IsDefined(enumType, name), $"Expected {enumType.Name}.{name}.");
        return Convert.ToInt32(Enum.Parse(enumType, name));
    }

    private static void AssertPublicProperties(Type type, params string[] expectedNames)
    {
        var actualNames = type
            .GetProperties(BindingFlags.Public | BindingFlags.Instance)
            .Select(property => property.Name)
            .ToArray();

        CollectionAssert.AreEquivalent(expectedNames, actualNames, $"Unexpected public shape for {type.Name}.");
    }

    private static IEnumerable<PropertyInfo> EnumerateProtocolProperties(Type rootType)
    {
        var pending = new Queue<Type>();
        var visited = new HashSet<Type>();
        pending.Enqueue(rootType);

        while (pending.Count > 0)
        {
            var type = pending.Dequeue();
            if (!visited.Add(type))
            {
                continue;
            }

            foreach (var property in type.GetProperties(BindingFlags.Public | BindingFlags.Instance))
            {
                yield return property;

                var nestedType = UnwrapCollection(property.PropertyType);
                if (nestedType.Namespace == ProtocolNamespace && !nestedType.IsEnum)
                {
                    pending.Enqueue(nestedType);
                }
            }
        }
    }

    private static Type UnwrapCollection(Type type)
    {
        if (type.IsArray)
        {
            return type.GetElementType()!;
        }

        return type.IsGenericType && type.GetGenericArguments().Length == 1
            ? type.GetGenericArguments()[0]
            : type;
    }

    private static bool IsClaimedPlayerIdentity(string propertyName)
    {
        var normalized = new string(propertyName.Where(char.IsLetterOrDigit).Select(char.ToLowerInvariant).ToArray());

        return normalized.Contains("claimedplayer", StringComparison.Ordinal)
            || normalized is "playerid" or "playername" or "platformid" or "platformuserid"
                or "steamid" or "xboxid" or "networkid" or "peerid" or "gameid"
                or "userid" or "accountid" or "characterid";
    }
}
