# syntax=docker/dockerfile:1
# Takaro DayZ sidecar: speaks the @TakaroIntegration mod's local HTTP shape on
# 127.0.0.1:8088 and the Takaro connector WebSocket upstream.
#
# BUILD CONTEXT = games/dayz/sidecar (the source tree; this file compiles it).
# compose/dayz.yml points `context:` there (DAYZ_BRIDGE_SRC) and `dockerfile:` here.
# The release tarball ships games/dayz/sidecar/Dockerfile instead, which copies a
# prebuilt dist/.
FROM node:22-slim AS build
WORKDIR /app
COPY package.json package-lock.json ./
RUN npm ci
COPY tsconfig.json ./
COPY src ./src
RUN npm run build && npm prune --omit=dev

FROM node:22-slim
RUN apt-get update \
    && apt-get install -y --no-install-recommends curl ca-certificates \
    && rm -rf /var/lib/apt/lists/*
ENV NODE_ENV=production \
    BRIDGE_LOCAL_PORT=8088 \
    BRIDGE_CONFIG=/data/TakaroConfig.txt \
    TAKARO_WS_URL=wss://connect.takaro.io/
WORKDIR /app
COPY --from=build /app/node_modules ./node_modules
COPY --from=build /app/dist ./dist
COPY package.json ./
RUN mkdir -p /data && chown node:node /data
USER node
VOLUME ["/data"]
EXPOSE 8088
HEALTHCHECK --interval=30s --timeout=5s --start-period=15s \
    CMD curl -fsS "http://127.0.0.1:${BRIDGE_LOCAL_PORT:-8088}/health" >/dev/null || exit 1
CMD ["node", "dist/index.js"]
