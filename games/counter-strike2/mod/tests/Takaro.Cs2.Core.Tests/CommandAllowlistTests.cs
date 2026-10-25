using Microsoft.VisualStudio.TestTools.UnitTesting;
using Takaro.Cs2.Core;

namespace Takaro.Cs2.Core.Tests;

[TestClass]
public sealed class CommandAllowlistTests
{
    [TestMethod]
    public void AllowsExactCommandsAndConfiguredPrefixesOnly()
    {
        var allowlist = new CommandAllowlist(
            ExactCommands: ["status"],
            Prefixes: ["mp_restartgame ", "say "]);

        Assert.IsTrue(allowlist.IsAllowed("status"));
        Assert.IsTrue(allowlist.IsAllowed(" mp_restartgame 1 "));
        Assert.IsTrue(allowlist.IsAllowed("say Takaro test"));
        Assert.IsFalse(allowlist.IsAllowed("quit"));
        Assert.IsFalse(allowlist.IsAllowed("mp_restartgameevil 1"));
    }
}
