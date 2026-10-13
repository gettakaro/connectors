const prefix = '[Takaro Dune]';

/**
 * Every log line goes through this. The Dune battlegroup hands the sidecar four separate secrets — the GM
 * `ServerCommandsAuthToken`, the Postgres URL (password in the userinfo), the AMQP URL (password in the userinfo) and
 * the Takaro identity/registration tokens — and all four appear inside payloads we would otherwise dump verbatim when
 * DEBUG=1 (GM envelopes carry `"AuthToken"`, connection errors quote the URL). Redaction therefore lives in the logger
 * itself rather than at each call site, so a new call site cannot forget it.
 */
export function redactSecrets(message: string): string {
  return (
    message
      // JSON fields: {"AuthToken":"…"}, {"identityToken":"…"}, {"password":"…"}
      .replace(
        /("(?:AuthToken|authToken|identityToken|registrationToken|token|password|Password|pass|secret)"\s*:\s*")[^"]*"/g,
        '$1<redacted>"',
      )
      // key=value / key: value in free text (env dumps, ini lines, rabbitmqctl output)
      .replace(/\b((?:\w*(?:Token|Password|password|Secret|secret))\s*[=:]\s*)(?:"([^"]*)"|([^\s",;]+))/g, '$1<redacted>')
      // URL userinfo: amqps://user:pass@host, postgres://user:pass@host
      .replace(/(\b[a-z][a-z0-9+.-]*:\/\/[^\s:/@]+:)[^\s@/]+(@)/gi, '$1<redacted>$2')
      // Funcom join-password style query args
      .replace(/(\?p=)[^\s"&]+/gi, '$1<redacted>')
      // JWTs (Takaro's errors can quote its internal `x-takaro-token`)
      .replace(/\beyJ[\w-]+\.[\w-]+\.[\w-]+/g, '<redacted-jwt>')
  );
}

export const logger = {
  debug: (message: string): void => {
    if (process.env.DEBUG && process.env.DEBUG !== '0') console.debug(`${prefix} ${redactSecrets(message)}`);
  },
  info: (message: string): void => console.info(`${prefix} ${redactSecrets(message)}`),
  warn: (message: string): void => console.warn(`${prefix} ${redactSecrets(message)}`),
  error: (message: string): void => console.error(`${prefix} ${redactSecrets(message)}`),
};
