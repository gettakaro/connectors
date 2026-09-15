import { describe, expect, it } from 'vitest';
import { loadConfig } from '../config.js';

describe('config', () => {
  it('applies defaults', () => {
    const c = loadConfig({ TAKARO_PLUGIN_TOKEN: 'p', TAKARO_REGISTRATION_TOKEN: 'r' });
    expect(c.identityToken).toBe('takaro-dev-enshrouded');
    expect(c.registrationToken).toBe('r');
    expect(c.pluginBaseUrl).toBe('http://127.0.0.1:18890');
    expect(c.takaroWsUrl).toBe('wss://connect.takaro.io/');
    expect(c.logTailMode).toBe('auto');
  });
  it('requires plugin token and validates log tail mode', () => {
    expect(() => loadConfig({})).toThrow(/TAKARO_PLUGIN_TOKEN/);
    expect(() => loadConfig({ TAKARO_PLUGIN_TOKEN: 'p', ENSHROUDED_LOG_TAIL: 'maybe' })).toThrow(/auto\|always\|never/);
  });
  it('honours overrides', () => {
    const c = loadConfig({ TAKARO_PLUGIN_TOKEN: 'p', TAKARO_IDENTITY_TOKEN: 'x', TAKARO_PLUGIN_URL: 'http://h:1/', TAKARO_POLL_INTERVAL_MS: '250' });
    expect(c.identityToken).toBe('x');
    expect(c.pluginBaseUrl).toBe('http://h:1');
    expect(c.pollIntervalMs).toBe(250);
  });
});
