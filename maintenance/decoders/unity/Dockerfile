# takaro-icons-decoder-unity: UnityPy-based exporter for Unity asset bundles.
# Built locally by `takaro-maint icons build` and never pushed. The network is needed at build time only
# (the hash-locked pip download); the container runs with --network none and a read-only root file system.
# Base image digest resolved 2026-11-07 with `docker buildx imagetools inspect python:3.12-slim-bookworm`.
FROM python:3.12-slim-bookworm@sha256:935c3c5b12357a7e10caa9c1e6ca5692344d97cf229832ef584be2cec9fc03df
COPY requirements.lock /tmp/requirements.lock
RUN pip install --no-cache-dir --no-deps --require-hashes --only-binary=:all: -r /tmp/requirements.lock \
    && rm /tmp/requirements.lock
COPY export.py /app/export.py
RUN python -m compileall -q /app
RUN groupadd --gid 10001 export && useradd --uid 10001 --gid 10001 --no-create-home --shell /usr/sbin/nologin export
USER 10001:10001
ENV PYTHONDONTWRITEBYTECODE=1 PYTHONHASHSEED=0 PYTHONNOUSERSITE=1
ENTRYPOINT ["python", "-I", "/app/export.py"]
