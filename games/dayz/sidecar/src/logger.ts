import { createLogger, format, transports } from 'winston';

export const logger = createLogger({
  level: process.env.LOG_LEVEL || 'info',
  format: format.combine(
    format.timestamp({ format: 'HH:mm:ss.SSS' }),
    format.printf(({ timestamp, level, message }) => `[${timestamp}] ${level} ${message}`),
  ),
  transports: [new transports.Console()],
});
