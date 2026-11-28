# syntax=docker/dockerfile:1
# Runtime image for the RELEASE tarball, where dist/ is already built. The dev rig builds
# from source with dev-servers/images/dayz-bridge/Dockerfile instead.
FROM node:22-slim
ENV NODE_ENV=production \
    BRIDGE_LOCAL_PORT=8088 \
    BRIDGE_BIND_ADDR=127.0.0.1 \
    BRIDGE_CONFIG=/data/TakaroConfig.txt
WORKDIR /app
COPY package*.json ./
RUN npm ci --omit=dev --no-audit --no-fund
COPY dist ./dist
# The game icon the sidecar sends Takaro in its game profile.
COPY assets ./assets
RUN mkdir -p /data && chown node:node /data
USER node
VOLUME ["/data"]
HEALTHCHECK --interval=30s --timeout=5s --start-period=15s \
    CMD node -e "fetch('http://127.0.0.1:'+(process.env.BRIDGE_LOCAL_PORT||8088)+'/health').then(r=>process.exit(r.ok?0:1),()=>process.exit(1))"
CMD ["node", "dist/index.js"]
