#!/usr/bin/env python3
"""Derive the startup signatures (core/pins) for a Conan Exiles server binary and prove each
matches exactly once.

Extended from games/vein/mod/tools/sigderive.py (VEIN): ELF and PE, function anchors AND data
anchors. A data anchor (GUObjectArray.ObjObjects, the FNamePool block table) has no code of its
own, so its signature is an instruction that references it; the runtime reads the address out
of the matched bytes (a RIP-relative or an absolute 32-bit displacement).

  # a function: the pattern starts at the function
  sigderive.py --binary ConanSandboxServer-Linux-Shipping --addr 0x3f12340

  # a data address: try code sites that reference it (sites from `objdump -d | grep`)
  sigderive.py --binary ConanSandboxServer-Linux-Shipping --xref 0xc35a580 --sites sites.txt

  # check a pattern (as in core/pins/pins.cpp) and print what it resolves to
  sigderive.py --binary ConanSandboxServer-Linux-Shipping --check "48 8B 15 ?? ?? ?? ??" \
      --capture rip32 --offset 3

Wildcards: every 4-byte field that encodes a rel32 branch, or that points into the image as a
RIP-relative or absolute address, is masked, because those shift between builds even when the
code does not. A pattern is only usable when it matches exactly ONCE in .text; the library
refuses the build otherwise.
"""
import argparse
import re
import struct
import sys


class Image:
    def __init__(self, path):
        d = open(path, "rb").read()
        self.text = None
        self.ranges = []  # (lo, hi) address ranges of mapped data and code, for the address heuristic
        if d[:2] == b"MZ":
            pe_off, = struct.unpack_from("<I", d, 0x3C)
            nsec, = struct.unpack_from("<H", d, pe_off + 6)
            opt_size, = struct.unpack_from("<H", d, pe_off + 20)
            sec_off = pe_off + 24 + opt_size
            self.base, = struct.unpack_from("<Q", d, pe_off + 24 + 24)
            for i in range(nsec):
                o = sec_off + i * 40
                name = d[o:o + 8].rstrip(b"\0").decode()
                vsize, vaddr, rawsize, rawoff = struct.unpack_from("<IIII", d, o + 8)
                self.ranges.append((self.base + vaddr, self.base + vaddr + vsize))
                if name == ".text":
                    self.text = d[rawoff:rawoff + rawsize]
                    self.text_addr = self.base + vaddr
        elif d[:4] == b"\x7fELF":
            self.base = 0
            e_shoff, = struct.unpack_from("<Q", d, 0x28)
            e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", d, 0x3A)
            shstr_off, = struct.unpack_from("<Q", d, e_shoff + e_shstrndx * e_shentsize + 0x18)
            for i in range(e_shnum):
                o = e_shoff + i * e_shentsize
                sh_name, = struct.unpack_from("<I", d, o)
                sh_flags, = struct.unpack_from("<Q", d, o + 8)
                sh_addr, sh_off, sh_size = struct.unpack_from("<QQQ", d, o + 0x10)
                end = d.index(b"\0", shstr_off + sh_name)
                name = d[shstr_off + sh_name:end]
                if sh_flags & 2 and sh_addr:  # SHF_ALLOC
                    self.ranges.append((sh_addr, sh_addr + sh_size))
                if name == b".text":
                    self.text = d[sh_off:sh_off + sh_size]
                    self.text_addr = sh_addr
        else:
            raise SystemExit("%s is neither ELF nor PE" % path)
        if self.text is None:
            raise SystemExit("no .text section")

    def mapped(self, a):
        return any(lo <= a < hi for lo, hi in self.ranges)

    def off(self, addr):
        o = addr - self.text_addr
        if not 0 <= o < len(self.text):
            raise SystemExit("0x%x is not inside .text" % addr)
        return o


def mask_for(img, start_off, buf, keep=()):
    """True where the byte must be a wildcard."""
    mask = [False] * len(buf)
    i = 0
    while i < len(buf):
        b = buf[i]
        if b in (0xE8, 0xE9) and i + 5 <= len(buf):
            for k in range(i + 1, i + 5):
                mask[k] = True
            i += 5
            continue
        if b == 0x0F and i + 6 <= len(buf) and 0x80 <= buf[i + 1] <= 0x8F:
            for k in range(i + 2, i + 6):
                mask[k] = True
            i += 6
            continue
        i += 1
    # any 4-byte field that is an address inside the image (absolute, or RIP-relative with 0, 1
    # or 4 immediate bytes after it) moves between builds
    for i in range(len(buf) - 3):
        if any(mask[i:i + 4]) or i in keep:
            continue
        v, = struct.unpack_from("<i", buf, i)
        u = v & 0xFFFFFFFF
        here = img.text_addr + start_off + i
        hit = u >= 0x10000 and img.mapped(u)
        for extra in (0, 1, 4):
            hit = hit or (abs(v) >= 0x1000 and img.mapped(here + 4 + extra + v))
        if hit:
            for k in range(i, i + 4):
                mask[k] = True
    return mask


def pattern_text(buf, mask):
    return " ".join("??" if m else "%02X" % b for b, m in zip(buf, mask))


def to_regex(pattern):
    out = b""
    for p in pattern.split():
        out += b"." if p == "??" else re.escape(bytes([int(p, 16)]))
    return re.compile(out, re.DOTALL)


def matches(img, pattern, limit=3):
    hits = []
    for m in to_regex(pattern).finditer(img.text):
        hits.append(m.start())
        if len(hits) >= limit:
            break
    return hits


def resolve(img, hit_off, capture, offset):
    at = img.text_addr + hit_off
    if capture == "start":
        return at + offset
    field = hit_off + offset
    if capture == "rip32":
        v, = struct.unpack_from("<i", img.text, field)
        return img.text_addr + field + 4 + v
    if capture == "abs32":
        v, = struct.unpack_from("<I", img.text, field)
        return v
    raise SystemExit("unknown capture " + capture)


def shortest_unique(img, start_off, max_len, capture_field=None):
    buf = img.text[start_off:start_off + max_len]
    keep = ()
    mask = mask_for(img, start_off, buf)
    if capture_field is not None:
        for k in range(capture_field, capture_field + 4):
            mask[k] = True
    for n in range(8, len(buf) + 1):
        if mask[n - 1] or mask[0]:
            continue
        if capture_field is not None and n < capture_field + 4:
            continue
        pat = pattern_text(buf[:n], mask[:n])
        if pat.split().count("??") * 2 > n:
            continue
        if len(matches(img, pat, 2)) == 1:
            return pat
    return None


def find_capture_field(img, site_off, target):
    """The offset (inside the instruction at site_off) of a disp32 that resolves to target."""
    for k in range(1, 12):
        f = site_off + k
        v, = struct.unpack_from("<i", img.text, f)
        u = v & 0xFFFFFFFF
        if u == target:
            return k, "abs32"
        for extra in (0, 1, 4):
            if img.text_addr + f + 4 + extra + v == target and extra == 0:
                return k, "rip32"
    return None, None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--binary", required=True)
    ap.add_argument("--addr", type=lambda v: int(v, 0), help="function address")
    ap.add_argument("--xref", type=lambda v: int(v, 0), help="data address referenced by code")
    ap.add_argument("--sites", help="file with candidate site addresses (objdump -d lines are fine)")
    ap.add_argument("--max-sites", type=int, default=200)
    ap.add_argument("--check", help="a pattern to check")
    ap.add_argument("--capture", default="start", choices=["start", "rip32", "abs32"])
    ap.add_argument("--offset", type=int, default=0)
    ap.add_argument("--max", type=int, default=48, help="longest pattern to try (bytes)")
    args = ap.parse_args()
    img = Image(args.binary)

    if args.check:
        hits = matches(img, args.check, 3)
        print("matches  %d" % len(hits))
        for h in hits:
            print("match    0x%x -> 0x%x" % (img.text_addr + h, resolve(img, h, args.capture, args.offset)))
        return 0 if len(hits) == 1 else 1

    if args.addr is not None:
        pat = shortest_unique(img, img.off(args.addr), args.max)
        if not pat:
            print("no unique pattern within %d bytes" % args.max, file=sys.stderr)
            return 1
        print("pattern  %s\ncapture  start offset 0\nresolves 0x%x" % (pat, args.addr))
        return 0

    if args.xref is not None:
        if not args.sites:
            raise SystemExit("--xref needs --sites (e.g. objdump -d | grep %x)" % args.xref)
        sites = []
        for line in open(args.sites):
            m = re.match(r"\s*([0-9a-f]+):", line)
            if m:
                sites.append(int(m.group(1), 16))
        best = None
        for s in sites[:args.max_sites]:
            so = img.off(s)
            field, kind = find_capture_field(img, so, args.xref)
            if field is None:
                continue
            pat = shortest_unique(img, so, args.max, field)
            if pat and (best is None or len(pat) < len(best[0])):
                best = (pat, kind, field, s)
                if len(pat.split()) <= 16:
                    break
        if not best:
            print("no unique pattern from %d sites" % min(len(sites), args.max_sites), file=sys.stderr)
            return 1
        pat, kind, field, s = best
        print("pattern  %s\ncapture  %s offset %d\nsite     0x%x\nresolves 0x%x" % (pat, kind, field, s, args.xref))
        return 0
    ap.error("pass --addr, --xref or --check")


if __name__ == "__main__":
    sys.exit(main())
