using System.Diagnostics;
using Microsoft.VisualStudio.TestTools.UnitTesting;

namespace Takaro.Valheim.Core.Tests;

[TestClass]
public sealed class SetupEnvironmentScriptTests
{
    [TestMethod]
    public void SetupScriptPassesDeterministicRetryFallbackAndSafetyHarness()
    {
        var valheimDirectory = Path.GetFullPath(Path.Combine(
            AppContext.BaseDirectory,
            "../../../../../"));
        var harness = Path.Combine(valheimDirectory, "tests/setup-environment-behavior.sh");
        var startInfo = new ProcessStartInfo
        {
            FileName = "/usr/bin/env",
            WorkingDirectory = valheimDirectory,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            UseShellExecute = false
        };
        startInfo.ArgumentList.Add("bash");
        startInfo.ArgumentList.Add(harness);

        using var process = Process.Start(startInfo)!;
        var standardOutput = process.StandardOutput.ReadToEnd();
        var standardError = process.StandardError.ReadToEnd();
        process.WaitForExit();

        Assert.AreEqual(
            0,
            process.ExitCode,
            $"Setup behavior harness failed.\nSTDOUT:\n{standardOutput}\nSTDERR:\n{standardError}");
        StringAssert.Contains(standardOutput, "All setup behavior tests passed");
    }
}
