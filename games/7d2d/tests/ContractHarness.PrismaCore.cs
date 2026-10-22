using System;
using System.Collections.Generic;
using System.Linq;
using Takaro.Commands.Contract;
using Takaro.Persistence;
using Takaro.Services;

// PrismaCore-compatible command foundation: the shared player-id resolver, the progression
// and blood moon output contracts, and the PrismaCore conflict detection.
public static partial class ContractHarness
{
    private static void FamilyPrismaCoreResolver()
    {
        Equal(
            PlayerIdResolver.IdForm.EntityId,
            PlayerIdResolver.Classify("171"),
            "an int is an entity id"
        );
        Equal(
            PlayerIdResolver.IdForm.EntityId,
            PlayerIdResolver.Classify("-1"),
            "a negative int is an entity id"
        );
        foreach (
            string id in new[]
            {
                "EOS_0002abc",
                "Steam_7656",
                "XBL_2533",
                "PSN_9876",
                "eos_0002abc",
            }
        )
            Equal(
                PlayerIdResolver.IdForm.PlatformId,
                PlayerIdResolver.Classify(id),
                id + " is a platform id"
            );
        Equal(
            PlayerIdResolver.IdForm.Name,
            PlayerIdResolver.Classify("Limon"),
            "anything else is a name"
        );
        Equal(
            PlayerIdResolver.IdForm.Name,
            PlayerIdResolver.Classify("Steam_"),
            "a bare prefix is a name"
        );
        Equal(
            PlayerIdResolver.IdForm.Name,
            PlayerIdResolver.Classify("EOSfan"),
            "a prefix needs its underscore"
        );

        var steam = new PlayerRecord
        {
            GameId = "0002aaa",
            EpicOnlineServicesId = "0002aaa",
            Name = "Limon",
            SteamId = "76561198000735875",
            PlatformId = "Steam_76561198000735875",
            EntityId = 171,
            Online = false,
        };
        var xbox = new PlayerRecord
        {
            GameId = "0002bbb",
            Name = "Xander",
            XboxLiveId = "2533274800000000",
            PlatformId = "XBL_2533274800000000",
            EntityId = 172,
            Online = true,
        };
        var psn = new PlayerRecord
        {
            GameId = "0002ccc",
            Name = "Pia",
            PlatformId = "PSN_8888777766665555",
            EntityId = 173,
        };
        var twinA = new PlayerRecord
        {
            GameId = "0002ddd",
            Name = "Twin",
            EntityId = 174,
        };
        var twinB = new PlayerRecord
        {
            GameId = "0002eee",
            Name = "twin",
            EntityId = 175,
        };
        var registry = new List<PlayerRecord> { steam, xbox, psn, twinA, twinB };

        AssertResolves(registry, "171", steam, "entity id");
        AssertResolves(registry, "Limon", steam, "exact name");
        AssertResolves(registry, "limon", steam, "names match case-insensitively");
        AssertResolves(registry, "EOS_0002aaa", steam, "EOS id is the game id");
        AssertResolves(registry, "EOS_0002AAA", steam, "EOS ids are case-insensitive hex");
        AssertResolves(registry, "Steam_76561198000735875", steam, "Steam id");
        AssertResolves(registry, "XBL_2533274800000000", xbox, "Xbox id");
        AssertResolves(registry, "PSN_8888777766665555", psn, "PSN id via the native platform id");
        AssertResolves(registry, "172", xbox, "an online registered player resolves too");

        AssertUnresolved(registry, "999", PlayerIdResolver.Outcome.NotFound, "unknown entity id");
        AssertUnresolved(registry, "Nobody", PlayerIdResolver.Outcome.NotFound, "unknown name");
        AssertUnresolved(
            registry,
            "Lim",
            PlayerIdResolver.Outcome.NotFound,
            "registry names are exact, never partial"
        );
        AssertUnresolved(registry, "EOS_ffff", PlayerIdResolver.Outcome.NotFound, "unknown EOS id");
        AssertUnresolved(
            registry,
            "Steam_1",
            PlayerIdResolver.Outcome.NotFound,
            "unknown Steam id"
        );
        AssertUnresolved(registry, "PSN_1", PlayerIdResolver.Outcome.NotFound, "unknown PSN id");
        AssertUnresolved(
            registry,
            "Twin",
            PlayerIdResolver.Outcome.Ambiguous,
            "a name two players share is ambiguous"
        );
        AssertUnresolved(registry, "", PlayerIdResolver.Outcome.NotFound, "an empty id");
        AssertUnresolved(
            new List<PlayerRecord>(),
            "Limon",
            PlayerIdResolver.Outcome.NotFound,
            "an empty registry"
        );

        AssertOnlineThenRegistry(registry);

        Equal(
            "\"Nobody\" is not a valid entity id, player name or user id.",
            PlayerIdResolver.NotFoundLine("Nobody"),
            "the not-found line is the game's own, verbatim"
        );
        Equal(
            "\"Twin\" matches multiple player names.",
            PlayerIdResolver.AmbiguousLine("Twin"),
            "the ambiguous line is the game's own, verbatim"
        );
    }

    private sealed class FakeClient
    {
        public string Name;
        public int EntityId;
        public string PlatformId;
        public string CrossplatformId;
    }

    // Stands in for ConsoleHelper.ParseParamPartialNameOrId: exact entity id, name, user id,
    // then a unique case-insensitive partial name; the count of partial matches otherwise.
    private static PlayerIdResolver.OnlineLookup<FakeClient> GameLookup(List<FakeClient> online)
    {
        return (string id, out FakeClient client) =>
        {
            client = online.FirstOrDefault(c =>
                c.EntityId.ToString() == id
                || string.Equals(c.Name, id, StringComparison.OrdinalIgnoreCase)
                || c.PlatformId == id
                || c.CrossplatformId == id
            );
            if (client != null)
                return 1;
            List<FakeClient> partial = online
                .Where(c => c.Name.IndexOf(id, StringComparison.OrdinalIgnoreCase) >= 0)
                .ToList();
            if (partial.Count == 1)
                client = partial[0];
            return partial.Count;
        };
    }

    private static void AssertOnlineThenRegistry(List<PlayerRecord> registry)
    {
        var limon = new FakeClient
        {
            Name = "Limon",
            EntityId = 171,
            PlatformId = "Steam_76561198000735875",
            CrossplatformId = "EOS_0002aaa",
        };
        var pia = new FakeClient
        {
            Name = "Pia",
            EntityId = 180,
            PlatformId = "PSN_8888777766665555",
            CrossplatformId = "EOS_0002ccc",
        };
        var lima = new FakeClient
        {
            Name = "Lima",
            EntityId = 181,
            CrossplatformId = "EOS_0002fff",
        };
        PlayerIdResolver.OnlineLookup<FakeClient> game = GameLookup(
            new List<FakeClient> { limon, pia }
        );
        int registryReads = 0;
        Func<IEnumerable<PlayerRecord>> readRegistry = () =>
        {
            registryReads++;
            return registry;
        };

        foreach (
            string id in new[] { "171", "Limon", "Steam_76561198000735875", "EOS_0002aaa", "Lim" }
        )
        {
            PlayerIdResolver.Resolution<FakeClient> online = PlayerIdResolver.Resolve(
                id,
                game,
                readRegistry
            );
            True(online.Found && ReferenceEquals(limon, online.Online), "online by " + id);
            True(online.Registered == null, "an online match is not a registry match (" + id + ")");
        }
        PlayerIdResolver.Resolution<FakeClient> psnOnline = PlayerIdResolver.Resolve(
            "PSN_8888777766665555",
            game,
            readRegistry
        );
        True(ReferenceEquals(pia, psnOnline.Online), "online by PSN id");
        Equal(0, registryReads, "online players never touch the registry");

        PlayerIdResolver.Resolution<FakeClient> ambiguousOnline = PlayerIdResolver.Resolve(
            "Lim",
            GameLookup(new List<FakeClient> { limon, lima }),
            readRegistry
        );
        Equal(
            "\"Lim\" matches multiple player names.",
            ambiguousOnline.FailureLine,
            "an ambiguous online partial name"
        );

        PlayerIdResolver.OnlineLookup<FakeClient> nobodyOnline = GameLookup(new List<FakeClient>());
        foreach (string id in new[] { "171", "Limon", "Steam_76561198000735875", "EOS_0002aaa" })
        {
            PlayerIdResolver.Resolution<FakeClient> offline = PlayerIdResolver.Resolve(
                id,
                nobodyOnline,
                readRegistry
            );
            True(offline.Found && offline.Online == null, "offline by " + id);
            Equal(
                "0002aaa",
                offline.Registered.GameId,
                "offline " + id + " is the registered player"
            );
        }
        Equal(
            "\"Twin\" matches multiple player names.",
            PlayerIdResolver.Resolve("Twin", nobodyOnline, readRegistry).FailureLine,
            "an ambiguous offline name"
        );
        Equal(
            "\"Ghost\" is not a valid entity id, player name or user id.",
            PlayerIdResolver.Resolve("Ghost", nobodyOnline, readRegistry).FailureLine,
            "neither online nor registered"
        );
        Equal(
            "\"EOS_dead\" is not a valid entity id, player name or user id.",
            PlayerIdResolver
                .Resolve(
                    "EOS_dead",
                    (string _, out FakeClient c) =>
                    {
                        c = null;
                        return 1;
                    },
                    readRegistry
                )
                .FailureLine,
            "a well-formed user id the game accepts but nobody owns"
        );
    }

    private static void AssertResolves(
        List<PlayerRecord> registry,
        string id,
        PlayerRecord expected,
        string form
    )
    {
        PlayerIdResolver.Outcome outcome = PlayerIdResolver.ResolveRegistered(
            id,
            registry,
            out PlayerRecord match
        );
        Equal(PlayerIdResolver.Outcome.Found, outcome, "resolves by " + form + " (" + id + ")");
        True(ReferenceEquals(expected, match), "resolves " + id + " to " + expected.Name);
    }

    private static void AssertUnresolved(
        List<PlayerRecord> registry,
        string id,
        PlayerIdResolver.Outcome expected,
        string description
    )
    {
        PlayerIdResolver.Outcome outcome = PlayerIdResolver.ResolveRegistered(
            id,
            registry,
            out PlayerRecord match
        );
        Equal(expected, outcome, description);
        True(match == null, description + " returns no player");
    }

    private static void FamilyPrismaCoreProgressionContract()
    {
        ProgressionContract.Spec rl = ProgressionContract.ResetLevel;
        ProgressionContract.Spec rs = ProgressionContract.ResetSkillPoints;
        True(
            rl.Names.SequenceEqual(new[] { "resetlevel", "rl" }),
            "resetlevel keeps PrismaCore's names"
        );
        True(
            rs.Names.SequenceEqual(new[] { "resetskillpoints", "rs" }),
            "resetskillpoints keeps PrismaCore's names"
        );
        True(
            rl.Names.Concat(rs.Names)
                .Concat(BloodMoonContract.Names)
                .All(name => !name.StartsWith("pc-")),
            "no command carries a pc- prefix"
        );

        AssertProgressionError(
            rl,
            new string[0],
            "ERR: Wrong number of arguments, expected 2, found 0"
        );
        AssertProgressionError(
            rl,
            new[] { "Limon" },
            "ERR: Wrong number of arguments, expected 2, found 1"
        );
        AssertProgressionError(
            rs,
            new[] { "a", "1", "2" },
            "ERR: Wrong number of arguments, expected 2, found 3"
        );
        AssertProgressionError(
            rl,
            new[] { "Limon", "abc" },
            "ERR: Invalid level parameter abc. Must be a valid integer"
        );
        AssertProgressionError(
            rl,
            new[] { "Limon", "1.5" },
            "ERR: Invalid level parameter 1.5. Must be a valid integer"
        );
        AssertProgressionError(
            rl,
            new[] { "Limon", "99999999999" },
            "ERR: Invalid level parameter 99999999999. Must be a valid integer"
        );
        AssertProgressionError(
            rs,
            new[] { "Limon", "x" },
            "ERR: Invalid count parameter x. Must be a valid integer"
        );
        AssertProgressionError(rl, new[] { "Limon", "0" }, "ERR: Level must be 1 or greater.");
        AssertProgressionError(rl, new[] { "Limon", "-3" }, "ERR: Level must be 1 or greater.");
        AssertProgressionError(rs, new[] { "Limon", "-1" }, "ERR: Count must be 0 or greater.");

        Equal(
            null,
            ProgressionContract.Validate(rl, new[] { "Limon", "1" }, out string id, out int value),
            "level 1 is valid"
        );
        Equal("Limon", id, "the player argument is passed on");
        Equal(1, value, "the level is parsed");
        Equal(
            null,
            ProgressionContract.Validate(rs, new[] { "EOS_0002aaa", "0" }, out id, out value),
            "count 0 is valid"
        );
        Equal(0, value, "the count is parsed");

        Equal(
            "ERR: Player is offline. Player MUST be online for level reset.",
            rl.OfflineLine,
            "rl offline line"
        );
        Equal(
            "ERR: Player is offline. Player MUST be online for skillpoints reset.",
            rs.OfflineLine,
            "rs offline line"
        );
        Equal(
            "You have reset the level of this player to level 25.",
            rl.SuccessLine(25),
            "rl success line"
        );
        Equal(
            "You have reset the skillpoints for this player to 0.",
            rs.SuccessLine(0),
            "rs success line"
        );

        Equal("true", BloodMoonContract.Line(true), "isbloodmoon prints true");
        Equal("false", BloodMoonContract.Line(false), "isbloodmoon prints false");
        True(
            BloodMoonContract.Names.SequenceEqual(new[] { "isbloodmoon" }),
            "isbloodmoon keeps its name"
        );
    }

    private static void AssertProgressionError(
        ProgressionContract.Spec spec,
        string[] args,
        string expected
    )
    {
        string error = ProgressionContract.Validate(spec, args, out string id, out int _);
        Equal(expected, error, spec.Names[1] + " " + string.Join(" ", args));
        True(id == null, "a rejected command resolves no player");
    }

    private static void FamilyPrismaCoreDetection()
    {
        True(PrismaCoreDetection.IsPrismaCore("PrismaCore", "PrismaCore", null), "the mod by name");
        True(
            PrismaCoreDetection.IsPrismaCore("Other", "prismacore-2.1", null),
            "the mod by folder"
        );
        True(
            PrismaCoreDetection.IsPrismaCore(
                "Server tools",
                "tools",
                new[] { "LiteDB", "PrismaCore" }
            ),
            "the mod by assembly"
        );
        True(
            !PrismaCoreDetection.IsPrismaCore("Takaro", "Takaro", new[] { "Takaro", "LiteDB" }),
            "not this connector"
        );
        True(
            !PrismaCoreDetection.IsPrismaCore("TFP_Harmony", "0_TFP_Harmony", new[] { "0Harmony" }),
            "not the game's Harmony"
        );
        True(!PrismaCoreDetection.IsPrismaCore(null, null, null), "a mod without names");
        True(
            PrismaCoreDetection.Warning.Contains("unsupported"),
            "the warning says running both is unsupported"
        );
        True(PrismaCoreDetection.Warning.Contains("LiteDB"), "the warning names the LiteDB clash");
    }
}
