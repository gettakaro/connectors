const mineflayer = require('mineflayer');
const { Vec3 } = require('vec3');

class BotInstance {
  constructor({ name, host, port, username, reconnectDelay, maxReconnectDelay }) {
    this.name = name;
    this.host = host;
    this.port = port;
    this.username = `${username}_${name}`;
    this.reconnectDelay = reconnectDelay;
    this.maxReconnectDelay = maxReconnectDelay;
    this.currentDelay = reconnectDelay;
    this.bot = null;
    this.connected = false;
    this.destroyed = false;
    this.reconnectTimer = null;
    this.moveTimer = null;
    this.moveInterval = null;
  }

  connect() {
    if (this.destroyed) return;

    console.log(`[${this.name}] Connecting to ${this.host}:${this.port} as ${this.username}...`);

    this.bot = mineflayer.createBot({
      host: this.host,
      port: this.port,
      username: this.username,
      version: '1.21.11',
      auth: 'offline',
      hideErrors: false,
    });

    this.bot.on('spawn', () => {
      console.log(`[${this.name}] Connected and spawned`);
      this.connected = true;
      this.currentDelay = this.reconnectDelay;
    });

    this.bot.on('kicked', (reason) => {
      console.log(`[${this.name}] Kicked: ${reason}`);
      this.connected = false;
      this.scheduleReconnect();
    });

    this.bot.on('end', (reason) => {
      console.log(`[${this.name}] Disconnected: ${reason}`);
      this.connected = false;
      this.scheduleReconnect();
    });

    this.bot.on('error', (err) => {
      console.error(`[${this.name}] Error: ${err.message}`);
    });
  }

  scheduleReconnect() {
    if (this.destroyed) return;
    if (this.reconnectTimer) return;

    console.log(`[${this.name}] Reconnecting in ${this.currentDelay}ms...`);
    this.reconnectTimer = setTimeout(() => {
      this.reconnectTimer = null;
      this.connect();
    }, this.currentDelay);

    this.currentDelay = Math.min(this.currentDelay * 2, this.maxReconnectDelay);
  }

  destroy() {
    this.destroyed = true;
    if (this.reconnectTimer) {
      clearTimeout(this.reconnectTimer);
      this.reconnectTimer = null;
    }
    this._clearMoveTimers();
    if (this.bot) {
      this.bot.removeAllListeners();
      this.bot.quit();
      this.bot = null;
    }
  }

  _clearMoveTimers() {
    if (this.moveInterval) {
      clearInterval(this.moveInterval);
      this.moveInterval = null;
    }
    if (this.moveTimer) {
      clearTimeout(this.moveTimer);
      this.moveTimer = null;
    }
  }

  chat(message) {
    this.bot.chat(message);
    return { sent: message };
  }

  moveTo(x, y, z) {
    const target = { x, y, z };
    this._clearMoveTimers();
    this.bot.lookAt(new Vec3(x, y + 1, z));

    this.bot.setControlState('forward', true);
    this.moveInterval = setInterval(() => {
      if (!this.bot || !this.bot.entity) {
        this._clearMoveTimers();
        return;
      }
      const pos = this.bot.entity.position;
      const dist = Math.sqrt((pos.x - x) ** 2 + (pos.z - z) ** 2);
      if (dist < 2) {
        this.bot.setControlState('forward', false);
        this._clearMoveTimers();
      }
    }, 100);

    this.moveTimer = setTimeout(() => {
      if (this.bot) this.bot.setControlState('forward', false);
      this._clearMoveTimers();
    }, 30000);

    return { movingTo: target };
  }

  attack() {
    const entity = this.bot.nearestEntity((e) => e.type === 'mob' || e.type === 'player');
    if (!entity) return { error: 'No entity nearby' };
    this.bot.attack(entity);
    return { attacked: entity.name || entity.username || entity.type, id: entity.id };
  }

  useBlock() {
    const block = this.bot.blockAtCursor(5);
    if (!block) return { error: 'No block in sight' };
    this.bot.activateBlock(block);
    return { used: block.name, position: block.position };
  }

  lookAt(x, y, z) {
    this.bot.lookAt(new Vec3(x, y, z));
    return { lookingAt: { x, y, z } };
  }

  jump() {
    this.bot.setControlState('jump', true);
    setTimeout(() => {
      if (this.bot) this.bot.setControlState('jump', false);
    }, 500);
    return { jumped: true };
  }

  respawn() {
    if (!this.bot.game || this.bot.health > 0) return { error: 'Not dead' };
    this.bot._client.write('client_command', { actionId: 0 });
    return { respawned: true };
  }

  getStatus() {
    if (!this.bot || !this.connected) {
      return { connected: false, name: this.name };
    }
    return {
      connected: true,
      name: this.name,
      username: this.username,
      health: this.bot.health,
      food: this.bot.food,
      position: this.bot.entity?.position,
      gameMode: this.bot.game?.gameMode,
    };
  }

  getPlayers() {
    return Object.values(this.bot.players).map((p) => ({
      username: p.username,
      uuid: p.uuid,
      ping: p.ping,
      gamemode: p.gamemode,
    }));
  }

  getInventory() {
    return this.bot.inventory.items().map((item) => ({
      name: item.name,
      count: item.count,
      slot: item.slot,
      displayName: item.displayName,
    }));
  }

  getPosition() {
    return this.bot.entity?.position || null;
  }

  getHealth() {
    return {
      health: this.bot.health,
      food: this.bot.food,
      saturation: this.bot.foodSaturation,
    };
  }
}

module.exports = { BotInstance };
