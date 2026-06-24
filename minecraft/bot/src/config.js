const DEFAULT_SERVERS = 'paper:paper:25565,neoforge:neoforge:25565,fabric:fabric:25565';

function parseConfig() {
  const serversRaw = process.env.BOT_SERVERS || DEFAULT_SERVERS;
  const servers = serversRaw.split(',').map((entry) => {
    const [name, host, port] = entry.trim().split(':');
    return { name, host, port: parseInt(port, 10) };
  });

  return {
    servers,
    username: process.env.BOT_USERNAME || 'TakaroBot',
    reconnectDelay: parseInt(process.env.BOT_RECONNECT_DELAY || '5000', 10),
    maxReconnectDelay: parseInt(process.env.BOT_MAX_RECONNECT_DELAY || '60000', 10),
    apiPort: parseInt(process.env.BOT_API_PORT || '3001', 10),
  };
}

module.exports = { parseConfig };
