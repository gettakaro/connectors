import http from 'node:http';

export interface HealthStatus {
  ok: boolean;
  takaroIdentified: boolean;
  gameServerId: string | null;
  rconConfigured: boolean;
  logTailers: number;
  modBridge?: unknown;
}

export class HealthServer {
  private server: http.Server | null = null;

  constructor(
    private readonly requestedPort: number,
    private readonly getStatus: () => HealthStatus,
    private readonly extraHandler?: (req: http.IncomingMessage, res: http.ServerResponse) => Promise<boolean> | boolean,
  ) {}

  async start(): Promise<void> {
    this.server = http.createServer((req, res) => {
      if (req.method === 'GET' && req.url?.split('?')[0] === '/health') {
        const body = JSON.stringify(this.getStatus());
        res.statusCode = 200;
        res.setHeader('Content-Type', 'application/json');
        res.setHeader('Content-Length', Buffer.byteLength(body));
        res.end(body);
        return;
      }

      void this.handleExtra(req, res);
    });

    await new Promise<void>((resolve) => {
      this.server!.listen(this.requestedPort, '127.0.0.1', resolve);
    });
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

  private async handleExtra(req: http.IncomingMessage, res: http.ServerResponse): Promise<void> {
    try {
      if (this.extraHandler && (await this.extraHandler(req, res))) return;
      res.statusCode = 404;
      res.setHeader('Content-Type', 'application/json');
      res.end(JSON.stringify({ error: 'Not found' }));
    } catch (err) {
      const message = err instanceof Error ? err.message : String(err);
      res.statusCode = 500;
      res.setHeader('Content-Type', 'application/json');
      res.end(JSON.stringify({ error: message }));
    }
  }
}
