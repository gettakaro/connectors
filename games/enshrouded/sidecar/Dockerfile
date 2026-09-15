# syntax=docker/dockerfile:1
FROM node:22-alpine AS build
WORKDIR /app
COPY package.json package-lock.json ./
RUN npm ci
COPY tsconfig.json tsconfig.build.json ./
COPY src ./src
RUN npm run build && npm prune --omit=dev

FROM node:22-alpine
ENV NODE_ENV=production \
    TAKARO_PLUGIN_URL=http://127.0.0.1:18890 \
    TAKARO_CURSOR_FILE=/data/event-cursor.json \
    ENSHROUDED_LOG_FILE=/opt/enshrouded/server/logs/enshrouded_server.log
WORKDIR /app
COPY --from=build /app/node_modules ./node_modules
COPY --from=build /app/dist ./dist
COPY package.json ./
RUN mkdir -p /data && chown node:node /data
USER node
VOLUME ["/data"]
HEALTHCHECK --interval=30s --timeout=5s CMD wget -qO- http://127.0.0.1:${SIDECAR_HEALTH_PORT:-18891}/health >/dev/null || exit 1
CMD ["node", "dist/index.js"]
