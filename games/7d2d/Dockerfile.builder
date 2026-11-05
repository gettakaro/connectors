ARG TOOLCHAIN
FROM ${TOOLCHAIN}

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
  apt-get install -y nuget curl unzip zip git && \
  apt-get clean && \
  rm -rf /var/lib/apt/lists/*

RUN mono --version && \
  msbuild /version || echo "Installing MSBuild..." && \
  apt-get update && \
  apt-get install -y mono-complete mono-devel && \
  rm -rf /var/lib/apt/lists/*  

# ILRepack folds the mod's own third-party assemblies into Takaro.dll (scripts/build-mod.sh),
# so the release ships nothing but Takaro.dll next to ModInfo.xml. 2.0.18 is the last
# release that runs on Mono; later ones need the .NET runtime.
ARG ILREPACK_VERSION=2.0.18
ARG ILREPACK_SHA256=082620ac829291247b418e317f155fb914632251e644383822d6dfecd0837050
RUN curl -fsSL "https://globalcdn.nuget.org/packages/ilrepack.${ILREPACK_VERSION}.nupkg" -o /tmp/ilrepack.nupkg && \
  echo "${ILREPACK_SHA256}  /tmp/ilrepack.nupkg" | sha256sum --check --status && \
  unzip -q -j /tmp/ilrepack.nupkg tools/ILRepack.exe -d /opt/ilrepack && \
  rm /tmp/ilrepack.nupkg

# Create necessary directories
RUN mkdir -p /app/lib /app/packages

# Set the default command
CMD ["bash", "-c", "echo 'Builder container ready' && tail -f /dev/null"]
