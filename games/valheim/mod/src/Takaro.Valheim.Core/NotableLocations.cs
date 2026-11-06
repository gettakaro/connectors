using System.Globalization;

namespace Takaro.Valheim.Core;

/// <summary>One placed location of the world, as ZoneSystem knows it on the dedicated server.</summary>
public readonly record struct ValheimLocationInstance(string Prefab, double X, double Y, double Z);

/// <summary>
/// The places worth a map marker. A world holds about 11,000 location instances (runestones,
/// camps, ruins); listLocations returns only the start temple, the boss altars and the traders,
/// each under its English in-game name, one entry per placed instance.
/// </summary>
public static class NotableLocations
{
    private static readonly IReadOnlyDictionary<string, string> DisplayNames =
        new Dictionary<string, string>(StringComparer.Ordinal)
        {
            ["StartTemple"] = "Sacrificial Stones",
            ["Eikthyrnir"] = "Eikthyr Altar",
            ["GDKing"] = "The Elder Altar",
            ["Bonemass"] = "Bonemass Altar",
            ["Dragonqueen"] = "Moder Altar",
            ["GoblinKing"] = "Yagluth Altar",
            ["Mistlands_DvergrBossEntrance1"] = "The Queen's Infested Citadel",
            ["FaderLocation"] = "Fader Altar",
            ["Vendor_BlackForest"] = "Haldor the Trader",
            ["Hildir_camp"] = "Hildir's Camp",
            ["BogWitch_Camp"] = "Bog Witch's Camp",
        };

    public static IReadOnlyCollection<string> Prefabs => (IReadOnlyCollection<string>)DisplayNames.Keys;

    public static bool TryGetDisplayName(string? prefab, out string displayName)
    {
        if (prefab is not null && DisplayNames.TryGetValue(prefab, out var name))
        {
            displayName = name;
            return true;
        }

        displayName = string.Empty;
        return false;
    }

    /// <summary>
    /// The notable instances as Takaro locations in the map's one dimension. The code keeps the
    /// prefab and the rounded ground position, so every placed altar or trader camp is its own
    /// stable location across resyncs.
    /// </summary>
    public static TakaroLocation[] Select(IEnumerable<ValheimLocationInstance> instances) =>
        instances
            .Select(instance => (Instance: instance, Found: TryGetDisplayName(instance.Prefab, out var name), Name: name))
            .Where(entry => entry.Found)
            .Select(entry => new TakaroLocation(
                entry.Name,
                new TakaroPosition(
                    Math.Round(entry.Instance.X, 1),
                    Math.Round(entry.Instance.Y, 1),
                    Math.Round(entry.Instance.Z, 1),
                    WorldMapFrame.DimensionId),
                Code(entry.Instance)))
            .GroupBy(location => location.Code, StringComparer.Ordinal)
            .Select(group => group.First())
            .OrderBy(location => location.Name, StringComparer.Ordinal)
            .ThenBy(location => location.Code, StringComparer.Ordinal)
            .ToArray();

    private static string Code(ValheimLocationInstance instance) =>
        string.Format(
            CultureInfo.InvariantCulture,
            "{0}@{1}_{2}",
            instance.Prefab,
            (long)Math.Round(instance.X),
            (long)Math.Round(instance.Z));
}
