#!/usr/bin/env python3
"""Dump fields and methods of classes inside projectzomboid.jar.

Self-contained JVM class-file parser (JVMS chapter 4): constant pool ->
fields/methods with name, descriptor and access flags, plus parameter names
recovered from the Code attribute's LocalVariableTable when present.

Project Zomboid B42 ships class-file major version 69 (Java 25) and is not
obfuscated, so this is enough to re-pin the ByteBuddy hooks after a PZ update
without needing a JDK on the host.

Usage:
    dump-signatures.py <jar> <class-name> [<class-name> ...]
    dump-signatures.py <jar> --grep <substring>

Class names may be given as zombie.network.RCONServer, zombie/network/RCONServer
or zombie/network/RCONServer.class.
"""
from __future__ import annotations

import re
import struct
import sys
import zipfile

# ── constant pool tags ───────────────────────────────────────────────────────
CONSTANT_Utf8 = 1
CONSTANT_Integer = 3
CONSTANT_Float = 4
CONSTANT_Long = 5
CONSTANT_Double = 6
CONSTANT_Class = 7
CONSTANT_String = 8
CONSTANT_Fieldref = 9
CONSTANT_Methodref = 10
CONSTANT_InterfaceMethodref = 11
CONSTANT_NameAndType = 12
CONSTANT_MethodHandle = 15
CONSTANT_MethodType = 16
CONSTANT_Dynamic = 17
CONSTANT_InvokeDynamic = 18
CONSTANT_Module = 19
CONSTANT_Package = 20

# tag -> number of bytes following the tag (None = variable, handled inline)
FIXED_SIZE = {
    CONSTANT_Integer: 4,
    CONSTANT_Float: 4,
    CONSTANT_Long: 8,
    CONSTANT_Double: 8,
    CONSTANT_Class: 2,
    CONSTANT_String: 2,
    CONSTANT_Fieldref: 4,
    CONSTANT_Methodref: 4,
    CONSTANT_InterfaceMethodref: 4,
    CONSTANT_NameAndType: 4,
    CONSTANT_MethodHandle: 3,
    CONSTANT_MethodType: 2,
    CONSTANT_Dynamic: 4,
    CONSTANT_InvokeDynamic: 4,
    CONSTANT_Module: 2,
    CONSTANT_Package: 2,
}

CLASS_FLAGS = [
    (0x0001, "public"), (0x0010, "final"), (0x0020, "super"),
    (0x0200, "interface"), (0x0400, "abstract"), (0x1000, "synthetic"),
    (0x2000, "annotation"), (0x4000, "enum"),
]
MEMBER_FLAGS = [
    (0x0001, "public"), (0x0002, "private"), (0x0004, "protected"),
    (0x0008, "static"), (0x0010, "final"), (0x0020, "synchronized"),
    (0x0040, "volatile/bridge"), (0x0080, "transient/varargs"),
    (0x0100, "native"), (0x0400, "abstract"), (0x0800, "strict"),
    (0x1000, "synthetic"),
]

BASE_TYPES = {
    "B": "byte", "C": "char", "D": "double", "F": "float",
    "I": "int", "J": "long", "S": "short", "Z": "boolean", "V": "void",
}


class Reader:
    def __init__(self, data: bytes) -> None:
        self.d = data
        self.p = 0

    def u1(self) -> int:
        v = self.d[self.p]
        self.p += 1
        return v

    def u2(self) -> int:
        v = struct.unpack_from(">H", self.d, self.p)[0]
        self.p += 2
        return v

    def u4(self) -> int:
        v = struct.unpack_from(">I", self.d, self.p)[0]
        self.p += 4
        return v

    def take(self, n: int) -> bytes:
        v = self.d[self.p:self.p + n]
        self.p += n
        return v


def parse_constant_pool(r: Reader) -> dict[int, object]:
    count = r.u2()
    pool: dict[int, object] = {}
    i = 1
    while i < count:
        tag = r.u1()
        if tag == CONSTANT_Utf8:
            length = r.u2()
            pool[i] = r.take(length).decode("utf-8", "replace")
        elif tag in FIXED_SIZE:
            raw = r.take(FIXED_SIZE[tag])
            if tag in (CONSTANT_Class, CONSTANT_String, CONSTANT_MethodType,
                       CONSTANT_Module, CONSTANT_Package):
                pool[i] = ("ref", struct.unpack(">H", raw)[0])
            elif tag == CONSTANT_NameAndType:
                pool[i] = ("nat",) + struct.unpack(">HH", raw)
            else:
                pool[i] = (tag, raw)
        else:
            raise ValueError(f"unknown constant pool tag {tag} at index {i}")
        # long and double take two slots (JVMS 4.4.5)
        i += 2 if tag in (CONSTANT_Long, CONSTANT_Double) else 1
    return pool


def utf8(pool, idx):
    v = pool.get(idx)
    return v if isinstance(v, str) else f"#{idx}"


def class_name(pool, idx):
    if idx == 0:
        return None
    v = pool.get(idx)
    if isinstance(v, tuple) and v[0] == "ref":
        return utf8(pool, v[1]).replace("/", ".")
    return f"#{idx}"


def flags_to_str(value: int, table) -> str:
    return " ".join(name for bit, name in table if value & bit)


def parse_descriptor_types(desc: str) -> tuple[list[str], str]:
    """Return (parameter type names, return type name) for a method descriptor."""
    assert desc.startswith("(")
    params: list[str] = []
    i = 1
    while desc[i] != ")":
        t, i = parse_one_type(desc, i)
        params.append(t)
    ret, _ = parse_one_type(desc, i + 1)
    return params, ret


def parse_one_type(desc: str, i: int) -> tuple[str, int]:
    dims = 0
    while desc[i] == "[":
        dims += 1
        i += 1
    c = desc[i]
    if c == "L":
        end = desc.index(";", i)
        name = desc[i + 1:end].replace("/", ".")
        i = end + 1
    else:
        name = BASE_TYPES.get(c, c)
        i += 1
    return name + "[]" * dims, i


def skip_attributes(r: Reader, pool, collect_code: bool = False):
    """Read the attribute table; optionally return the raw Code attribute."""
    code_attr = None
    count = r.u2()
    for _ in range(count):
        name = utf8(pool, r.u2())
        length = r.u4()
        body = r.take(length)
        if collect_code and name == "Code":
            code_attr = body
    return code_attr


def local_variable_names(code: bytes, pool) -> list[str]:
    """Parameter names from the Code attribute's LocalVariableTable, in slot order."""
    r = Reader(code)
    r.u2()  # max_stack
    r.u2()  # max_locals
    code_len = r.u4()
    r.take(code_len)
    ex_len = r.u2()
    r.take(ex_len * 8)
    names: dict[int, str] = {}
    count = r.u2()
    for _ in range(count):
        name = utf8(pool, r.u2())
        length = r.u4()
        body = r.take(length)
        if name != "LocalVariableTable":
            continue
        sub = Reader(body)
        n = sub.u2()
        for _ in range(n):
            start_pc = sub.u2()
            sub.u2()  # length
            var_name = utf8(pool, sub.u2())
            sub.u2()  # descriptor
            index = sub.u2()
            # Parameters are live from pc 0; locals declared later are not.
            if start_pc == 0 and index not in names:
                names[index] = var_name
    return [names[k] for k in sorted(names)]


def dump_class(data: bytes) -> None:
    r = Reader(data)
    magic = r.u4()
    if magic != 0xCAFEBABE:
        raise ValueError("not a class file")
    minor = r.u2()
    major = r.u2()
    pool = parse_constant_pool(r)
    access = r.u2()
    this_class = class_name(pool, r.u2())
    super_class = class_name(pool, r.u2())
    ifaces = [class_name(pool, r.u2()) for _ in range(r.u2())]

    print(f"class {this_class}")
    print(f"  class-file version {major}.{minor}  (Java {major - 44})")
    print(f"  access  {flags_to_str(access, CLASS_FLAGS)}")
    print(f"  extends {super_class}")
    if ifaces:
        print(f"  implements {', '.join(ifaces)}")

    print("  -- fields --")
    for _ in range(r.u2()):
        f_access = r.u2()
        f_name = utf8(pool, r.u2())
        f_desc = utf8(pool, r.u2())
        skip_attributes(r, pool)
        f_type, _ = parse_one_type(f_desc, 0)
        print(f"    {flags_to_str(f_access, MEMBER_FLAGS):<28} "
              f"{f_type} {f_name}    [{f_desc}]")

    print("  -- methods --")
    for _ in range(r.u2()):
        m_access = r.u2()
        m_name = utf8(pool, r.u2())
        m_desc = utf8(pool, r.u2())
        code = skip_attributes(r, pool, collect_code=True)
        params, ret = parse_descriptor_types(m_desc)
        names: list[str] = []
        if code:
            try:
                slots = local_variable_names(code, pool)
            except Exception:
                slots = []
            if not (m_access & 0x0008) and slots:  # drop `this` for instance methods
                slots = slots[1:]
            names = slots
        rendered = []
        for i, p in enumerate(params):
            rendered.append(f"{p} {names[i]}" if i < len(names) else p)
        print(f"    {flags_to_str(m_access, MEMBER_FLAGS):<28} "
              f"{ret} {m_name}({', '.join(rendered)})    [{m_desc}]")


def normalise(name: str) -> str:
    name = name.strip()
    if name.endswith(".class"):
        name = name[:-len(".class")]
    return name.replace(".", "/") + ".class"


def main(argv: list[str]) -> int:
    if len(argv) < 3:
        print(__doc__, file=sys.stderr)
        return 2
    jar_path, rest = argv[1], argv[2:]
    with zipfile.ZipFile(jar_path) as zf:
        if rest[0] == "--grep":
            pattern = re.compile(rest[1])
            for n in sorted(zf.namelist()):
                if n.endswith(".class") and pattern.search(n):
                    print(n[:-len(".class")].replace("/", "."))
            return 0
        status = 0
        for raw in rest:
            entry = normalise(raw)
            try:
                data = zf.read(entry)
            except KeyError:
                print(f"ERROR: {entry} not found in {jar_path}", file=sys.stderr)
                status = 1
                continue
            dump_class(data)
            print()
        return status


if __name__ == "__main__":
    sys.exit(main(sys.argv))
