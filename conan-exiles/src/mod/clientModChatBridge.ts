import type { ConanPlayer } from '../conan/parsers.js';

export type ClientModRconExecutor = (command: string) => Promise<string>;
export type ConanPlayerLoader = () => Promise<ConanPlayer[]>;

export interface ConanClientModChatStatus {
  mode: 'client-mod-rcon-datacmd';
  configured: boolean;
  lastAttemptAt: string | null;
  lastDispatchAt: string | null;
  lastMessage: string | null;
  selectedTargets: string[];
  dispatchAccepted: boolean | null;
  deliveryVerified: false;
  verificationReason: string;
  lastError: string | null;
}

export interface ConanClientModChatResult {
  success: true;
  message: string;
  targetIds: string[];
  dispatchAccepted: true;
  deliveryVerified: false;
  verificationReason: string;
}

const ACCEPTED_REASON = 'RCON accepted the client DataCmd; client delivery is not acknowledged.';
const FAILED_REASON = 'RCON dispatch was not fully accepted.';

export function quoteConanEventArgument(value: string): string {
  if (/[\u0000-\u001f\u007f-\u009f\u2028\u2029;|]/u.test(value) || value.includes('&&')) {
    throw new Error('Unsafe Conan event argument: control characters and command separators are not allowed');
  }
  return `"${value.replace(/\\/g, '\\\\').replace(/"/g, '\\"')}"`;
}

/**
 * Conan's DataCmd parser splits quoted arguments on ASCII spaces before the
 * command Blueprint receives Parameters. Preserve argument boundaries with a
 * small reversible percent encoding; BP_TakaroChatCommand decodes %20 and %25
 * before it renders the message.
 */
export function encodeConanDataCommandArgument(value: string): string {
  return value.replace(/%/g, '%25').replace(/ /g, '%20');
}

export class ConanClientModChatBridge {
  private diagnostics: ConanClientModChatStatus;
  private nextAttemptId = 0;
  private latestAttemptId = 0;

  constructor(
    private readonly execute: ClientModRconExecutor,
    private readonly loadPlayers: ConanPlayerLoader,
    configured = true,
  ) {
    this.diagnostics = {
      mode: 'client-mod-rcon-datacmd',
      configured,
      lastAttemptAt: null,
      lastDispatchAt: null,
      lastMessage: null,
      selectedTargets: [],
      dispatchAccepted: null,
      deliveryVerified: false,
      verificationReason: 'No dispatch attempt has completed.',
      lastError: null,
    };
  }

  isConnected(): boolean {
    return this.diagnostics.configured;
  }

  status(): ConanClientModChatStatus {
    return { ...this.diagnostics, selectedTargets: [...this.diagnostics.selectedTargets] };
  }

  async sendMessage(
    message: string,
    recipientIdentifier: string | null,
    senderNameOverride: string | null,
  ): Promise<ConanClientModChatResult> {
    const attemptId = ++this.nextAttemptId;
    this.latestAttemptId = attemptId;
    let targetIds: string[] = [];

    try {
      if (!this.diagnostics.configured) throw new Error('Takaro Conan client-mod chat transport is disabled');
      const recipients = selectRecipients(await this.loadPlayers(), recipientIdentifier);
      targetIds = recipients.map(stablePlayerId);
      for (const player of recipients) validateRconPlayerIndex(player);

      const sender = quoteConanEventArgument(encodeConanDataCommandArgument(senderNameOverride ?? 'Takaro'));
      const encodedMessage = quoteConanEventArgument(encodeConanDataCommandArgument(message));
      for (const player of recipients) {
        const command = `con ${player.rconId} dc TakaroChat ${sender} ${encodedMessage}`;
        const rawResult = await this.execute(command);
        if (rawResult.trim() !== `Successfully executed: ${command}`) {
          throw new Error(`Conan RCON did not exactly accept the client DataCmd: ${rawResult}`);
        }
      }

      const completedAt = new Date().toISOString();
      this.publishDiagnostics(attemptId, {
        mode: 'client-mod-rcon-datacmd', configured: this.diagnostics.configured,
        lastAttemptAt: completedAt, lastDispatchAt: completedAt, lastMessage: message,
        selectedTargets: targetIds, dispatchAccepted: true, deliveryVerified: false,
        verificationReason: ACCEPTED_REASON, lastError: null,
      });
      return {
        success: true, message, targetIds, dispatchAccepted: true,
        deliveryVerified: false, verificationReason: ACCEPTED_REASON,
      };
    } catch (err) {
      this.publishDiagnostics(attemptId, {
        mode: 'client-mod-rcon-datacmd', configured: this.diagnostics.configured,
        lastAttemptAt: new Date().toISOString(), lastDispatchAt: this.diagnostics.lastDispatchAt,
        lastMessage: message, selectedTargets: targetIds, dispatchAccepted: false,
        deliveryVerified: false, verificationReason: FAILED_REASON, lastError: errorMessage(err),
      });
      throw err;
    }
  }

  private publishDiagnostics(attemptId: number, snapshot: ConanClientModChatStatus): void {
    if (attemptId === this.latestAttemptId) this.diagnostics = snapshot;
  }
}

function selectRecipients(players: ConanPlayer[], recipientIdentifier: string | null): ConanPlayer[] {
  if (!recipientIdentifier) {
    if (!players.length) throw new Error('No online Conan players are available for client-mod chat dispatch');
    return players;
  }
  const requested = normalizeIdentifier(recipientIdentifier);
  const matches = players.filter((player) =>
    playerAliases(player).some((alias) => normalizeIdentifier(alias) === requested));
  if (!matches.length) throw new Error(`No online Conan player matched ${recipientIdentifier}`);
  if (matches.length > 1) throw new Error(`Ambiguous Conan player identifier ${recipientIdentifier}`);
  return matches;
}

function validateRconPlayerIndex(player: ConanPlayer): void {
  if (!player.rconId) throw new Error(`Online Conan player ${stablePlayerId(player)} has no RCON player index`);
  if (!/^\d+$/.test(player.rconId)) {
    throw new Error(`Online Conan player ${stablePlayerId(player)} has an invalid RCON player index`);
  }
}

function playerAliases(player: ConanPlayer): string[] {
  return [player.gameId, player.steamId, player.platformId, player.characterName, player.name]
    .filter((value): value is string => Boolean(value));
}

function normalizeIdentifier(value: string): string {
  const normalized = value.trim().toLowerCase();
  const separator = normalized.indexOf(':');
  return separator >= 0 ? normalized.slice(separator + 1) : normalized;
}

function stablePlayerId(player: ConanPlayer): string {
  return player.steamId ?? player.platformId ?? player.gameId;
}

function errorMessage(err: unknown): string {
  return err instanceof Error ? err.message : String(err);
}
