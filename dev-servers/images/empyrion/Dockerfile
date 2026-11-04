# Wine runtime for the Windows-only Empyrion dedicated server (Steam app 530870).
# The server bytes are bind-mounted, never baked: this image is only the runtime.
FROM debian:bookworm-slim
ENV DEBIAN_FRONTEND=noninteractive WINEDEBUG=-all WINEARCH=win64 WINEPREFIX=/wine
RUN dpkg --add-architecture i386 && apt-get update && \
    apt-get install -y --no-install-recommends ca-certificates curl gnupg xvfb xauth procps && \
    mkdir -pm755 /etc/apt/keyrings && \
    curl -fsSL https://dl.winehq.org/wine-builds/winehq.key | gpg --dearmor -o /etc/apt/keyrings/winehq.gpg && \
    echo "deb [signed-by=/etc/apt/keyrings/winehq.gpg] https://dl.winehq.org/wine-builds/debian/ bookworm main" > /etc/apt/sources.list.d/winehq.list && \
    apt-get update && apt-get install -y --install-recommends winehq-stable && \
    apt-get clean && rm -rf /var/lib/apt/lists/*
RUN useradd -m -u 1000 empyrion && mkdir -p /wine && chown 1000:1000 /wine
USER empyrion
COPY --chown=1000:1000 entrypoint.sh /entrypoint.sh
ENTRYPOINT ["/bin/bash", "/entrypoint.sh"]
