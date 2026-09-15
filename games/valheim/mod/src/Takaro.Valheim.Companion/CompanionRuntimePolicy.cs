namespace Takaro.Valheim.Companion;

public static class CompanionRuntimePolicy
{
    public static bool IsGraphicalValheimClient(
        bool isBatchMode,
        string? processName,
        string? executablePath)
    {
        if (isBatchMode)
        {
            return false;
        }

        var processFileName = NormalizeFileName(processName);
        var executableFileName = NormalizeFileName(executablePath);
        if (IsDedicatedServer(processFileName)
            || IsDedicatedServer(executableFileName))
        {
            return false;
        }

        return IsGraphicalClient(processFileName)
            || IsGraphicalClient(executableFileName);
    }

    private static bool IsDedicatedServer(string fileName) =>
        fileName == "valheim_server"
        || fileName == "valheim_server.exe"
        || fileName == "valheim_server.x86_64";

    private static bool IsGraphicalClient(string fileName) =>
        fileName == "valheim"
        || fileName == "valheim.exe"
        || fileName == "valheim.x86_64"
        || fileName == "valheim.app";

    private static string NormalizeFileName(string? value)
    {
        if (string.IsNullOrWhiteSpace(value))
        {
            return string.Empty;
        }

        var normalized = value!.Trim().Replace('\\', '/');
        var separator = normalized.LastIndexOf('/');
        if (separator >= 0)
        {
            normalized = normalized.Substring(separator + 1);
        }

        return normalized.ToLowerInvariant();
    }
}
