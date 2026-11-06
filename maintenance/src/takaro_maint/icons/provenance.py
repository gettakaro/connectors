"""What a catalog build records about its inputs, and how the client build compares with the target."""

from __future__ import annotations

import platform

from .. import __version__
from ..steam import depotdownloader as dd
from . import build
from .decoders import pz_texturepack


def client_status(
    buildid: int | None, revision: str, client_builds: dict[str, list[int]]
) -> tuple[str, list[int], str | None]:
    """``(status, expected ids, warning)``: how the installed client's build compares with the target's release.

    Steam build ids only grow, so a build id below every id known for the release belongs to an older
    client and one above them to a newer one.
    """
    expected = list(client_builds.get(revision, []))
    if buildid is None:
        return "unknown", expected, "no client build id could be read (no appmanifest next to the client files)"
    elsewhere = sorted(rev for rev, ids in client_builds.items() if rev != revision and buildid in ids)
    where = f" (it is listed for {', '.join(elsewhere)})" if elsewhere else ""
    if buildid in expected:
        return "match", expected, None
    if not expected:
        return (
            "unknown",
            expected,
            f"client build {buildid} cannot be compared: release {revision} lists no client builds{where}",
        )
    status = "older" if buildid < min(expected) else "newer"
    return (
        status,
        expected,
        f"client build {buildid} is {status} than the {revision} client builds {expected}{where}; "
        "the pixels may not match the server data",
    )


def tools_record() -> dict[str, str]:
    encoder = build.encoder_description()
    return {
        "takaroMaint": __version__,
        "python": platform.python_version(),
        "depotdownloader": dd.read_lock().version,
        "pillow": encoder["pillow"],
        "libwebp": encoder["libwebp"],
        "decoder": pz_texturepack.DECODER_DESCRIPTION,
    }
