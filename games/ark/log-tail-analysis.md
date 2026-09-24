# Narrow ARK game-log event source

`log_tail.hpp` reads the game's own `ShooterGame/Saved/Logs/ShooterGame.log`
from a native worker. It primes at EOF after connector startup so historical
file contents are not sent as fresh events. Each poll reads at most 64 KiB,
retains at most 512 bytes per line, and returns at most 32 allowed messages.
It follows file rotation/truncation by inode and reports `more` and `dropped`
separately. The native integration must expose/drop-count diagnostic evidence
when `dropped` is true; a quiet empty event batch is not complete delivery.

The allowlist intentionally covers only fixed, low-risk engine diagnostics:
`Saving world...`, `World Save Complete. Took <number>`, `ARK Version:
<number>`, `Primal Game Data Took <number> seconds`, and `New Save Format
enabled`. It rejects arbitrary game/console lines, command-line arguments,
credential terms, RCON fields and address-bearing lines. This is a narrow
log capability, not general console output. The raw source file may contain
credentials and must not be forwarded or copied wholesale into reports.

A replaced log file may already contain lines by the next poll. Those lines
belong to the new file but are not guaranteed to be after the Takaro test's
T0; acceptance must check fresh timestamp, boot identity and a real log-hook
execution. The ring/WebSocket cursor remains at-least-once transport, not an
exactly-once application guarantee. No live Takaro log-hook execution has been
proved for this helper yet.

The exact build has a `FORCELOGFLUSH` command-line flag. Its UTF-32 literal
is at `0x42C2FC8`; file-output serialization parses it at
`0x1B1C6D5–0x1B1C6E7`. When enabled, the same function calls its writer's
virtual flush slot `+0xC0` at `0x1B1C6F1–0x1B1C706` after writing. Read-only
inspection of the controlled server found `-log` present, `-NoLog` and
`-ForceLogFlush` absent. The running process had FD 42 open to
`/server/ShooterGame/Saved/Logs/ShooterGame.log`, but that file's size and
offset were zero. Enabling the exact flag in a later controlled launch is
a candidate for making file-tail events observable, not a live result. No
engine startup or save line was found in its Docker stdout either; connector
diagnostic stdout is not an engine log event.
