"""The target fingerprint: one hash both Python and Gradle compute from the same record.

The canonical form is deliberately hand-rolled so the Kotlin twin in
``games/minecraft/mod/buildSrc`` can reproduce it byte for byte:
keys sorted recursively, no whitespace, integers only, minimal JSON string escaping,
non-ASCII left as-is and encoded as UTF-8.
"""

from __future__ import annotations

import hashlib
from typing import Any

FINGERPRINT_KEYS = ("id", "game", "platform", "revision", "inputs", "runtime", "build")

_SHORT_ESCAPES = {
    "\n": "\\n",
    "\r": "\\r",
    "\t": "\\t",
    "\b": "\\b",
    "\f": "\\f",
    '"': '\\"',
    "\\": "\\\\",
}


def _escape(text: str) -> str:
    out: list[str] = []
    for char in text:
        short = _SHORT_ESCAPES.get(char)
        if short is not None:
            out.append(short)
        elif ord(char) < 0x20:
            out.append(f"\\u{ord(char):04x}")
        else:
            out.append(char)
    return '"' + "".join(out) + '"'


def canonical(value: Any) -> str:
    """Canonical JSON text for ``value`` (see the module docstring)."""
    if value is None:
        return "null"
    if value is True:
        return "true"
    if value is False:
        return "false"
    if isinstance(value, str):
        return _escape(value)
    if isinstance(value, int):
        return str(value)
    if isinstance(value, float):
        raise TypeError("the catalog forbids floats; fingerprints are integer-only")
    if isinstance(value, dict):
        items = sorted(value.items(), key=lambda kv: kv[0])
        return "{" + ",".join(f"{_escape(str(k))}:{canonical(v)}" for k, v in items) + "}"
    if isinstance(value, (list, tuple)):
        return "[" + ",".join(canonical(item) for item in value) + "]"
    raise TypeError(f"cannot canonicalise {type(value).__name__}")


def subset(record: dict[str, Any]) -> dict[str, Any]:
    return {key: record[key] for key in FINGERPRINT_KEYS if key in record}


def fingerprint(record: dict[str, Any]) -> str:
    """The 64-hex fingerprint of a target record."""
    return hashlib.sha256(canonical(subset(record)).encode("utf-8")).hexdigest()


def fp16(record_or_fingerprint: dict[str, Any] | str) -> str:
    value = record_or_fingerprint if isinstance(record_or_fingerprint, str) else fingerprint(record_or_fingerprint)
    return value[:16]


def references_key(server: dict[str, Any], selectors: list[str]) -> str:
    """The identity of a reference-assembly directory: which depot manifests, which files of them.

    Deliberately not the fingerprint. A change to anything else in the target (a framework
    release, a runtime image) leaves these bytes the same, so it must not force a download
    Steam may no longer serve.
    """
    manifests = {str(depot): str(entry["manifest"]) for depot, entry in server["depots"].items()}
    identity = {"app": int(server["app"]), "manifests": manifests, "references": [str(s) for s in selectors]}
    return hashlib.sha256(canonical(identity).encode("utf-8")).hexdigest()[:16]
