FROM mono:6.12.0.182-slim

WORKDIR /app

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