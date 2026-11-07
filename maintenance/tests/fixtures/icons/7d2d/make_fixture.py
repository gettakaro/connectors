#!/usr/bin/env python3
"""Regenerate the synthetic 7 Days to Die server depot under ``depots/294422/<manifest>/tree``.

Run by hand (``python make_fixture.py``); a test regenerates everything into a temporary directory and
requires it to match the committed bytes. Nothing here is game art or game text: every icon is a solid,
uniquely coloured square (written uncompressed, so the bytes do not depend on zlib), and the XML and
localization rows are hand-written in the shape of the game's files with invented names.
"""

from __future__ import annotations

import hashlib
import io
import sys
from pathlib import Path

from PIL import Image

HERE = Path(__file__).parent
MANIFEST = "7440000000000000745"
TREE = Path("depots") / "294422" / MANIFEST / "tree"
BOM = b"\xef\xbb\xbf"

TINT_BASE = "tintBase"
TINT_BASE_GREY = (128, 128, 128)
HALF_ALPHA = 128
BIG = "bigIcon"
BIG_SIZE = 160

# Every icon the fixture ships, besides tintBase and bigIcon, whose pixels are described above.
ICON_STEMS = [
    "iconA",
    "iconB",
    "iconParent",
    "itemNameFallback",
    "childExcludesIcon",
    "childExcludesBoth",
    "itemNestedDecoy",
    "itemOrphan",
    "itemCycleA",
    "itemCycleB",
    "modByName",
    "blockArt",
    "blockNameFallback",
    "blockExcludes",
    "blockExtendsItem",
]


def colour(stem: str) -> tuple[int, int, int]:
    """A stable, distinct colour per icon name, kept away from black and white."""
    digest = hashlib.sha256(stem.encode("utf-8")).digest()
    return tuple(40 + digest[index] % 176 for index in range(3))  # type: ignore[return-value]


def png_bytes(image: Image.Image) -> bytes:
    buffer = io.BytesIO()
    image.save(buffer, format="PNG", compress_level=0)
    return buffer.getvalue()


def solid(rgb: tuple[int, int, int], size: int = 16) -> Image.Image:
    return Image.new("RGBA", (size, size), (*rgb, 255))


def tint_base() -> Image.Image:
    image = solid(TINT_BASE_GREY)
    image.paste((*TINT_BASE_GREY, HALF_ALPHA), (8, 0, 16, 16))
    return image


def icons() -> dict[str, bytes]:
    files = {f"{stem}.png": png_bytes(solid(colour(stem))) for stem in ICON_STEMS}
    files[f"{TINT_BASE}.png"] = png_bytes(tint_base())
    files[f"{BIG}.png"] = png_bytes(solid(colour(BIG), BIG_SIZE))
    return files


ITEMS_XML = """<?xml version="1.0" encoding="UTF-8"?>
<items>
\t<item name="itemOwnIcon">
\t\t<property name="CustomIcon" value="iconA"/>
\t</item>
\t<item name="itemNameFallback">
\t\t<property name="Material" value="Mmetal"/>
\t</item>
\t<item name="parentTinted">
\t\t<property name="CustomIcon" value="iconParent"/>
\t\t<property name="CustomIconTint" value="ff0000"/>
\t</item>
\t<item name="childInherits">
\t\t<property name="Extends" value="parentTinted"/>
\t</item>
\t<item name="grandchild">
\t\t<property name="Extends" value="childInherits"/>
\t</item>
\t<item name="childExcludesIcon">
\t\t<property name="Extends" value="parentTinted" param1="CustomIcon"/>
\t</item>
\t<item name="childExcludesBoth">
\t\t<property name="Extends" value="parentTinted" param1="CustomIcon, CustomIconTint"/>
\t</item>
\t<item name="childOverrides">
\t\t<property name="Extends" value="parentTinted"/>
\t\t<property name="CustomIcon" value="iconB"/>
\t</item>
\t<item name="tintHex">
\t\t<property name="CustomIcon" value="tintBase"/>
\t\t<property name="CustomIconTint" value="80FF40"/>
\t</item>
\t<item name="tintRgb">
\t\t<property name="CustomIcon" value="tintBase"/>
\t\t<property name="CustomIconTint" value="128, 255,64"/>
\t</item>
\t<item name="tintRgba">
\t\t<property name="CustomIcon" value="tintBase"/>
\t\t<property name="CustomIconTint" value="128,255,64,200"/>
\t</item>
\t<item name="tintHexAlpha">
\t\t<property name="CustomIcon" value="tintBase"/>
\t\t<property name="CustomIconTint" value="#80ff40cc"/>
\t</item>
\t<item name="tintWhite">
\t\t<property name="CustomIcon" value="tintBase"/>
\t\t<property name="CustomIconTint" value="ffffff"/>
\t</item>
\t<item name="tintNone">
\t\t<property name="CustomIcon" value="tintBase"/>
\t</item>
\t<item name="tintBad">
\t\t<property name="CustomIcon" value="tintBase"/>
\t\t<property name="CustomIconTint" value="notacolour"/>
\t</item>
\t<item name="itemMissingIcon">
\t\t<property name="CustomIcon" value="nope"/>
\t</item>
\t<item name="itemCaseIcon">
\t\t<property name="CustomIcon" value="ICONA"/>
\t</item>
\t<item name="itemNestedDecoy">
\t\t<property class="Action0">
\t\t\t<property name="CustomIcon" value="decoy"/>
\t\t</property>
\t</item>
\t<item name="itemOwnIcon">
\t\t<property name="CustomIcon" value="iconB"/>
\t</item>
\t<item name="itemOrphan">
\t\t<property name="Extends" value="noSuchParent"/>
\t</item>
\t<item name="itemCycleA">
\t\t<property name="Extends" value="itemCycleB"/>
\t</item>
\t<item name="itemCycleB">
\t\t<property name="Extends" value="itemCycleA"/>
\t</item>
\t<item name="itemBigIcon">
\t\t<property name="CustomIcon" value="bigIcon"/>
\t</item>
\t<item name="itemUnlocalized">
\t\t<property name="CustomIcon" value="iconA"/>
\t</item>
\t<item name="itemKeyAsValue">
\t\t<property name="CustomIcon" value="iconA"/>
\t</item>
\t<item name="itemEmptyEnglish">
\t\t<property name="CustomIcon" value="iconA"/>
\t</item>
</items>
"""

MODIFIERS_XML = """<?xml version="1.0" encoding="UTF-8"?>
<item_modifiers>
\t<item_modifier name="modByName" installable_tags="melee">
\t\t<property name="Material" value="Mmetal"/>
\t</item_modifier>
\t<item_modifier name="modUnlocalized">
\t\t<property name="CustomIcon" value="iconA"/>
\t</item_modifier>
</item_modifiers>
"""

BLOCKS_XML = """<?xml version="1.0" encoding="UTF-8"?>
<blocks>
\t<block name="blockOwnIcon">
\t\t<property name="CustomIcon" value="blockArt"/>
\t</block>
\t<block name="blockNameFallback">
\t\t<property name="Material" value="Mmetal"/>
\t</block>
\t<block name="blockTinted">
\t\t<property name="CustomIcon" value="blockArt"/>
\t\t<property name="CustomIconTint" value="#00ff00"/>
\t</block>
\t<block name="blockExcludes">
\t\t<property name="Extends" value="blockTinted" param1="CustomIcon"/>
\t</block>
\t<block name="blockExtendsItem">
\t\t<property name="Extends" value="parentTinted"/>
\t</block>
\t<block name="itemNameFallback">
\t\t<property name="CustomIcon" value="blockArt"/>
\t</block>
\t<block name="blockUnlocalized">
\t\t<property name="Material" value="Mmetal"/>
\t</block>
</blocks>
"""

LOCALIZED = [
    "itemOwnIcon",
    "itemNameFallback",
    "parentTinted",
    "childInherits",
    "grandchild",
    "childExcludesIcon",
    "childExcludesBoth",
    "childOverrides",
    "tintHex",
    "tintRgb",
    "tintRgba",
    "tintHexAlpha",
    "tintWhite",
    "tintNone",
    "tintBad",
    "itemMissingIcon",
    "itemCaseIcon",
    "itemNestedDecoy",
    "itemOrphan",
    "itemCycleA",
    "itemCycleB",
    "itemBigIcon",
    "modByName",
    "blockOwnIcon",
    "blockNameFallback",
    "blockTinted",
    "blockExcludes",
    "blockExtendsItem",
]
QUOTED_NAME = "Quoted, name\nsecond line"
LOCALIZATION_HEADER = "Key,File,Type,UsedInMainMenu,NoTranslate,english,Context / Alternate Text,german"


def localization_csv() -> str:
    rows = [LOCALIZATION_HEADER]
    for key in LOCALIZED:
        english = f'"{QUOTED_NAME}"' if key == "itemOwnIcon" else f"Fixture {key}"
        rows.append(f"{key},items,Item,,,{english},,Fixture {key} de")
    rows.append("itemKeyAsValue,items,Item,,,itemKeyAsValue,,")
    rows.append("itemEmptyEnglish,items,Item,,,,,Nur Deutsch")
    return "\n".join(rows) + "\n"


def texts() -> dict[str, bytes]:
    return {
        "Data/Config/items.xml": BOM + ITEMS_XML.encode("utf-8"),
        "Data/Config/item_modifiers.xml": BOM + MODIFIERS_XML.encode("utf-8"),
        "Data/Config/blocks.xml": BOM + BLOCKS_XML.encode("utf-8"),
        "Data/Config/Localization.csv": BOM + localization_csv().encode("utf-8"),
        # Decoys the file filter must leave out.
        "Data/Config/notes.xml": b"<notes/>\n",
        "Data/ItemIcons/readme.txt": b"not an icon\n",
    }


def files() -> dict[str, bytes]:
    found = {f"Data/ItemIcons/{name}": payload for name, payload in icons().items()}
    found["Data/ItemIcons/sub/nested.png"] = png_bytes(solid(colour("nested")))
    found.update(texts())
    return found


def write(root: Path) -> None:
    for relative, payload in files().items():
        path = root / TREE / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(payload)
    head = root / "depots" / "head.json"
    head.parent.mkdir(parents=True, exist_ok=True)
    head.write_text(f'{{"294422": "{MANIFEST}"}}\n', encoding="utf-8")


if __name__ == "__main__":
    write(Path(sys.argv[1]) if len(sys.argv) > 1 else HERE)
