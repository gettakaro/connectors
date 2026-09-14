import { MockPlugin } from './mockPlugin.js';

const mock = new MockPlugin();
mock.token = process.env.TAKARO_PLUGIN_TOKEN || 'dev-token';
const port = Number(process.env.MOCK_PLUGIN_PORT || 18890);
await mock.start(port);
console.log(`Mock Enshrouded plugin on ${mock.url()} (token ${mock.token})`);
setInterval(() => mock.pushEvent('log', { msg: `mock heartbeat ${new Date().toISOString()}` }), 30000);
