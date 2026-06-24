const { parseConfig } = require('./config');
const { BotInstance } = require('./bot-instance');
const { createApi } = require('./api');

async function main() {
  const config = parseConfig();
  const bots = new Map();

  for (const server of config.servers) {
    const bot = new BotInstance({
      name: server.name,
      host: server.host,
      port: server.port,
      username: config.username,
      reconnectDelay: config.reconnectDelay,
      maxReconnectDelay: config.maxReconnectDelay,
    });
    bots.set(server.name, bot);
    bot.connect();
  }

  const server = await createApi(bots, config.apiPort);

  const shutdown = () => {
    console.log('Shutting down...');
    for (const bot of bots.values()) {
      bot.destroy();
    }
    server.close();
    process.exit(0);
  };

  process.on('SIGTERM', shutdown);
  process.on('SIGINT', shutdown);
}

main();
