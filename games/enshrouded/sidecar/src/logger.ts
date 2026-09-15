const prefix = '[Takaro Enshrouded]';

export const logger = {
  debug: (message: string): void => {
    if (process.env.DEBUG) console.debug(`${prefix} ${message}`);
  },
  info: (message: string): void => console.info(`${prefix} ${message}`),
  warn: (message: string): void => console.warn(`${prefix} ${message}`),
  error: (message: string): void => console.error(`${prefix} ${message}`),
};
