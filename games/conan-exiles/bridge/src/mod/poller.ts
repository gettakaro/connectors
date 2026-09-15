import { spawn } from 'node:child_process';
import { sendRconCommand, type RconCommandOptions } from '../rcon/client.js';

export interface ModPollCommand {
  requestId: string;
  action: string;
  args: Record<string, unknown>;
}

export interface BridgePollResponse {
  hasCommand: boolean;
  command?: ModPollCommand;
}

export interface BridgeClient {
  poll(): Promise<BridgePollResponse>;
  complete(requestId: string, result: unknown): Promise<void>;
}

export interface RenderedMessage {
  requestId: string;
  message: string;
  recipient: string | null;
  senderNameOverride: string | null;
}

export interface CommandRenderer {
  sendMessage(message: RenderedMessage): Promise<unknown>;
}

export interface PollResult {
  handled: boolean;
  action?: string;
  requestId?: string;
}

export class ModCommandPoller {
  constructor(
    private readonly bridge: BridgeClient,
    private readonly renderer: CommandRenderer,
  ) {}

  async pollOnce(): Promise<PollResult> {
    const polled = await this.bridge.poll();
    if (!polled.hasCommand || !polled.command) return { handled: false };

    const { command } = polled;
    try {
      const result = await this.handle(command);
      await this.bridge.complete(command.requestId, result);
    } catch (err) {
      await this.bridge.complete(command.requestId, {
        success: false,
        error: errorMessage(err),
      });
    }

    return {
      handled: true,
      action: command.action,
      requestId: command.requestId,
    };
  }

  private async handle(command: ModPollCommand): Promise<unknown> {
    if (command.action !== 'sendMessage') {
      return {
        success: false,
        error: `Unsupported Conan helper command: ${command.action}`,
      };
    }

    const message = requiredString(command.args.message, 'message');
    const recipient = optionalString(command.args.recipient);
    const senderNameOverride = optionalString(command.args.senderNameOverride);
    return this.renderer.sendMessage({
      requestId: command.requestId,
      message,
      recipient,
      senderNameOverride,
    });
  }
}

export class HttpBridgeClient implements BridgeClient {
  constructor(private readonly baseUrl: string) {}

  async poll(): Promise<BridgePollResponse> {
    const response = await fetch(new URL('/mod/poll', this.baseUrl));
    if (!response.ok) throw new Error(`Bridge poll failed: HTTP ${response.status}`);
    return (await response.json()) as BridgePollResponse;
  }

  async complete(requestId: string, result: unknown): Promise<void> {
    const response = await fetch(new URL('/mod/result', this.baseUrl), {
      method: 'POST',
      headers: {
        'content-type': 'application/json',
      },
      body: JSON.stringify({ requestId, result }),
    });
    if (!response.ok) throw new Error(`Bridge result failed: HTTP ${response.status}`);
  }
}

export class ExternalCommandRenderer implements CommandRenderer {
  constructor(private readonly command: string) {}

  async sendMessage(message: RenderedMessage): Promise<unknown> {
    const result = await runRendererCommand(this.command, message);
    return {
      success: result.exitCode === 0,
      sent: result.exitCode === 0,
      ...(result.exitCode === 0 ? {} : { error: result.stderr || `Renderer exited with ${result.exitCode}` }),
      ...(result.stdout ? { stdout: result.stdout } : {}),
    };
  }
}

export type ConanChatMod = 'pippi' | 'amunet';

export interface RconChatModRendererOptions {
  rcon: Omit<RconCommandOptions, 'command'>;
  mod: ConanChatMod;
  senderName?: string;
  execute?: (command: string) => Promise<string>;
}

export class RconChatModRenderer implements CommandRenderer {
  private readonly senderName: string;
  private readonly execute: (command: string) => Promise<string>;

  constructor(private readonly options: RconChatModRendererOptions) {
    this.senderName = cleanChatField(options.senderName || 'Takaro');
    this.execute = options.execute || ((command) => sendRconCommand({ ...options.rcon, command }));
  }

  async sendMessage(message: RenderedMessage): Promise<unknown> {
    if (this.options.mod === 'pippi') return this.sendPippiMessage(message);

    const command = this.renderCommand(message.message);
    const response = await this.execute(command);
    return {
      success: true,
      sent: true,
      mod: this.options.mod,
      command,
      ...(response ? { response } : {}),
    };
  }

  private renderCommand(message: string): string {
    const cleanMessage = cleanChatField(message);

    switch (this.options.mod) {
      case 'pippi':
        throw new Error('Pippi messages are rendered with server/directmessage commands');
      case 'amunet':
        return `ast chat "global" ${this.senderName}:${cleanMessage}`;
    }
  }

  private async sendPippiMessage(message: RenderedMessage): Promise<unknown> {
    const cleanMessage = cleanChatField(message.message);
    if (!message.recipient) {
      const senderName = cleanChatField(message.senderNameOverride || this.senderName);
      const commandMessage = senderName ? `${senderName}: ${cleanMessage}` : cleanMessage;
      const command = `server ${commandMessage}`;
      const response = await this.execute(command);
      return {
        success: true,
        sent: true,
        mod: this.options.mod,
        command,
        ...(response ? { response } : {}),
      };
    }

    const players = parsePippiListPlayers(await this.execute('listplayers'));
    const targets = findPippiTargets(players, message.recipient);

    if (targets.length === 0) {
      return {
        success: false,
        sent: false,
        mod: this.options.mod,
        command: null,
        error: `No online Conan player matched recipient ${message.recipient}`,
      };
    }

    const commands = targets.map(
      (target) => `directmessage ${quotePippiArgument(this.senderName)} ${quotePippiArgument(target)} ${cleanMessage}`,
    );
    const responses = [];
    const failures = [];
    for (const command of commands) {
      const response = await this.execute(command);
      responses.push(response);
      if (/could not find|usage:/i.test(response)) failures.push({ command, response });
    }

    return {
      success: failures.length === 0,
      sent: failures.length === 0,
      mod: this.options.mod,
      commands,
      responses,
      ...(failures.length ? { failures } : {}),
    };
  }
}

interface PippiPlayerTarget {
  characterName: string;
  playerName: string;
  userId: string;
  platformId: string;
}

function parsePippiListPlayers(raw: string): PippiPlayerTarget[] {
  return raw
    .split(/\r?\n/)
    .map((line) => line.trim())
    .map((line) => {
      const parts = line.split('|').map((part) => part.trim());
      if (parts.length < 6 || !/^\d+$/.test(parts[0]!)) return null;
      const platformName = parts[5]!.toLowerCase();
      const rawPlatformId = parts[4]!;
      return {
        characterName: parts[1]!,
        playerName: parts[2]!,
        userId: parts[3]!,
        platformId: platformName && rawPlatformId ? `${platformName}:${rawPlatformId}` : rawPlatformId,
      };
    })
    .filter((player): player is PippiPlayerTarget => Boolean(player?.characterName));
}

function findPippiTargets(players: PippiPlayerTarget[], recipient: string): string[] {
  const normalized = normalizeRecipient(recipient);
  return players
    .filter((player) =>
      [
        player.characterName,
        player.playerName,
        player.userId,
        player.platformId,
        stripPlatformPrefix(player.platformId),
      ]
        .filter(Boolean)
        .some((value) => normalizeRecipient(value) === normalized),
    )
    .map((player) => player.characterName);
}

function normalizeRecipient(value: string): string {
  return stripPlatformPrefix(value).toLowerCase();
}

function stripPlatformPrefix(value: string): string {
  const parts = value.split(':');
  return parts.length > 1 ? parts.slice(1).join(':') : value;
}

function quotePippiArgument(value: string): string {
  return /\s/.test(value) ? `"${value.replace(/\\/g, '\\\\').replace(/"/g, '\\"')}"` : value;
}

interface RendererCommandResult {
  exitCode: number;
  stdout: string;
  stderr: string;
}

function runRendererCommand(command: string, message: RenderedMessage): Promise<RendererCommandResult> {
  return new Promise((resolve, reject) => {
    const child = spawn(command, {
      shell: true,
      env: {
        ...process.env,
        TAKARO_CONAN_REQUEST_ID: message.requestId,
        TAKARO_CONAN_MESSAGE: message.message,
        TAKARO_CONAN_RECIPIENT: message.recipient ?? '',
      },
      stdio: ['pipe', 'pipe', 'pipe'],
    });

    let stdout = '';
    let stderr = '';
    child.stdout.setEncoding('utf8');
    child.stderr.setEncoding('utf8');
    child.stdout.on('data', (chunk) => {
      stdout += chunk;
    });
    child.stderr.on('data', (chunk) => {
      stderr += chunk;
    });
    child.on('error', reject);
    child.on('close', (exitCode) => {
      resolve({
        exitCode: exitCode ?? 1,
        stdout: stdout.trim(),
        stderr: stderr.trim(),
      });
    });
    child.stdin.end(`${JSON.stringify(message)}\n`);
  });
}

function requiredString(value: unknown, name: string): string {
  const parsed = optionalString(value);
  if (!parsed) throw new Error(`Missing required command arg: ${name}`);
  return parsed;
}

function optionalString(value: unknown): string | null {
  if (typeof value === 'string' && value.trim()) return value.trim();
  if (typeof value === 'number') return String(value);
  return null;
}

function errorMessage(err: unknown): string {
  return err instanceof Error ? err.message : String(err);
}

function cleanChatField(value: string): string {
  return value.replace(/[\r\n|]/g, ' ').replace(/\s+/g, ' ').trim();
}
