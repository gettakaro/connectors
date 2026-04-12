FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

RUN dpkg --add-architecture i386 && \
    apt-get update && \
    apt-get install -y \
        lib32gcc-s1 \
        lib32stdc++6 \
        libsdl2-2.0-0:i386 \
        curl \
        ca-certificates \
        expect \
        && rm -rf /var/lib/apt/lists/*

# Create non-root user
RUN useradd -m -s /bin/bash steam

# Install SteamCMD
RUN mkdir -p /steamcmd && \
    curl -sL https://steamcdn-a.akamaihd.net/client/installer/steamcmd_linux.tar.gz | tar -xzf - -C /steamcmd && \
    chown -R steam:steam /steamcmd

# Install Rust dedicated server
RUN mkdir -p /rust && chown -R steam:steam /rust

USER steam

RUN /steamcmd/steamcmd.sh \
    +force_install_dir /rust \
    +login anonymous \
    +app_update 258550 validate \
    +quit

USER root

# Install Carbon
RUN curl -L https://github.com/CarbonCommunity/Carbon/releases/download/production_build/Carbon.Linux.Release.tar.gz \
    -o /tmp/carbon.tar.gz && \
    tar -xzf /tmp/carbon.tar.gz -C /rust && \
    rm /tmp/carbon.tar.gz && \
    chown -R steam:steam /rust

COPY start.sh /start.sh
RUN chmod +x /start.sh

WORKDIR /rust

EXPOSE 28015/udp 28016

ENTRYPOINT ["/start.sh"]
