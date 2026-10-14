#!/usr/bin/env python3
"""ABI gate for main.dll: every symbol it imports from UE4SS.dll must be exported by the pinned UE4SS.dll.

usage: check-imports.py <main.dll> <UE4SS.dll>
Exits 1 when an import is missing (a mangled-name mismatch would make UE4SS fail to load the mod).
"""
import struct
import sys


def pe(path):
    d = open(path, 'rb').read()
    p = struct.unpack_from('<I', d, 0x3c)[0]
    nsec = struct.unpack_from('<H', d, p + 6)[0]
    optsz = struct.unpack_from('<H', d, p + 20)[0]
    opt = p + 24
    secs = [struct.unpack_from('<8sIIII', d, opt + optsz + i * 40) for i in range(nsec)]

    def off(rva):
        for _n, vs, va, rs, ro in secs:
            if va <= rva < va + max(vs, rs):
                return rva - va + ro
        raise ValueError(hex(rva))

    def cstr(o):
        return d[o:d.index(b'\0', o)].decode()

    return d, opt, off, cstr


def exports(path):
    d, opt, off, cstr = pe(path)
    rva = struct.unpack_from('<I', d, opt + 112)[0]
    n = struct.unpack_from('<I', d, off(rva) + 24)[0]
    names = struct.unpack_from('<I', d, off(rva) + 32)[0]
    return {cstr(off(struct.unpack_from('<I', d, off(names) + i * 4)[0])) for i in range(n)}


def imports(path):
    d, opt, off, cstr = pe(path)
    rva = struct.unpack_from('<I', d, opt + 120)[0]
    out = {}
    o = off(rva)
    while True:
        ilt, _ts, _fc, name, iat = struct.unpack_from('<IIIII', d, o)
        if not name:
            break
        dll = cstr(off(name))
        syms = out.setdefault(dll, [])
        t = off(ilt or iat)
        while True:
            e = struct.unpack_from('<Q', d, t)[0]
            if not e:
                break
            if e & (1 << 63):
                syms.append('#%d' % (e & 0xffff))
            else:
                syms.append(cstr(off(e & 0x7fffffff) + 2))
            t += 8
        o += 20
    return out


def main():
    mod, ue4ss = sys.argv[1], sys.argv[2]
    imp = imports(mod)
    exp = exports(ue4ss)
    ue = next((v for k, v in imp.items() if k.lower() == 'ue4ss.dll'), [])
    missing = [s for s in ue if s not in exp]
    for dll, syms in sorted(imp.items()):
        print('%s: %d imports' % (dll, len(syms)))
    for s in ue:
        print('  %s %s' % ('MISSING' if s in missing else 'ok     ', s))
    if not ue:
        print('FAIL: main.dll imports nothing from UE4SS.dll')
        return 1
    if missing:
        print('FAIL: %d UE4SS import(s) not exported by %s' % (len(missing), ue4ss))
        return 1
    print('PASS: all %d UE4SS imports resolve against %s' % (len(ue), ue4ss))
    return 0


if __name__ == '__main__':
    sys.exit(main())
