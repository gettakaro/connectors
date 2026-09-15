using Microsoft.VisualStudio.TestTools.UnitTesting;
using Takaro.Valheim.Core;

namespace Takaro.Valheim.Core.Tests;

[TestClass]
public sealed class ValheimRuntimePolicyTests
{
    [DataTestMethod]
    [DataRow("valheim_server", "/opt/valheim/valheim_server.x86_64")]
    [DataRow("valheim_server.x86_64", "/opt/valheim/valheim_server.x86_64")]
    [DataRow("valheim_server", "C:\\Valheim\\valheim_server.exe")]
    public void AcceptsBatchDedicatedServerExecutables(string processName, string executablePath)
    {
        Assert.IsTrue(ValheimRuntimePolicy.IsDedicatedServerProcess(
            isBatchMode: true,
            processName,
            executablePath));
    }

    [TestMethod]
    public void RejectsDedicatedServerExecutableOutsideBatchMode()
    {
        Assert.IsFalse(ValheimRuntimePolicy.IsDedicatedServerProcess(
            isBatchMode: false,
            processName: "valheim_server.x86_64",
            executablePath: "/opt/valheim/valheim_server.x86_64"));
    }

    [DataTestMethod]
    [DataRow("valheim", "/opt/valheim/valheim.x86_64")]
    [DataRow("Unity", "/opt/tools/Unity")]
    [DataRow("bash", "/usr/bin/bash")]
    public void RejectsOtherBatchUnityOrShellProcesses(string processName, string executablePath)
    {
        Assert.IsFalse(ValheimRuntimePolicy.IsDedicatedServerProcess(
            isBatchMode: true,
            processName,
            executablePath));
    }

    [TestMethod]
    public void RejectsLooseArgumentTextThatOnlyMentionsServerExecutable()
    {
        Assert.IsFalse(ValheimRuntimePolicy.IsDedicatedServerProcess(
            isBatchMode: true,
            processName: "dotnet",
            executablePath: "--config=/tmp/valheim_server.x86_64.json"));
    }
}
