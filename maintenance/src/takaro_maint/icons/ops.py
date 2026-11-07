"""Image operations a recipe layer may name. Each takes RGBA pixels and returns RGBA pixels."""

from __future__ import annotations

from collections.abc import Callable
from typing import Any

from PIL import Image, ImageChops

from ..exit_codes import BuildFailed


def trim(image: Image.Image, *, code: str) -> Image.Image:
    """Crop to the bounding box of the non-transparent pixels."""
    box = image.getchannel("A").getbbox()
    if box is None:
        raise BuildFailed(f"recipe {code!r}: trim found a fully transparent image", recipe=code)
    return image.crop(box)


def tint_multiply(image: Image.Image, *, code: str, color: str) -> Image.Image:
    """Multiply the colour channels by ``color`` (``#rrggbb``), the way a sprite is coloured by a tint.

    Straight (non-premultiplied) RGB: ``out = c * t // 255`` per channel (Pillow's ``ImageChops.multiply``
    floors), alpha unchanged. This is NGUI's texture-times-colour with a colour alpha of 1.
    Game syntaxes are the mappers' business; the op only reads the canonical form.
    """
    red, green, blue = (int(color[position : position + 2], 16) for position in (1, 3, 5))
    return ImageChops.multiply(image, Image.new("RGBA", image.size, (red, green, blue, 255)))


OPS: dict[str, Callable[..., Image.Image]] = {"trim": trim, "tint-multiply": tint_multiply}


def apply(operation: dict[str, Any], image: Image.Image, *, code: str) -> Image.Image:
    parameters = {key: value for key, value in operation.items() if key != "op"}
    return OPS[operation["op"]](image, code=code, **parameters)


def composite_over(images: list[Image.Image]) -> Image.Image:
    """Layers combined in order, the first at the bottom, on the first layer's canvas.

    A layer of another size is resized to the canvas with LANCZOS on premultiplied pixels, the way the
    builder downscales. A single layer comes back untouched.
    """
    canvas = images[0]
    for layer in images[1:]:
        if layer.size != canvas.size:
            layer = layer.convert("RGBa").resize(canvas.size, Image.Resampling.LANCZOS).convert("RGBA")
        canvas = Image.alpha_composite(canvas, layer)
    return canvas
