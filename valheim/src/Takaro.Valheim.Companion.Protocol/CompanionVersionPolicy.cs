namespace Takaro.Valheim.Companion.Protocol;

public static class CompanionVersionPolicy
{
    public static bool TryNegotiate(
        int localMinimum,
        int localMaximum,
        int remoteMinimum,
        int remoteMaximum,
        out int selected)
    {
        selected = 0;

        if (localMinimum <= 0
            || localMaximum <= 0
            || remoteMinimum <= 0
            || remoteMaximum <= 0
            || localMinimum > localMaximum
            || remoteMinimum > remoteMaximum)
        {
            return false;
        }

        var highestCommonVersion = Math.Min(localMaximum, remoteMaximum);
        var lowestCommonVersion = Math.Max(localMinimum, remoteMinimum);
        if (highestCommonVersion < lowestCommonVersion)
        {
            return false;
        }

        selected = highestCommonVersion;
        return true;
    }
}
