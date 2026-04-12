const express = require('express');

function createApi(bots, port) {
  const app = express();
  app.use(express.json());

  function getBot(req, res) {
    const bot = bots.get(req.params.server);
    if (!bot) {
      res.status(404).json({ error: `Unknown server: ${req.params.server}` });
      return null;
    }
    if (!bot.connected) {
      res.status(503).json({ error: `Bot not connected to ${req.params.server}` });
      return null;
    }
    return bot;
  }

  app.get('/status', (_req, res) => {
    const status = {};
    for (const [name, bot] of bots) {
      status[name] = bot.getStatus();
    }
    res.json(status);
  });

  app.post('/bot/:server/chat', (req, res) => {
    const bot = getBot(req, res);
    if (!bot) return;
    res.json(bot.chat(req.body.message));
  });

  app.post('/bot/:server/move', (req, res) => {
    const bot = getBot(req, res);
    if (!bot) return;
    const { x, y, z } = req.body;
    res.json(bot.moveTo(x, y, z));
  });

  app.post('/bot/:server/attack', (req, res) => {
    const bot = getBot(req, res);
    if (!bot) return;
    res.json(bot.attack());
  });

  app.post('/bot/:server/use', (req, res) => {
    const bot = getBot(req, res);
    if (!bot) return;
    res.json(bot.useBlock());
  });

  app.post('/bot/:server/look', (req, res) => {
    const bot = getBot(req, res);
    if (!bot) return;
    const { x, y, z } = req.body;
    res.json(bot.lookAt(x, y, z));
  });

  app.post('/bot/:server/jump', (req, res) => {
    const bot = getBot(req, res);
    if (!bot) return;
    res.json(bot.jump());
  });

  app.post('/bot/:server/respawn', (req, res) => {
    const bot = getBot(req, res);
    if (!bot) return;
    res.json(bot.respawn());
  });

  app.get('/bot/:server/players', (req, res) => {
    const bot = getBot(req, res);
    if (!bot) return;
    res.json(bot.getPlayers());
  });

  app.get('/bot/:server/position', (req, res) => {
    const bot = getBot(req, res);
    if (!bot) return;
    res.json(bot.getPosition());
  });

  app.get('/bot/:server/health', (req, res) => {
    const bot = getBot(req, res);
    if (!bot) return;
    res.json(bot.getHealth());
  });

  app.get('/bot/:server/inventory', (req, res) => {
    const bot = getBot(req, res);
    if (!bot) return;
    res.json(bot.getInventory());
  });

  return new Promise((resolve) => {
    const server = app.listen(port, () => {
      console.log(`Bot API listening on port ${port}`);
      resolve(server);
    });
  });
}

module.exports = { createApi };
