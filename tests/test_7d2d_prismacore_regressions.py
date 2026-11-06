"""Source invariants of the 7D2D PrismaCore-compatible command foundation (#304)."""

import re
import unittest
from pathlib import Path

REPOSITORY_ROOT = Path(__file__).resolve().parents[1]
GAME_ROOT = REPOSITORY_ROOT / "games" / "7d2d"
SOURCE_ROOT = GAME_ROOT / "mod" / "src"
COMMANDS = SOURCE_ROOT / "Commands"


class PrismaCoreFoundationTests(unittest.TestCase):
    def source(self, relative_path: str) -> str:
        return (SOURCE_ROOT / relative_path).read_text()

    def test_the_database_is_a_write_through_file(self):
        database = self.source("Persistence/Database.cs")
        files = self.source("Persistence/DatabaseFiles.cs")
        # Opening by filename hits Mono's buffered page write ("ReadFull must read PAGE_SIZE
        # bytes"); the streams are the fix, and :memory: is only the locked-file fallback.
        self.assertIn("new LiteDatabase(_data, null, _log)", files)
        self.assertIn("StorageLifecycle.ApplyAtBoot(Db)", files)
        self.assertNotIn("Filename=", database + files)
        self.assertEqual(database.count('new LiteDatabase(":memory:")'), 1)
        self.assertIn("catch (IOException", database)
        self.assertIn("DatabaseFiles.Open(", database)
        # A damaged log is set aside before the data file is.
        self.assertLess(files.index("set its log aside"), files.index("started a new one"))

    def test_commands_never_wait_for_the_database_lock(self):
        mirror = self.source("Services/StateMirror.cs")
        registry = mirror.split("public IReadOnlyList<PlayerRecord> GetRegisteredPlayers()", 1)[1].split("\n", 1)[0]
        self.assertNotIn("SyncRoot", registry)
        self.assertGreaterEqual(mirror.count("PublishRegistry();"), 2)

    def test_every_collection_has_a_lifecycle(self):
        database = self.source("Persistence/Database.cs")
        lifecycle = self.source("Persistence/StorageLifecycle.cs")
        used = set(re.findall(r"GetCollection<\w+>\(StorageLifecycle\.(\w+)\)", database))
        self.assertTrue(used)
        self.assertNotRegex(database, r'GetCollection<\w+>\("')
        for name in used:
            self.assertRegex(lifecycle, r"\{ " + name + r", CollectionLifecycle\.")

    def test_the_database_opens_before_anything_writes(self):
        api = self.source("API.cs")
        startup = api.split("private static void GameStartDone", 1)[1].split("private static void GameUpdate", 1)[0]
        self.assertLess(
            startup.index("Database.Instance.Open(saveDirectory)"),
            startup.index("StateMirror.Instance.SeedOnGameStart()"),
        )
        self.assertIn("DbWriter.Instance.Enqueue(() => Database.Instance.Open(saveDirectory))", startup)
        self.assertLess(
            startup.index("Database.Instance.Open(saveDirectory)"),
            startup.index("PositionSampler.Instance.Start()"),
        )
        sampler = self.source("Services/PositionSampler.cs")
        self.assertIn("if (!_started)\n                return;", sampler)

    def test_prismacore_conflict_is_checked_once_at_game_start(self):
        api = self.source("API.cs")
        self.assertEqual(api.count("PrismaCoreConflict.WarnIfLoaded()"), 1)
        conflict = self.source("Services/PrismaCoreConflict.cs")
        self.assertIn("if (_checked)", conflict)

    def test_commands_follow_the_conventions(self):
        families = [p for p in COMMANDS.glob("*.cs") if p.name not in ("Debug.cs", "PrismaCoreCommand.cs", "PlayerTarget.cs")]
        self.assertTrue(families)
        for path in [*families, *COMMANDS.glob("Contract/*.cs")]:
            text = path.read_text()
            for forbidden in ("new Thread", "Thread.Sleep", "Task.Delay", "Task.Run", "ThreadPool"):
                self.assertNotIn(forbidden, text, f"{path.name} must not start threads or sleep")
            self.assertNotRegex(text, r'"pc-', f"{path.name} uses plain PrismaCore names")
        for path in families:
            for match in re.finditer(r"internal (sealed |abstract )?class (\w+) : (\w+)", path.read_text()):
                self.assertIn(
                    match.group(3),
                    ("PrismaCoreCommand", "ProgressionCommand"),
                    f"{match.group(2)} in {path.name} derives from the shared base",
                )
        for path in COMMANDS.glob("Contract/*.cs"):
            text = path.read_text()
            self.assertNotRegex(text, r"\b(GameManager|SdtdConsole|ConsoleHelper|EntityPlayer)\b", f"{path.name} is game-free")

    def test_online_players_resolve_through_the_game(self):
        target = (COMMANDS / "PlayerTarget.cs").read_text()
        self.assertIn("ConsoleHelper.ParseParamPartialNameOrId(", target)
        self.assertIn("StateMirror.Instance.GetRegisteredPlayers", target)

    def test_new_sources_are_compiled_and_tested(self):
        project = (GAME_ROOT / "mod" / "Takaro.csproj").read_text(encoding="utf-8-sig")
        harness = (GAME_ROOT / "scripts" / "test-contract.sh").read_text()
        self.assertIn(r"src\Commands\**\*.cs", project)
        for name in ("StorageLifecycle", "WriteThroughFileStream", "DatabaseFiles"):
            self.assertIn(rf"src\Persistence\{name}.cs", project)
            self.assertIn(f"/app/mod/src/Persistence/{name}.cs", harness)
        self.assertIn(r"src\Services\PlayerIdResolver.cs", project)
        self.assertIn("/app/mod/src/Services/PlayerIdResolver.cs", harness)
        self.assertIn("/app/mod/src/Commands/Contract/*.cs", harness)
        self.assertIn("/app/tests/ContractHarness*.cs", harness)
        self.assertIn("/app/tests/StorageHarness.cs", harness)


if __name__ == "__main__":
    unittest.main()
