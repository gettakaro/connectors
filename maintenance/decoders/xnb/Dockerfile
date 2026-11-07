# takaro-icons-decoder-xnb: XNA content (.xnb) texture exporter, on the npm package `xnb` (LGPL-3.0-or-later,
# used unmodified).
# Built locally by `takaro-maint icons build` and never pushed. The network is needed at build time only
# (`npm ci`, which enforces the lock's sha512 integrity values); the container runs with --network none and a
# read-only root file system.
# The base image is the Node digest the catalog already pins for the Terraria bridge runtime.
FROM node:22.23.2-bookworm-slim@sha256:48e4b67d85f87bd551df43704e24d252f56cc5f8e9718841aace50f19948f0f9
WORKDIR /app
COPY package.json package-lock.json ./
RUN npm ci --omit=dev --ignore-scripts --no-audit --no-fund && npm cache clean --force
COPY export.mjs png.mjs ./
RUN groupadd --gid 10001 export && useradd --uid 10001 --gid 10001 --no-create-home --shell /usr/sbin/nologin export
USER 10001:10001
ENV NODE_ENV=production
ENTRYPOINT ["node", "/app/export.mjs"]
