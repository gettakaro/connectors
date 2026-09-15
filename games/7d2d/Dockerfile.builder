FROM mono:6.12.0.182-slim@sha256:eb9a8d42dc58090cdb0d39b36422d9560f33e9d2cce22f4c888da263cee46c45

WORKDIR /app

# Debian buster is EOL — repoint apt to archive.debian.org and disable
# Valid-Until check (archived Release files are signed but expired).
# Mono apt repo (download.mono-project.com) still serves stable-buster, leave it.
RUN sed -i 's|http://deb.debian.org|http://archive.debian.org|g; \
            s|http://security.debian.org|http://archive.debian.org|g; \
            /buster-updates/d' /etc/apt/sources.list && \
    echo 'Acquire::Check-Valid-Until "false";' \
      > /etc/apt/apt.conf.d/99-archive-valid

RUN apt-get update && \
  apt-get install -y nuget curl unzip git && \
  apt-get clean && \
  rm -rf /var/lib/apt/lists/*

RUN mono --version && \
  msbuild /version || echo "Installing MSBuild..." && \
  apt-get update && \
  apt-get install -y mono-complete mono-devel && \
  rm -rf /var/lib/apt/lists/*  

# Create necessary directories
RUN mkdir -p /app/lib /app/packages

# Set the default command
CMD ["bash", "-c", "echo 'Builder container ready' && tail -f /dev/null"]