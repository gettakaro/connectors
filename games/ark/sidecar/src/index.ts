import http from 'node:http';
import { BanManager } from './native/banManager.js';
import { FileBanStore } from './native/banStore.js';
import { ArkAdapter } from './adapter.js';
import { loadConfig } from './config.js';
import { EventPump } from './eventPump.js';
import { logger } from './logger.js';
import { NativeClient } from './native/client.js';
import { FileCursorStore } from './native/cursorStore.js';
import { handleTakaroRequest } from './requestHandler.js';
import { TakaroWsClient } from './takaro/client.js';
import type { WsMessage } from './takaro/protocol.js';

async function main(): Promise<void> {
  const config = loadConfig();
  const native = new NativeClient(config.nativeUrl, config.nativeToken, config.timeoutMs);
  const banManager = new BanManager(native, new FileBanStore(config.banMetadataFile));
  const adapter = new ArkAdapter(native, banManager);
  const takaro = new TakaroWsClient(config.takaroWsUrl, {
    identityToken: config.identityToken,
    registrationToken: config.registrationToken,
    serverName: config.serverName,
  });
  const events = new EventPump(native, takaro, new FileCursorStore(config.cursorFile), config.pollIntervalMs);

  takaro.on('request', (message: WsMessage) => {
    void handleTakaroRequest(message, adapter, (response) => takaro.send(response));
  });
  takaro.on('identified', () => events.start());
  takaro.on('disconnected', () => events.disconnected());

  const server = http.createServer((_request, response) => {
    if (_request.url !== '/health') {
      response.writeHead(404).end();
      return;
    }
    response.writeHead(200, { 'Content-Type': 'application/json' });
    response.end(JSON.stringify({
      ok: true,
      takaroIdentified: takaro.identified(),
      gameServerId: takaro.getGameServerId(),
      nativeBootId: events.currentBootId() ?? null,
      eventCursor: events.cursor(),
      eventScanCursor: events.scanCursor(),
      banReconciliationReady: banManager.ready(),
      lastConfirmedSendId: takaro.lastConfirmedId(),
    }));
  });
  await new Promise<void>((resolve) => server.listen(config.healthPort, config.healthHost, resolve));
  logger.info(`Sidecar health on http://${config.healthHost}:${config.healthPort}/health`);
  banManager.start();
  takaro.connect();

  const stop = (): void => {
    events.stop();
    banManager.stop();
    takaro.shutdown();
    server.close();
  };
  process.once('SIGINT', stop);
  process.once('SIGTERM', stop);
}

main().catch((error) => {
  logger.error(`Fatal startup error: ${error instanceof Error ? error.stack ?? error.message : String(error)}`);
  process.exitCode = 1;
});
