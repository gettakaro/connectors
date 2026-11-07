"""Image operations a recipe layer may name. Each takes RGBA pixels and returns RGBA pixels."""

from __future__ import annotations

from collections.abc import Callable
from typing import Any

from PIL import Image

from ..exit_codes import BuildFailed


def trim(image: Image.Image, *, code: str) -> Image.Image:
    """Crop to the bounding box of the non-transparent pixels."""
    box = image.getchannel("A").getbbox()
    if box is None:
        raise BuildFailed(f"recipe {code!r}: trim found a fully transparent image", recipe=code)
    return image.crop(box)


OPS: dict[str, Callable[..., Image.Image]] = {"trim": trim}


def apply(operation: dict[str, Any], image: Image.Image, *, code: str) -> Image.Image:
    return OPS[operation["op"]](image, code=code)
