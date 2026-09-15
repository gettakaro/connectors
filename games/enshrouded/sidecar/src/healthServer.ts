import http from 'node:http';

export class HealthServer {
  private server: http.Server | null = null;

  constructor(
    private readonly port: number,
    private readonly host: string,
    private readonly snapshot: () => Promise<Record<string, unknown>> | Record<string, unknown>,
  ) {}

  address(): number | null {
    const addr = this.server?.address();
    return addr && typeof addr === 'object' ? addr.port : null;
  }

  start(): Promise<void> {
    if (this.server) return Promise.resolve();
    this.server = http.createServer((req, res) => {
      if (req.method !== 'GET' || req.url?.split('?')[0] !== '/health') {
        res.statusCode = 404;
        res.end('not found');
        return;
      }
      Promise.resolve(this.snapshot())
        .then((body) => {
          const raw = JSON.stringify(body);
          res.statusCode = body.ok === false ? 503 : 200;
          res.setHeader('Content-Type', 'application/json');
          res.end(raw);
        })
        .catch((err: Error) => {
          res.statusCode = 500;
          res.end(JSON.stringify({ ok: false, error: err.message }));
        });
    });
    return new Promise((resolve, reject) => {
      this.server?.once('error', reject);
      this.server?.listen(this.port, this.host, () => resolve());
    });
  }

  stop(): Promise<void> {
    const active = this.server;
    this.server = null;
    if (!active) return Promise.resolve();
    return new Promise((resolve, reject) => active.close((err) => (err ? reject(err) : resolve())));
  }
}
