using Microsoft.VisualStudio.TestTools.UnitTesting;
using Takaro.Valheim.Core;

namespace Takaro.Valheim.Core.Tests;

[TestClass]
public sealed class ConsoleCommandPolicyTests
{
    [TestMethod]
    public void DefaultPolicyAllowsOnlyExactHelp()
    {
        var policy = ConsoleCommandPolicy.Default;

        Assert.IsTrue(policy.IsAllowed("help"));
        Assert.IsTrue(policy.IsAllowed("  help  "));
        Assert.IsFalse(policy.IsAllowed("players"));
    }

    [TestMethod]
    public void ExactAllowlistDoesNotPermitPrefixAbuse()
    {
        var policy = new ConsoleCommandPolicy(new[] { "help" }, Array.Empty<string>());

        Assert.IsFalse(policy.IsAllowed("help players"));
        Assert.IsFalse(policy.IsAllowed("help;shutdown"));
    }

    [TestMethod]
    public void PrefixAllowlistRequiresConfiguredBoundary()
    {
        var policy = new ConsoleCommandPolicy(Array.Empty<string>(), new[] { "say" });

        Assert.IsTrue(policy.IsAllowed("say hello"));
        Assert.IsTrue(policy.IsAllowed("say"));
        Assert.IsFalse(policy.IsAllowed("sayanything"));
    }
}
