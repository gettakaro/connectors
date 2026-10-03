#!/usr/bin/env python3
"""Candidate code sites that reference a data address in a PE image, for tools/sigderive.py --xref.

On the Windows server the data anchors (GUObjectArray.ObjObjects, the FNamePool block table) are
reached with RIP-relative ModRM operands (mod=00 rm=101). This lists every .text position whose
disp32 resolves to the target and writes candidate instruction starts (field - 2/3/4) in the
"<hex>: ..." form sigderive reads. Addresses are VAs at the preferred ImageBase (0x140000000), as
sigderive uses them; the runtime anchors from platform/windows/anchor_scan are RVAs (add the base).

  pe_xref_sites.py ConanSandboxServer-Win64-Shipping.exe 0x14a92cdc0 > sites.txt
  sigderive.py --binary ConanSandboxServer-Win64-Shipping.exe --xref 0x14a92cdc0 --sites sites.txt --max-sites 3000
"""
import re
import struct
import sys


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    d = open(sys.argv[1], "rb").read()
    target = int(sys.argv[2], 0)
    pe, = struct.unpack_from("<I", d, 0x3C)
    nsec, = struct.unpack_from("<H", d, pe + 6)
    osz, = struct.unpack_from("<H", d, pe + 20)
    base, = struct.unpack_from("<Q", d, pe + 24 + 24)
    for i in range(nsec):
        o = pe + 24 + osz + 40 * i
        if d[o:o + 8].rstrip(b"\0") != b".text":
            continue
        vsize, va, rawsize, rawoff = struct.unpack_from("<IIII", d, o + 8)
        text, ta = d[rawoff:rawoff + rawsize], base + va
        n = 0
        for m in re.finditer(rb"[\x05\x0d\x15\x1d\x25\x2d\x35\x3d]", text):
            p = m.start() + 1
            if p + 4 > len(text):
                break
            v, = struct.unpack_from("<i", text, p)
            if ta + p + 4 + v == target:
                n += 1
                for k in (3, 2, 4):
                    print("%x: site of 0x%x" % (ta + p - k, target))
        print("%d references to 0x%x" % (n, target), file=sys.stderr)


if __name__ == "__main__":
    main()
