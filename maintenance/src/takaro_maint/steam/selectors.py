"""File-list selectors, shared by every command that downloads part of a depot."""

from __future__ import annotations

import re


def selects(relative: str, selectors: list[str]) -> bool:
    """Whether a selector names this file by exact path or case-insensitive regex.

    DepotDownloader writes its own bookkeeping (the depot manifest and its checksum) into
    the download directory alongside the files it fetched, so what came out of it is
    filtered by the same selectors that went in rather than taken wholesale.
    """
    for selector in selectors:
        if selector.startswith("regex:"):
            if re.search(selector[len("regex:") :], relative, re.IGNORECASE):
                return True
        elif relative.casefold() == selector.casefold():
            return True
    return False
