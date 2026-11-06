using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using Newtonsoft.Json;
using Newtonsoft.Json.Linq;
using Takaro.Services;
using Takaro.WebSocket;

// getPlayerProgression: the track rules Takaro enforces on a whole reading, and the manifest entry.
public static partial class ContractHarness
{
    private static TakaroProgressionTrack Track(string key, int level = 0) =>
        new TakaroProgressionTrack { Key = key, Level = level };

    private static void FamilyProgressionManifest()
    {
        Dictionary<string, object> entry = ProtocolHandshake
            .BuildCapabilityManifest(RequestRouter.SupportedActions)
            .Single(c => (string)c["name"] == "players.progression");
        Equal(
            "client-reported",
            (string)((Dictionary<string, object>)entry["parameters"])["source"],
            "players.progression names the client as the source"
        );
    }

    private static void FamilyProgressionTracks()
    {
        List<TakaroProgressionTrack> tracks = PlayerProgression.Build(
            13,
            48210,
            0.42,
            new[]
            {
                Track("attperception", 3),
                Track("perkminer69r", 0),
                Track("craftingarmor", 12),
            }
        );
        Equal("character", tracks[0].Key, "the character level comes first");
        Equal(13, tracks[0].Level, "character level");
        Equal(4, tracks.Count, "a perk at level 0 stays in the reading");

        JObject first = JObject.Parse(JsonConvert.SerializeObject(tracks[0]));
        Equal(0.42, (double)first["progress"], "character progress on the wire");
        Equal(48210L, (long)first["experience"], "character experience on the wire");

        JObject perk = JObject.Parse(JsonConvert.SerializeObject(tracks[2]));
        True(perk["experience"] == null, "a perk has no experience field");
        True(perk["progress"] == null, "a perk has no progress field");
        True(perk["name"] == null, "a nameless track has no name field");

        List<TakaroProgressionTrack> filtered = PlayerProgression.Build(
            1,
            0,
            0,
            new[]
            {
                Track("character", 9),
                Track("perk_withUnderscore"),
                Track("PerkCapital"),
                Track(""),
                Track(null),
                Track("p" + new string('x', 128)),
                Track("perkTwice", 1),
                Track("perkTwice", 2),
            }
        );
        Equal(2, filtered.Count, "keys Takaro would refuse are left out");
        Equal(1, filtered[0].Level, "only the real character level uses the reserved key");
        Equal(1, filtered[1].Level, "the first of two equal keys stays");

        Equal(
            PlayerProgression.MaxTracks,
            PlayerProgression
                .Build(1, 0, 0, Enumerable.Range(0, 500).Select(i => Track("perk" + i)))
                .Count,
            "a reading never holds more than 200 tracks"
        );

        var longName = Track("perkLong");
        longName.Name = new string('n', 200);
        Equal(
            PlayerProgression.MaxTextLength,
            PlayerProgression.Build(1, 0, 0, new[] { longName })[1].Name.Length,
            "a long name is cut to what Takaro accepts"
        );
    }

    // The layout Progression.Write and ProgressionValue.Write produce on V 3.4.
    private static byte[] SavedProgression(int level, int expToNext, params object[][] values)
    {
        using (var stream = new MemoryStream())
        using (var writer = new BinaryWriter(stream))
        {
            writer.Write((byte)3);
            writer.Write((ushort)level);
            writer.Write(expToNext);
            writer.Write((ushort)4);
            writer.Write(values.Length);
            foreach (object[] value in values)
            {
                writer.Write((byte)1);
                writer.Write((string)value[0]);
                writer.Write((byte)(int)value[1]);
                writer.Write(0);
            }
            writer.Write(0);
            writer.Flush();
            return stream.ToArray();
        }
    }

    private static void FamilyProgressionBlob()
    {
        ProgressionBlob saved = ProgressionBlob.Parse(
            SavedProgression(
                7,
                1234,
                new object[] { "attperception", 3 },
                new object[] { "perkminer69r", 0 },
                new object[] { "craftingarmor", 75 }
            )
        );
        Equal(7, saved.Level, "saved level");
        Equal(1234, saved.ExpToNextLevel, "saved experience to the next level");
        Equal(3, saved.Values.Count, "every saved value is read");
        Equal("craftingarmor", saved.Values[2].Key, "value name");
        Equal(75, saved.Values[2].Value, "value level");

        byte[] cut = SavedProgression(7, 1234, new object[] { "attperception", 3 });
        Array.Resize(ref cut, 14);
        bool failed = false;
        try
        {
            ProgressionBlob.Parse(cut);
        }
        catch (EndOfStreamException)
        {
            failed = true;
        }
        True(failed, "a cut-off blob fails, so the previous reading stays");
    }

    private static void FamilyProgressionExperience()
    {
        Equal(30L, PlayerProgression.TotalExperience(1, 70, _ => 100), "first level");
        Equal(230L, PlayerProgression.TotalExperience(3, 70, _ => 100), "levels passed count");
        Equal(0L, PlayerProgression.TotalExperience(1, 500, _ => 100), "never below zero");
        Equal(
            550L,
            PlayerProgression.TotalExperience(3, 50, level => level * 100),
            "each level costs its own amount"
        );

        Equal(0.25, PlayerProgression.LevelProgress(75, 100).Value, "fraction of the level");
        Equal(1d, PlayerProgression.LevelProgress(-5, 100).Value, "progress stops at 1");
        Equal(0d, PlayerProgression.LevelProgress(500, 100).Value, "progress stops at 0");
        True(PlayerProgression.LevelProgress(5, 0) == null, "no cost means no progress");
    }
}
