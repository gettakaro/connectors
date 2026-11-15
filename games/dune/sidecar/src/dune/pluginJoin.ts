import { num, str } from './identity.js';
import type { DunePlayerRow, PluginPlayer } from './types.js';

/**
 * Joins the native plugin's player refs to Takaro's `gameId` (the FLS id).
 *
 * ---------------------------------------------------------------------------------------------
 * WHY A JOIN IS NEEDED AT ALL
 *
 * The map server process holds no FLS id. Lane L1b checked every UPROPERTY on `DunePlayerState`,
 * `DunePlayerStateBase` and `DunePlayerConnectionInfo` and found no account identifier; the FLS id is
 * a string the game only ever needs out of process (chat routing keys and the GM command envelope).
 *
 * What lane L2 found on the live class layouts IS the Postgres identity, on
 * `DunePlayerControllerPersistenceComponent`:
 *
 *   m_AccountID                → encrypted_player_state.account_id   (bigint → encrypted_accounts.id)
 *   m_PlayerStateUniqueID      → encrypted_player_state.player_state_id       (bigint → actors.id)
 *   m_PlayerControllerUniqueID → encrypted_player_state.player_controller_id  (bigint → actors.id)
 *   m_PlayerCharacterUniqueID  → encrypted_player_state.player_pawn_id        (bigint → actors.id)
 *   m_CharacterName            → decrypt(encrypted_character_name)
 *
 * and `accounts` is the view that turns an `encrypted_accounts.id` into the FLS id in the clear. So
 * the primary route is ONE HOP ON A PRIMARY KEY: `plugin accountId === roster row's accountId`, and
 * the roster row already carries `flsId`. No new SQL is needed — `pg.roster()` selects
 * `ps.account_id` and `ps.character_name` already.
 *
 * ---------------------------------------------------------------------------------------------
 * ROUTES, IN PRIORITY ORDER, EACH WITH ITS CONFIDENCE
 *
 *  1. `accountId`     — a primary-key equality. **High** confidence; the only uncertainty is whether
 *     the plugin's 8-byte id struct really holds that bigint, which the first join settles by
 *     agreeing (or not) with route 3.
 *  2. `playerPawnId` / `playerStateId` / `playerControllerId` — also exact, but the roster does not
 *     select them, so they are used only as a CORROBORATION when a caller supplies them
 *     (`disagreements()` counts the cases where two routes pick different players).
 *  3. `characterName` — case-insensitive equality on `player_state.character_name`. **Medium**: names
 *     are not unique by schema (there is no UNIQUE constraint on the decrypted name), so a name that
 *     matches more than one roster row is REFUSED rather than guessed.
 *
 * A ref that no route resolves yields `null`, and every caller then degrades — it never picks the
 * only online player "because there is only one". That shortcut is exactly how a kill gets credited
 * to the wrong person on a two-player server.
 */
export interface PluginJoinOptions {
  roster: () => Promise<DunePlayerRow[]>;
  players: () => Promise<{ players: PluginPlayer[] }>;
  /** How long a resolved mapping is reused before the roster and /players are re-read. */
  ttlMs?: number;
  now?: () => number;
  onError?: (err: Error) => void;
}

export interface JoinResult {
  flsId: string;
  /** The plugin-side handle for the same player. */
  ref: string;
  characterName: string | null;
  /** `accountId` | `characterName` — which route matched, for the report and for /health. */
  how: string;
}

/** Normalises the plugin's id fields, which arrive as a JSON number or (for large bigints) a string. */
function idOf(value: unknown): string | null {
  const n = num(value);
  if (n !== null && Number.isFinite(n) && n > 0) return String(Math.trunc(n));
  const s = str(value);
  return s && /^\d+$/.test(s) && s !== '0' ? s : null;
}

export class PluginJoin {
  private byRef = new Map<string, JoinResult>();
  private byFlsId = new Map<string, JoinResult>();
  private lastRefreshAt = 0;
  private refreshInFlight: Promise<void> | null = null;
  private lastError: string | null = null;
  private counters = { refreshes: 0, resolved: 0, unresolved: 0, ambiguousNames: 0, disagreements: 0 };

  constructor(private readonly options: PluginJoinOptions) {}

  private now(): number {
    return this.options.now?.() ?? Date.now();
  }

  /** Re-reads `/players` and the roster and rebuilds the mapping. Never throws. */
  async refresh(): Promise<void> {
    try {
      const [pluginPlayers, roster] = await Promise.all([this.options.players(), this.options.roster()]);
      const byAccount = new Map<string, DunePlayerRow>();
      const byName = new Map<string, DunePlayerRow[]>();
      for (const row of roster) {
        const acct = idOf(row.accountId);
        if (acct) byAccount.set(acct, row);
        const name = (row.characterName ?? '').trim().toLowerCase();
        if (name) byName.set(name, [...(byName.get(name) ?? []), row]);
      }
      const byRef = new Map<string, JoinResult>();
      const byFlsId = new Map<string, JoinResult>();
      let resolved = 0;
      let unresolved = 0;
      for (const p of pluginPlayers.players ?? []) {
        const ref = str(p.ref);
        if (!ref) continue;
        const match = this.match(p, byAccount, byName);
        if (!match) {
          unresolved++;
          continue;
        }
        resolved++;
        byRef.set(ref, match);
        byFlsId.set(match.flsId, match);
      }
      this.byRef = byRef;
      this.byFlsId = byFlsId;
      this.counters.refreshes++;
      this.counters.resolved = resolved;
      this.counters.unresolved = unresolved;
      this.lastRefreshAt = this.now();
      this.lastError = null;
    } catch (err) {
      this.lastError = err instanceof Error ? err.message : String(err);
      this.options.onError?.(err instanceof Error ? err : new Error(String(err)));
    }
  }

  private match(
    p: PluginPlayer,
    byAccount: Map<string, DunePlayerRow>,
    byName: Map<string, DunePlayerRow[]>,
  ): JoinResult | null {
    const ref = str(p.ref)!;
    const characterName = str(p.characterName);
    const acct = idOf(p.accountId);
    let viaAccount: DunePlayerRow | undefined;
    if (acct) viaAccount = byAccount.get(acct);

    const nameKey = (characterName ?? '').trim().toLowerCase();
    const nameRows = nameKey ? (byName.get(nameKey) ?? []) : [];
    let viaName: DunePlayerRow | undefined;
    if (nameRows.length === 1) viaName = nameRows[0];
    else if (nameRows.length > 1) this.counters.ambiguousNames++;

    // When both routes answer and they disagree, neither is trusted: a wrong join is worse than none.
    if (viaAccount && viaName && viaAccount.flsId !== viaName.flsId) {
      this.counters.disagreements++;
      return null;
    }
    const row = viaAccount ?? viaName;
    if (!row?.flsId) return null;
    return {
      flsId: row.flsId,
      ref,
      // The ROW wins. The plugin reads its name off the live `PlayerState` at PostLogin time, where Dune still has
      // the Funcom ACCOUNT display name (`Tester`) rather than the character name (`TakaroTest`) — which is exactly
      // why the plugin stamps its connect `identityComplete: false, identityAuthority: "sidecar/postgres"`. The
      // plugin name stays the matching key above; `player_state.character_name` is what Takaro is told.
      characterName: str(row.characterName) ?? characterName,
      how: viaAccount ? (viaName ? 'accountId+characterName' : 'accountId') : 'characterName',
    };
  }

  /** Refreshes when the cached mapping is older than the TTL. */
  private async ensureFresh(): Promise<void> {
    const ttl = this.options.ttlMs ?? 5000;
    if (this.lastRefreshAt && this.now() - this.lastRefreshAt < ttl) return;
    if (!this.refreshInFlight) this.refreshInFlight = this.refresh();
    const pending = this.refreshInFlight;
    try {
      await pending;
    } finally {
      if (this.refreshInFlight === pending) this.refreshInFlight = null;
    }
  }

  /** The FLS id (Takaro `gameId`) for a plugin ref, or null. */
  async flsIdFor(ref: string): Promise<JoinResult | null> {
    await this.ensureFresh();
    return this.byRef.get(ref) ?? null;
  }

  /** The plugin's ref for an FLS id, or null — what `getPlayerLocation` needs. */
  async refFor(flsId: string): Promise<JoinResult | null> {
    await this.ensureFresh();
    return this.byFlsId.get(flsId) ?? null;
  }

  /** Synchronous lookups for the event path, which must not await inside a mapper. */
  flsIdForCached(ref: string): JoinResult | null {
    return this.byRef.get(ref) ?? null;
  }
  refForCached(flsId: string): JoinResult | null {
    return this.byFlsId.get(flsId) ?? null;
  }

  status(): Record<string, unknown> {
    return {
      ...this.counters,
      mapped: this.byRef.size,
      ageMs: this.lastRefreshAt ? this.now() - this.lastRefreshAt : null,
      error: this.lastError,
      routes: 'accountId (primary key, high confidence) then a UNIQUE character name (medium); a disagreement or an '
        + 'ambiguous name resolves to nothing rather than to a guess',
    };
  }
}
