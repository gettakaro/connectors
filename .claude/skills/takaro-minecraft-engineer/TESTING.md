# Testing

## Framework

JUnit Jupiter 5.11.4 with JUnit Platform Launcher. Tests exist only in the `core/` module — platform modules have no tests.

## Commands

### Run All Tests

```bash
cd minecraft && ./gradlew :core:test
```

25 tests, runs in ~2 seconds.

### Run Specific Test Class

```bash
cd minecraft && ./gradlew :core:test --tests "io.takaro.minecraft.core.TakaroConfigTest"
```

### Run Specific Test Method

```bash
cd minecraft && ./gradlew :core:test --tests "io.takaro.minecraft.core.TakaroConfigTest.defaultValuesAreSet"
```

## Test Files

All in `minecraft/core/src/test/java/io/takaro/minecraft/core/`:

| File | What it tests |
|------|---------------|
| `TakaroConfigTest.java` | Config defaults, getters/setters, env var overrides |
| `TakaroConnectorTest.java` | WebSocket connector initialization |
| `ActionRoutingTest.java` | Action message routing and dispatch |

## Test Reports

HTML report: `minecraft/core/build/reports/tests/test/index.html`

## Notes

- No setup required (no Docker services, no env vars needed)
- Platform modules (paper, neoforge, fabric) have no tests — testing happens via Docker dev servers and the test bot

## Integration Tests

Unit tests verify internal logic. Integration tests verify the full protocol flow from game server through WebSocket to the Takaro backend. They use:

- **Mineflayer test bot** — triggers in-game events (chat, death, kills)
- **Takaro MCP tools** — verifies events arrive in Takaro and actions execute correctly
- **RCON** — triggers server-side events (kill player, summon mobs)

See [INTEGRATION-TESTING.md](INTEGRATION-TESTING.md) for the full workflow, event verification patterns, and the code-change-to-test mapping table.
