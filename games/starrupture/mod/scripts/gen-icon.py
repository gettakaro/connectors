#!/usr/bin/env python3
"""Generate src/game_icon.{h,cpp} from assets/icon-256.png. Run from the mod folder's parent."""
import base64
import hashlib
import pathlib

root = pathlib.Path(__file__).resolve().parent.parent
png = (root.parent / "assets" / "icon-256.png").read_bytes()
encoded = base64.b64encode(png).decode()
lines = "\n".join('    "%s"' % encoded[i:i + 100] for i in range(0, len(encoded), 100))
revision = hashlib.sha256(png).hexdigest()[:16]
(root / "src" / "game_icon.h").write_text(
    "#pragma once\n#include <cstddef>\n\n// Generated from assets/icon-256.png by scripts/gen-icon.py.\n"
    "const char* TakaroGameIconRevision();\nsize_t TakaroGameIconSize();\nconst char* TakaroGameIconBase64();\n")
(root / "src" / "game_icon.cpp").write_text(
    '#include "game_icon.h"\n\nnamespace\n{\nconstexpr char Base64[] =\n%s;\n}\n\n'
    'const char* TakaroGameIconRevision() { return "%s"; }\n'
    "size_t TakaroGameIconSize() { return %d; }\n"
    "const char* TakaroGameIconBase64() { return Base64; }\n" % (lines, revision, len(png)))
