# ARK ban metadata and expiry

ARK's native ban collection stores Steam64 IDs, not reasons or expiry dates. The
sidecar keeps those fields in `ban-metadata.json` beside `TAKARO_CURSOR_FILE` (or
at `TAKARO_BAN_METADATA_FILE`). Keep that directory on the same persistent
sidecar data mount across restarts. The file is written atomically before a ban
is sent to native ARK. A corrupt existing file fails startup; a missing file
with native bans leaves ban reconciliation unavailable until migration.

`listBans` reads fresh native membership and joins it to the saved metadata.
If native contains an ID without metadata, `listBans` fails closed because
Takaro's ban sync would otherwise overwrite a managed ban's reason and expiry
with blank/permanent values. To migrate a preexisting native ban, obtain its
actual reason and expiry from the authoritative Takaro ban record or operator,
then reissue an authorized Generic `banPlayer` action for that Steam64 with
those values. Verify the new `listBans` result; do not guess an unknown expiry.

The sidecar checks timed bans independently of its Takaro WebSocket connection.
At expiry it verifies native unban; if the sidecar or game is unavailable, the
ban can remain enforced until reconciliation succeeds after recovery. Takaro's
current ban sync has no expiry-deletion job. An expired managed database row can
remain visible, and a stale global sync request is rejected before it can
re-ban the player. Remove stale managed rows through Takaro's ban controls.
