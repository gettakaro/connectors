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


PACKING_ROTATIONS = ("none", "flip-horizontal", "flip-vertical", "rotate-180", "rotate-90")

# Undoing a Unity sprite's packing rotation, in top-down image space. UnityPy applies these to the bottom-up cut
# (SpriteHelper.get_image_from_sprite) and flips the result at the end; conjugating by that vertical flip leaves a
# vertical flip, a horizontal flip and a 180 degree turn as they are and swaps the two quarter turns.
_UNPACK = {
    "none": None,
    "flip-horizontal": Image.Transpose.FLIP_LEFT_RIGHT,
    "flip-vertical": Image.Transpose.FLIP_TOP_BOTTOM,
    "rotate-180": Image.Transpose.ROTATE_180,
    "rotate-90": Image.Transpose.ROTATE_90,
}


def atlas_cut(
    image: Image.Image,
    *,
    code: str,
    rect: list[float],
    rotation: str,
    canvas: list[float] | None = None,
    offset: list[float] | None = None,
) -> Image.Image:
    """Cut one Unity sprite out of its atlas.

    The layer image is the atlas (``W x H``, top row first). ``rect`` is the sprite's ``textureRect`` in Unity texels
    with the origin at the bottom-left; edges are rounded half to even (Python ``round``, which is what Pillow's
    float crop does in UnityPy), and a rect outside the atlas is a failed build.

    The cut is ``image.crop((l, H - t, r, H - b))`` for ``l = round(x)``, ``r = round(x + w)``, ``b = round(y)``,
    ``t = round(y + h)``. ``rotation`` is the sprite's packing rotation, undone with PIL transposes: none (nothing),
    flip-horizontal (FLIP_LEFT_RIGHT), flip-vertical (FLIP_TOP_BOTTOM), rotate-180 (ROTATE_180) and rotate-90
    (ROTATE_90; UnityPy's ROTATE_270 in bottom-up space).

    With ``canvas`` (the sprite's full ``rect`` size) and ``offset`` (its ``textureRectOffset``, origin bottom-left)
    the cut is pasted onto a transparent canvas of ``(round(cw), round(ch))`` at ``left = round(ox)``,
    ``top = round(ch) - round(oy) - cut height``: the frame the game draws a trimmed sprite in. Nothing is resampled.
    """
    width, height = image.size
    left, right = round(rect[0]), round(rect[0] + rect[2])
    bottom, top = round(rect[1]), round(rect[1] + rect[3])
    if not (0 <= left < right <= width and 0 <= bottom < top <= height):
        raise BuildFailed(f"recipe {code!r}: atlas-cut rect {rect} is outside the {width}x{height} atlas", recipe=code)
    cut = image.crop((left, height - top, right, height - bottom))
    transpose = _UNPACK[rotation]
    if transpose is not None:
        cut = cut.transpose(transpose)
    if canvas is None or offset is None:
        return cut
    canvas_width, canvas_height = round(canvas[0]), round(canvas[1])
    paste_left, paste_top = round(offset[0]), canvas_height - round(offset[1]) - cut.height
    if (
        paste_left < 0
        or paste_top < 0
        or paste_left + cut.width > canvas_width
        or paste_top + cut.height > canvas_height
    ):
        raise BuildFailed(
            f"recipe {code!r}: atlas-cut of {cut.width}x{cut.height} at offset {offset} does not fit the "
            f"{canvas_width}x{canvas_height} canvas",
            recipe=code,
        )
    framed = Image.new("RGBA", (canvas_width, canvas_height))
    framed.paste(cut, (paste_left, paste_top))
    return framed


OPS: dict[str, Callable[..., Image.Image]] = {"trim": trim, "tint-multiply": tint_multiply, "atlas-cut": atlas_cut}


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
