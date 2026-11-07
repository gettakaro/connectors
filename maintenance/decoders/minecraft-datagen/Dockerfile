# takaro-icons-decoder-minecraft-datagen: runs the Minecraft server jar's data generator (--reports) and hands the
# reports back. Built locally by `takaro-maint icons build` and never pushed. Nothing is downloaded at build time;
# the container runs with --network none and a read-only root file system.
# Both base images are pinned by digest elsewhere in this repository: the Temurin 25 JDK is the
# `build.toolchain` image of the Minecraft targets (catalog/minecraft/targets/*.json), the Python image is the
# base of maintenance/decoders/unity/Dockerfile.
FROM eclipse-temurin:25-jdk@sha256:010ab2b79329b951bf052dbf515843ab345ae4e4f0ef72287d85c9fa54da275f AS jdk
FROM python:3.12-slim-bookworm@sha256:935c3c5b12357a7e10caa9c1e6ca5692344d97cf229832ef584be2cec9fc03df
COPY --from=jdk /opt/java/openjdk /opt/java/openjdk
ENV JAVA_HOME=/opt/java/openjdk PATH=/opt/java/openjdk/bin:$PATH
COPY export.py /app/export.py
RUN python -m compileall -q /app
RUN groupadd --gid 10001 export && useradd --uid 10001 --gid 10001 --no-create-home --shell /usr/sbin/nologin export
USER 10001:10001
ENV PYTHONDONTWRITEBYTECODE=1 PYTHONHASHSEED=0 PYTHONNOUSERSITE=1
ENTRYPOINT ["python", "-I", "/app/export.py"]
