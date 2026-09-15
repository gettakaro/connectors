import fs from 'node:fs';
import path from 'node:path';
import winston from 'winston';

const logsDir = path.join(process.cwd(), 'logs');
if (!fs.existsSync(logsDir)) {
  fs.mkdirSync(logsDir, { recursive: true });
}

const format = winston.format.combine(
  winston.format.timestamp(),
  winston.format.printf(({ timestamp, level, message }) => `${timestamp} [${level.toUpperCase()}] ${message}`),
);

export const logger = winston.createLogger({
  level: process.env.LOG_LEVEL || 'info',
  format,
  transports: [
    new winston.transports.Console({ format }),
    new winston.transports.File({
      filename: path.join(logsDir, 'conan-exiles-takaro.log'),
      format,
      maxsize: 5 * 1024 * 1024,
      maxFiles: 5,
    }),
  ],
});
