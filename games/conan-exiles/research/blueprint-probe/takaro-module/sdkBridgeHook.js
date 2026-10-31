// Research hook (eventType "log"): turns a Blueprint SDK console line that the connector already
// sends as a `log` event into a Takaro action, with no connector protocol change and no stub.
//   takaro run <steam64> <command text>       -> runs the command as that player (Takaro's own pipeline)
//   takaro event <steam64|-> <name> <json...>  -> a custom event (logged; answered privately here)
import { data, takaro } from '@takaro/helpers';

async function main() {
  const { gameServerId, eventData } = data;
  const line = String((eventData && eventData.msg) || '');
  // Anchored: only the engine's own "Command not recognized" line, optionally behind the log's
  // "[date][frame]" prefix. A player's chat is logged as "...ChatWindow: Character X said: <text>",
  // so typed text can never start the line (an unanchored match was exploitable from chat).
  const m = line.match(/^(?:\[[^\]]*\]\[[^\]]*\])?Command not recognized: takaro (run|event) (\S+)\s*(.*)$/);
  if (!m) {
    console.log(`sdk-bridge: ignored a line that is not an engine console line: ${line.slice(0, 120)}`);
    return;
  }
  const verb = m[1];
  const who = m[2];
  const rest = m[3].trim();
  let pog = null;
  if (/^7656\d{13}$/.test(who)) {
    const res = await takaro.playerOnGameserver.playerOnGameServerControllerSearch({
      filters: { gameServerId: [gameServerId], gameId: [who] },
      limit: 1,
    });
    pog = res.data.data[0] || null;
  }
  if (verb === 'run') {
    if (!pog) {
      console.log(`sdk-bridge: run for unknown player ${who}`);
      return;
    }
    const res = await takaro.command.commandControllerTrigger(gameServerId, { playerId: pog.playerId, msg: rest });
    console.log(`sdk-bridge: run '${rest}' as ${who} -> executions ${JSON.stringify(res.data.data.executionIds)}`);
    return;
  }
  const sp = rest.indexOf(' ');
  const name = sp < 0 ? rest : rest.slice(0, sp);
  const payload = sp < 0 ? '' : rest.slice(sp + 1);
  console.log(`sdk-bridge: custom event '${name}' player=${who} payload=${payload}`);
  if (pog) {
    await takaro.gameserver.gameServerControllerSendMessage(gameServerId, {
      message: `SDK event ${name} reached Takaro`,
      opts: { recipient: { gameId: who } },
    });
  }
}

await main();
