// The headless player behind Minecraft's gameplay checks.
//
// One JSON object per line in both directions. stdin carries commands from the harness
// (`chat`, `inventory`, `position`, `quit`); stdout carries what this player saw: `spawn`
// once it is in the world, the answer to each command, and the server's own pushes
// (`forcedMove` after a teleport, `message` for every chat or system line). Anything on
// stderr is diagnostics. The process exits 0 only after a `quit` it was asked for.
import { createInterface } from 'node:readline'
import mineflayer from 'mineflayer'

const [host, port, username, version] = process.argv.slice(2)
let quitting = false

function emit(event, fields = {}) {
  process.stdout.write(JSON.stringify({ event, ...fields }) + '\n')
}

function position() {
  const { x, y, z } = bot.entity.position
  return { x, y, z }
}

const bot = mineflayer.createBot({
  host,
  port: Number(port),
  username,
  auth: 'offline',
  version: version || false,
  // Nothing renders, so nothing beyond the spawn chunks is worth asking the server for.
  viewDistance: 'tiny',
})

bot.once('spawn', () => {
  emit('spawn', {
    username: bot.username,
    uuid: bot.player?.uuid ?? bot._client.uuid ?? null,
    version: bot.version,
    protocol: bot.protocolVersion,
    position: position(),
  })
})
bot.on('forcedMove', () => emit('forcedMove', { position: position() }))
bot.on('messagestr', (text) => emit('message', { text }))
bot.on('kicked', (reason) => emit('kicked', { reason: typeof reason === 'string' ? reason : JSON.stringify(reason) }))
bot.on('error', (error) => emit('error', { message: String(error?.message ?? error) }))
bot.on('end', (reason) => {
  emit('end', { reason: String(reason ?? '') })
  process.exit(quitting ? 0 : 1)
})

createInterface({ input: process.stdin }).on('line', (line) => {
  let command
  try {
    command = JSON.parse(line)
  } catch {
    emit('error', { message: `not JSON: ${line.slice(0, 200)}` })
    return
  }
  switch (command.op) {
    case 'chat':
      bot.chat(command.text)
      emit('chat', { text: command.text })
      break
    case 'inventory':
      emit('inventory', { items: bot.inventory.items().map((item) => ({ name: item.name, count: item.count })) })
      break
    case 'position':
      emit('position', { position: position() })
      break
    case 'quit':
      quitting = true
      bot.quit('takaro-verify done')
      break
    default:
      emit('error', { message: `unknown op ${command.op}` })
  }
})
