"""The icon bucket: environment resolution and a small S3 wrapper (Cloudflare R2, path-style).

Credentials come from the environment only and are registered with the redactor the moment they
are read, because only ``*SECRET*`` names are hidden automatically. The environments table has no
production entry on purpose: production is written by ``icons promote``, not by ``icons publish``.
"""

from __future__ import annotations

import os
from typing import Any
from urllib.parse import urlsplit

import boto3
import botocore.config
import botocore.exceptions

from .. import redact
from ..exit_codes import PublishFailed, UsageError
from . import contract

ENVIRONMENT_PREFIXES = {"staging": "STG_"}
_VARIABLES = ("R2_ACCESS_KEY_ID", "R2_SECRET_ACCESS_KEY", "R2_ENDPOINT", "R2_BUCKET")
_LOOPBACK_HOSTS = {"127.0.0.1", "localhost", "::1"}
_ABSENT_CODES = {"404", "NoSuchKey", "NotFound"}


class Bucket:
    def __init__(self, client: Any, bucket: str) -> None:
        self._client = client
        self._bucket = bucket

    @classmethod
    def for_env(cls, env: str) -> Bucket:
        prefix = ENVIRONMENT_PREFIXES.get(env)
        if prefix is None:
            raise UsageError(f"no icon bucket is configured for environment {env!r}")
        names = [prefix + name for name in _VARIABLES]
        values = {name: os.environ.get(name, "") for name in names}
        missing = [name for name, value in values.items() if not value]
        if missing:
            raise UsageError(f"missing environment variables: {', '.join(missing)}", missing=missing)
        key_id, secret, endpoint, bucket = (values[name] for name in names)
        redact.remember(key_id, secret, endpoint, bucket)
        _require_safe_endpoint(endpoint, names[2])
        return cls(_client(endpoint, key_id, secret), bucket)

    def get(self, key: str) -> bytes | None:
        contract.guard_keys([key])
        try:
            return bytes(self._client.get_object(Bucket=self._bucket, Key=key)["Body"].read())
        except botocore.exceptions.ClientError as exc:
            if _code(exc) in _ABSENT_CODES:
                return None
            raise _failure("GET", key, exc) from None
        except botocore.exceptions.BotoCoreError as exc:
            raise _failure("GET", key, exc) from None

    def exists(self, key: str) -> bool:
        contract.guard_keys([key])
        try:
            self._client.head_object(Bucket=self._bucket, Key=key)
        except botocore.exceptions.ClientError as exc:
            if _code(exc) in _ABSENT_CODES:
                return False
            raise _failure("HEAD", key, exc) from None
        except botocore.exceptions.BotoCoreError as exc:
            raise _failure("HEAD", key, exc) from None
        return True

    def put(self, key: str, body: bytes, content_type: str, cache_control: str) -> None:
        contract.guard_keys([key])
        try:
            self._client.put_object(
                Bucket=self._bucket, Key=key, Body=body, ContentType=content_type, CacheControl=cache_control
            )
        except (botocore.exceptions.ClientError, botocore.exceptions.BotoCoreError) as exc:
            raise _failure("PUT", key, exc) from None


def _client(endpoint: str, key_id: str, secret: str) -> Any:
    return boto3.session.Session().client(
        "s3",
        endpoint_url=endpoint,
        region_name="auto",
        aws_access_key_id=key_id,
        aws_secret_access_key=secret,
        config=botocore.config.Config(
            signature_version="s3v4",
            s3={"addressing_style": "path"},
            retries={"max_attempts": 3, "mode": "standard"},
            connect_timeout=10,
            read_timeout=60,
            request_checksum_calculation="when_required",
            response_checksum_validation="when_required",
        ),
    )


def _require_safe_endpoint(endpoint: str, variable: str) -> None:
    parts = urlsplit(endpoint)
    if parts.scheme == "https" and parts.hostname:
        return
    if parts.scheme == "http" and parts.hostname in _LOOPBACK_HOSTS:
        return
    raise UsageError(f"{variable} must be an https:// URL (plain http is only allowed for a loopback host)")


def _code(exc: botocore.exceptions.ClientError) -> str:
    return str(exc.response.get("Error", {}).get("Code", ""))


def _failure(operation: str, key: str, exc: Exception) -> PublishFailed:
    detail = _code(exc) if isinstance(exc, botocore.exceptions.ClientError) else type(exc).__name__
    return PublishFailed(f"{operation} {key} failed: {detail}", operation=operation, key=key, s3Error=detail)
