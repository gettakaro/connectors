"""A throwaway certificate authority for a fake Takaro that speaks ``wss://``.

Some connectors refuse a plaintext Takaro on principle (Enshrouded's native connector only
accepts ``wss://`` and validates the chain and the host name), so a local verification run
has to offer TLS they can be told to trust. The CA and the server certificate live for one
run only: they are made in a private temporary directory, the CA's public half is what the
connector is given, and nothing here ever reaches a report or a kept log.

``openssl`` is the only tool used, because the maintenance environment carries no crypto
package and every host that runs ``takaro-maint verify`` already has it.
"""

from __future__ import annotations

import shutil
import ssl
import subprocess
from dataclasses import dataclass
from pathlib import Path

from ..exit_codes import UsageError


@dataclass(frozen=True)
class FakeTakaroPki:
    """What a run needs from its throwaway PKI."""

    #: The CA certificate the connector has to trust (public; safe to copy into the server tree).
    ca_pem: Path
    #: The server side of the fake Takaro, loaded with the certificate for ``hostname``.
    context: ssl.SSLContext
    hostname: str


def make_test_pki(directory: Path, hostname: str) -> FakeTakaroPki:
    """A CA and one server certificate for ``hostname``, valid for two days."""
    if shutil.which("openssl") is None:
        raise UsageError("openssl is required to give the fake Takaro a TLS certificate")
    directory.mkdir(parents=True, exist_ok=True)
    directory.chmod(0o700)
    ca_key, ca_pem = directory / "ca.key", directory / "ca.pem"
    key, csr, pem, ext = (directory / name for name in ("server.key", "server.csr", "server.pem", "server.ext"))
    ext.write_text(f"subjectAltName=DNS:{hostname}\nextendedKeyUsage=serverAuth\n", encoding="utf-8")
    ca_subject = "/CN=takaro-maint verify test CA"
    ca_ext = ["-addext", "basicConstraints=critical,CA:TRUE", "-addext", "keyUsage=critical,keyCertSign,cRLSign"]
    steps = [
        ["req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", str(ca_key), "-out", str(ca_pem)]
        + ["-days", "2", "-subj", ca_subject, *ca_ext],
        ["req", "-newkey", "rsa:2048", "-nodes", "-keyout", str(key), "-out", str(csr), "-subj", f"/CN={hostname}"],
        ["x509", "-req", "-in", str(csr), "-CA", str(ca_pem), "-CAkey", str(ca_key), "-CAcreateserial"]
        + ["-out", str(pem), "-days", "2", "-extfile", str(ext)],
    ]
    for step in steps:
        completed = subprocess.run(["openssl", *step], capture_output=True, text=True, check=False)
        if completed.returncode != 0:
            raise UsageError(f"openssl could not make the test certificate: {completed.stderr.strip()[:300]}")
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(str(pem), str(key))
    return FakeTakaroPki(ca_pem=ca_pem, context=context, hostname=hostname)
