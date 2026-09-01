import http from 'node:http';
import { ACTION_COVERAGE, EVENT_COVERAGE } from '../takaro/coverage.js';

export interface HealthStatus {
  ok: boolean;
  takaroIdentified: boolean;
  gameServerId: string | null;
  tshockReachable: boolean;
  lastPollAt: string | null;
}

export class HealthServer {
  private server: http.Server | null = null;

  constructor(
    private readonly requestedPort: number,
    private readonly getStatus: () => HealthStatus,
  ) {}

  async start(): Promise<void> {
    this.server = http.createServer((req, res) => {
      const pathname = req.url?.split('?')[0];
      if (req.method === 'GET' && pathname === '/health') {
        sendJson(res, 200, this.getStatus());
        return;
      }
      if (req.method === 'GET' && pathname === '/coverage') {
        sendJson(res, 200, { actions: ACTION_COVERAGE, events: EVENT_COVERAGE });
        return;
      }
      sendJson(res, 404, { error: 'Not found' });
    });
    await new Promise<void>((resolve) => this.server!.listen(this.requestedPort, '127.0.0.1', resolve));
  }

  async stop(): Promise<void> {
    if (!this.server) return;
    await new Promise<void>((resolve) => this.server!.close(() => resolve()));
    this.server = null;
  }

  port(): number {
    const address = this.server?.address();
    if (!address || typeof address === 'string') return this.requestedPort;
    return address.port;
  }
}

function sendJson(res: http.ServerResponse, status: number, body: unknown): void {
  const raw = JSON.stringify(body);
  res.statusCode = status;
  res.setHeader('Content-Type', 'application/json');
  res.setHeader('Content-Length', Buffer.byteLength(raw));
  res.end(raw);
}
