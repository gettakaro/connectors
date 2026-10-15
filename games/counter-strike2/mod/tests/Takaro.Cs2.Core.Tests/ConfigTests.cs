using Microsoft.VisualStudio.TestTools.UnitTesting;
using Takaro.Cs2.Core;

namespace Takaro.Cs2.Core.Tests;

[TestClass]
public sealed class ConfigTests
{
    [TestMethod]
    public void ValidateRejectsMissingTakaroUrl()
    {
        var config = new ConnectorConfig
        {
            TakaroWsUrl = "",
            ServerName = "CS2",
            RegistrationToken = "registration"
        };

        var errors = config.Validate();

        CollectionAssert.Contains(errors.ToList(), "TakaroWsUrl is required.");
    }

    [TestMethod]
    public void ValidateRequiresStableIdentityOnFirstRun()
    {
        var config = new ConnectorConfig
        {
            TakaroWsUrl = "wss://connect.takaro.io/",
            ServerName = "CS2",
            RegistrationToken = "registration"
        };

        CollectionAssert.Contains(config.Validate().ToList(), "IdentityToken is required and must be stable for this server.");
    }
}
