#!/usr/bin/env python3
"""Generate the synthetic Terraria fixtures: a connector catalogue, a client tree and the decoder stand-ins' outputs.

Nothing here is game data. The item names are invented, the .xnb files are a few synthetic bytes the decoder stand-in
never parses, the pictures are small strips of flat colours, and the "decompiled" C# is hand-written in the shape the
mapper reads. Only the numeric code 5644 is a real item id, because a rule of the mapper names it.
The client tree is built by ``write_client`` at test time and is not committed.
``python make_fixture.py`` rewrites the committed files; ``test_cli_icons_terraria.py`` regenerates them into a
temporary directory and requires byte equality, so the JSON, the pictures and the expectations change together.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path
from typing import Any

from PIL import Image

HERE = Path(__file__).parent
BUILD_ID = 24893155
MANIFEST = "2046220459945595868"
IMAGES = "Content/Images"
ASSEMBLY = "Terraria.exe"
GAP = 2
BODY = 5
WIDTH = 6

# code -> (name, kind); kinds drive the pictures, the registrations and the client files.
ITEMS: dict[str, tuple[str, str]] = {
    "1": ("Synth Plain", "plain"),
    "2": ("Synth Strip", "strip4"),
    "3": ("Synth Snack", "food"),
    "4": ("Synth Twin A", "twin"),
    "5": ("Synth Twin B", "twin"),
    "10": ("Alpha Chest", "chest"),
    "11": ("Fake Alpha Chest", "none"),
    "12": ("Fake Beta Chest", "own-chest"),
    "13": ("Beta Chest", "chest-b"),
    "14": ("Fake Chest", "none"),
    "15": ("Fake newchest1", "none"),
    "16": ("Synth Odd Format", "bad-format"),
    "17": ("Synth Glass", "glass"),
    "5644": ("Synth Orb", "orb9"),
}
CLIENT_ONLY = {"99": "plain"}
DECOYS = {
    f"{IMAGES}/Item_7.png": b"fixture decoy png\n",
    f"{IMAGES}/Projectile_1.xnb": b"fixture decoy projectile\n",
    f"{IMAGES}/sub/Item_3.xnb": b"fixture decoy nested\n",
    f"{IMAGES}/Item_x.xnb": b"fixture decoy name\n",
    f"{IMAGES}/Item_8.xnb.bak": b"fixture decoy backup\n",
}
GLASS_PIXEL = (3, 3)
GLASS_COLOR = (200, 100, 50, 128)
STRIP_FRAMES = {"2": 4, "3": 3, "5644": 9}
FRAME_ASSEMBLY = {"name": "SynthGame", "version": "1.2.3.4", "mvid": "11111111-2222-3333-4444-555555555555"}


def frame_color(frame: int, seed: int = 0) -> tuple[int, int, int, int]:
    return (30 + 25 * frame + seed, 220 - 20 * frame, 60 + 10 * frame, 255)


def strip_image(frames: int, seed: int = 0) -> Image.Image:
    """A vertical strip of ``frames`` frames, each ``BODY`` rows of one colour and ``GAP`` transparent rows."""
    image = Image.new("RGBA", (WIDTH, frames * (BODY + GAP)))
    for frame in range(frames):
        image.paste(frame_color(frame, seed), (0, frame * (BODY + GAP), WIDTH, frame * (BODY + GAP) + BODY))
    return image


def expected_first_frame(frames: int, seed: int = 0) -> Image.Image:
    return Image.new("RGBA", (WIDTH, BODY), frame_color(0, seed))


def flat(color: tuple[int, int, int, int], marker: tuple[int, int, int, int]) -> Image.Image:
    image = Image.new("RGBA", (8, 8), color)
    image.putpixel((1, 2), marker)
    return image


def picture(code: str, kind: str) -> Image.Image | None:
    if kind == "plain":
        return flat((40, 90, 200, 255), (255, 255, 255, 255))
    if kind == "twin":
        return flat((200, 60, 60, 255), (0, 0, 0, 255))
    if kind == "chest":
        return flat((150, 110, 40, 255), (255, 255, 0, 255))
    if kind == "chest-b":
        return flat((120, 40, 150, 255), (255, 255, 0, 255))
    if kind == "own-chest":
        return flat((20, 160, 80, 255), (255, 0, 255, 255))
    if kind == "glass":
        image = flat((90, 90, 90, 255), (255, 255, 255, 255))
        image.putpixel(GLASS_PIXEL, GLASS_COLOR)
        return image
    if kind.startswith("strip"):
        return strip_image(STRIP_FRAMES[code], seed=1)
    if kind == "food":
        return strip_image(STRIP_FRAMES[code], seed=2)
    if kind == "orb9":
        return strip_image(STRIP_FRAMES[code], seed=3)
    return None


def catalogue_text() -> str:
    entries = [
        {"code": code, "name": name, "amount": 1, "quality": "0", "aliases": [name.replace(" ", "")]}
        for code, (name, _) in sorted(ITEMS.items(), key=lambda item: int(item[0]))
    ]
    body = json.dumps(entries, indent=2)
    return (
        "export interface TerrariaItemCatalogEntry {\n  code: string;\n  name: string;\n  amount: number;\n"
        "  quality: string;\n  aliases: string[];\n}\n\n"
        f"export const TERRARIA_ITEM_CATALOG: readonly TerrariaItemCatalogEntry[] = {body} as const;\n\n"
        "export function listTerrariaItems(): TerrariaItemCatalogEntry[] {\n"
        "  return TERRARIA_ITEM_CATALOG.map((item) => ({ ...item, aliases: [...item.aliases] }));\n}\n"
    )


def appmanifest_text() -> str:
    return "\n".join(
        [
            '"AppState"',
            "{",
            '\t"appid"\t\t"105600"',
            f'\t"buildid"\t\t"{BUILD_ID}"',
            '\t"InstalledDepots"',
            "\t{",
            '\t\t"105601"',
            "\t\t{",
            f'\t\t\t"manifest"\t\t"{MANIFEST}"',
            "\t\t}",
            "\t}",
            "}",
            "",
        ]
    )


def client_files() -> dict[str, bytes]:
    files = {"client/steamapps/appmanifest_105600.acf": appmanifest_text().encode()}
    root = "client/steamapps/common/Terraria"
    files[f"{root}/{ASSEMBLY}"] = b"fixture assembly\n"
    for code, (_, kind) in ITEMS.items():
        if kind != "none":
            files[f"{root}/{IMAGES}/Item_{code}.xnb"] = f"fixture xnb {code}\n".encode()
    for code in CLIENT_ONLY:
        files[f"{root}/{IMAGES}/Item_{code}.xnb"] = f"fixture xnb {code}\n".encode()
    files.update({f"{root}/{name}": payload for name, payload in DECOYS.items()})
    return files


def world_source(*, extra: str = "") -> str:
    return (
        "namespace Synth;\n\n"
        "public class World\n{\n"
        "\tpublic static void RegisterItemAnimation(int index, DrawAnimation animation)\n\t{\n"
        "\t\titemAnimations[index] = animation;\n\t}\n\n"
        "\tpublic static void InitializeItemAnimations()\n\t{\n"
        "\t\tRegisterItemAnimation(2, new DrawAnimationVertical(6, 4));\n"
        "\t\tfor (int j = 0; j < ItemID.Sets.IsFood.Length; j++)\n\t\t{\n"
        "\t\t\tif (ItemID.Sets.IsFood[j])\n\t\t\t{\n"
        "\t\t\t\tRegisterItemAnimation(j, new DrawAnimationVertical(int.MaxValue, 3));\n\t\t\t}\n\t\t}\n"
        "\t\tRegisterItemAnimation(5644, new DrawAnimationScryingOrb\n\t\t{\n"
        "\t\t\tTicksPerFrame = 7,\n\t\t\tFrameCount = 9\n\t\t});\n"
        f"{extra}"
        "\t}\n}\n"
    )


SETS_SOURCE = (
    "namespace Synth.ID;\n\n"
    "public class ItemID\n{\n\tpublic static class Sets\n\t{\n"
    "\t\tpublic static bool[] IsFood = Factory.CreateBoolSet(3);\n\t}\n}\n"
)
WORLD_TYPE = "Synth.World"
SETS_TYPE = "Terraria.ID.ItemID+Sets"
REGISTER_MEMBER = "Terraria.Main::RegisterItemAnimation"


def _json(value: Any) -> bytes:
    return (json.dumps(value, indent=2) + "\n").encode("utf-8")


def _png(image: Image.Image, path: Path) -> bytes:
    image.save(path, format="PNG", compress_level=0)
    return path.read_bytes()


def decoder_files(scratch: Path) -> dict[str, bytes]:
    """Every file under ``decoder/`` by relative path; ``scratch`` is where PNG bytes are rendered."""
    scratch.mkdir(parents=True, exist_ok=True)
    files: dict[str, bytes] = {}
    index: dict[str, Any] = {}

    def add_json(key: str, name: str, value: Any) -> None:
        files[f"json/{name}.json"] = _json(value)
        index[key] = {"file": f"json/{name}.json"}

    add_json(f"assembly-info:{ASSEMBLY}", "assembly-info", FRAME_ASSEMBLY)
    add_json(
        f"callers:{REGISTER_MEMBER}",
        "callers",
        [{"type": WORLD_TYPE, "method": "InitializeItemAnimations", "count": 3}],
    )
    add_json(f"decompile-type:{WORLD_TYPE}", "decompile-world", {"type": WORLD_TYPE, "source": world_source()})
    add_json(f"decompile-type:{SETS_TYPE}", "decompile-sets", {"type": SETS_TYPE, "source": SETS_SOURCE})

    def add_texture(code: str, image: Image.Image, name: str) -> None:
        files[f"png/{name}.png"] = _png(image, scratch / f"{name}.png")
        index[f"texture:{IMAGES}/Item_{code}.xnb"] = {
            "file": f"png/{name}.png",
            "meta": {
                "width": image.width,
                "height": image.height,
                "format": "Color",
                "xnbVersion": 5,
                "platform": "w",
                "compressed": True,
            },
        }

    for code, (_, kind) in ITEMS.items():
        if kind == "bad-format":
            index[f"texture:{IMAGES}/Item_{code}.xnb"] = {
                "error": f"{IMAGES}/Item_{code}.xnb has SurfaceFormat 4",
                "errorKind": "unsupported-format",
            }
            continue
        image = picture(code, kind)
        if image is not None:
            add_texture(code, image, f"it{code}")
    for code, kind in CLIENT_ONLY.items():
        add_texture(code, picture(code, kind) or flat((1, 2, 3, 255), (4, 5, 6, 255)), f"it{code}")
    files["index.json"] = _json(index)
    return files


def all_files(scratch: Path) -> dict[str, bytes]:
    files = {f"decoder/{name}": payload for name, payload in decoder_files(scratch).items()}
    files["connector/itemCatalog.ts"] = catalogue_text().encode("utf-8")
    return files


def write_client(root: Path) -> None:
    """The synthetic client tree (``steamapps/...``) under ``root``; tests build it in a temporary directory.

    It is not committed: it holds ``.xnb`` and ``.exe`` stand-ins, and a committed file with those names invites
    mistaking it for game data.
    """
    for name, payload in client_files().items():
        target = root / name.removeprefix("client/")
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(payload)


def write(root: Path, scratch: Path) -> None:
    for name, payload in all_files(scratch).items():
        target = root / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(payload)


if __name__ == "__main__":
    import tempfile

    with tempfile.TemporaryDirectory() as temporary:
        write(Path(sys.argv[1]) if len(sys.argv) > 1 else HERE, Path(temporary))
