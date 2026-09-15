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
    dump-signatures.py <jar> --code <class-name> <method-name>

The --code mode disassembles a single method's Code attribute (opcode walk),
resolving constant-pool references on field/method/type/ldc instructions. It is
used to copy a command class's exact call sequence (e.g. TeleportToCommand's
TeleportUserToCoords) into the agent's typed facade.

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


# ── bytecode disassembler (opcode walk) ──────────────────────────────────────
# opcode -> (mnemonic, number of operand bytes). Variable-length opcodes
# (tableswitch/lookupswitch/wide) are handled specially in walk_code().
OPCODES = {
    0x00: ("nop", 0), 0x01: ("aconst_null", 0), 0x02: ("iconst_m1", 0),
    0x03: ("iconst_0", 0), 0x04: ("iconst_1", 0), 0x05: ("iconst_2", 0),
    0x06: ("iconst_3", 0), 0x07: ("iconst_4", 0), 0x08: ("iconst_5", 0),
    0x09: ("lconst_0", 0), 0x0a: ("lconst_1", 0), 0x0b: ("fconst_0", 0),
    0x0c: ("fconst_1", 0), 0x0d: ("fconst_2", 0), 0x0e: ("dconst_0", 0),
    0x0f: ("dconst_1", 0), 0x10: ("bipush", 1), 0x11: ("sipush", 2),
    0x12: ("ldc", 1), 0x13: ("ldc_w", 2), 0x14: ("ldc2_w", 2),
    0x15: ("iload", 1), 0x16: ("lload", 1), 0x17: ("fload", 1),
    0x18: ("dload", 1), 0x19: ("aload", 1), 0x1a: ("iload_0", 0),
    0x1b: ("iload_1", 0), 0x1c: ("iload_2", 0), 0x1d: ("iload_3", 0),
    0x1e: ("lload_0", 0), 0x1f: ("lload_1", 0), 0x20: ("lload_2", 0),
    0x21: ("lload_3", 0), 0x22: ("fload_0", 0), 0x23: ("fload_1", 0),
    0x24: ("fload_2", 0), 0x25: ("fload_3", 0), 0x26: ("dload_0", 0),
    0x27: ("dload_1", 0), 0x28: ("dload_2", 0), 0x29: ("dload_3", 0),
    0x2a: ("aload_0", 0), 0x2b: ("aload_1", 0), 0x2c: ("aload_2", 0),
    0x2d: ("aload_3", 0), 0x2e: ("iaload", 0), 0x2f: ("laload", 0),
    0x30: ("faload", 0), 0x31: ("daload", 0), 0x32: ("aaload", 0),
    0x33: ("baload", 0), 0x34: ("caload", 0), 0x35: ("saload", 0),
    0x36: ("istore", 1), 0x37: ("lstore", 1), 0x38: ("fstore", 1),
    0x39: ("dstore", 1), 0x3a: ("astore", 1), 0x3b: ("istore_0", 0),
    0x3c: ("istore_1", 0), 0x3d: ("istore_2", 0), 0x3e: ("istore_3", 0),
    0x3f: ("lstore_0", 0), 0x40: ("lstore_1", 0), 0x41: ("lstore_2", 0),
    0x42: ("lstore_3", 0), 0x43: ("fstore_0", 0), 0x44: ("fstore_1", 0),
    0x45: ("fstore_2", 0), 0x46: ("fstore_3", 0), 0x47: ("dstore_0", 0),
    0x48: ("dstore_1", 0), 0x49: ("dstore_2", 0), 0x4a: ("dstore_3", 0),
    0x4b: ("astore_0", 0), 0x4c: ("astore_1", 0), 0x4d: ("astore_2", 0),
    0x4e: ("astore_3", 0), 0x4f: ("iastore", 0), 0x50: ("lastore", 0),
    0x51: ("fastore", 0), 0x52: ("dastore", 0), 0x53: ("aastore", 0),
    0x54: ("bastore", 0), 0x55: ("castore", 0), 0x56: ("sastore", 0),
    0x57: ("pop", 0), 0x58: ("pop2", 0), 0x59: ("dup", 0),
    0x5a: ("dup_x1", 0), 0x5b: ("dup_x2", 0), 0x5c: ("dup2", 0),
    0x5d: ("dup2_x1", 0), 0x5e: ("dup2_x2", 0), 0x5f: ("swap", 0),
    0x60: ("iadd", 0), 0x61: ("ladd", 0), 0x62: ("fadd", 0),
    0x63: ("dadd", 0), 0x64: ("isub", 0), 0x65: ("lsub", 0),
    0x66: ("fsub", 0), 0x67: ("dsub", 0), 0x68: ("imul", 0),
    0x69: ("lmul", 0), 0x6a: ("fmul", 0), 0x6b: ("dmul", 0),
    0x6c: ("idiv", 0), 0x6d: ("ldiv", 0), 0x6e: ("fdiv", 0),
    0x6f: ("ddiv", 0), 0x70: ("irem", 0), 0x71: ("lrem", 0),
    0x72: ("frem", 0), 0x73: ("drem", 0), 0x74: ("ineg", 0),
    0x75: ("lneg", 0), 0x76: ("fneg", 0), 0x77: ("dneg", 0),
    0x78: ("ishl", 0), 0x79: ("lshl", 0), 0x7a: ("ishr", 0),
    0x7b: ("lshr", 0), 0x7c: ("iushr", 0), 0x7d: ("lushr", 0),
    0x7e: ("iand", 0), 0x7f: ("land", 0), 0x80: ("ior", 0),
    0x81: ("lor", 0), 0x82: ("ixor", 0), 0x83: ("lxor", 0),
    0x84: ("iinc", 2), 0x85: ("i2l", 0), 0x86: ("i2f", 0),
    0x87: ("i2d", 0), 0x88: ("l2i", 0), 0x89: ("l2f", 0),
    0x8a: ("l2d", 0), 0x8b: ("f2i", 0), 0x8c: ("f2l", 0),
    0x8d: ("f2d", 0), 0x8e: ("d2i", 0), 0x8f: ("d2l", 0),
    0x90: ("d2f", 0), 0x91: ("i2b", 0), 0x92: ("i2c", 0),
    0x93: ("i2s", 0), 0x94: ("lcmp", 0), 0x95: ("fcmpl", 0),
    0x96: ("fcmpg", 0), 0x97: ("dcmpl", 0), 0x98: ("dcmpg", 0),
    0x99: ("ifeq", 2), 0x9a: ("ifne", 2), 0x9b: ("iflt", 2),
    0x9c: ("ifge", 2), 0x9d: ("ifgt", 2), 0x9e: ("ifle", 2),
    0x9f: ("if_icmpeq", 2), 0xa0: ("if_icmpne", 2), 0xa1: ("if_icmplt", 2),
    0xa2: ("if_icmpge", 2), 0xa3: ("if_icmpgt", 2), 0xa4: ("if_icmple", 2),
    0xa5: ("if_acmpeq", 2), 0xa6: ("if_acmpne", 2), 0xa7: ("goto", 2),
    0xa8: ("jsr", 2), 0xa9: ("ret", 1),
    0xac: ("ireturn", 0), 0xad: ("lreturn", 0), 0xae: ("freturn", 0),
    0xaf: ("dreturn", 0), 0xb0: ("areturn", 0), 0xb1: ("return", 0),
    0xb2: ("getstatic", 2), 0xb3: ("putstatic", 2), 0xb4: ("getfield", 2),
    0xb5: ("putfield", 2), 0xb6: ("invokevirtual", 2), 0xb7: ("invokespecial", 2),
    0xb8: ("invokestatic", 2), 0xb9: ("invokeinterface", 4), 0xba: ("invokedynamic", 4),
    0xbb: ("new", 2), 0xbc: ("newarray", 1), 0xbd: ("anewarray", 2),
    0xbe: ("arraylength", 0), 0xbf: ("athrow", 0), 0xc0: ("checkcast", 2),
    0xc1: ("instanceof", 2), 0xc2: ("monitorenter", 0), 0xc3: ("monitorexit", 0),
    0xc5: ("multianewarray", 3), 0xc6: ("ifnull", 2), 0xc7: ("ifnonnull", 2),
    0xc8: ("goto_w", 4), 0xc9: ("jsr_w", 4),
}

CP_REF_OPS = {
    0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba,
    0xbb, 0xbd, 0xc0, 0xc1, 0xc5,
}
LDC_OPS = {0x12, 0x13, 0x14}


def resolve_cp(pool, idx) -> str:
    """Human-readable form of a constant-pool entry for disassembly output."""
    v = pool.get(idx)
    if isinstance(v, str):
        return f'"{v}"'
    if isinstance(v, tuple):
        tag = v[0]
        if tag == "ref":
            return class_name(pool, idx) or f"#{idx}"
        if tag == "nat":
            return f"{utf8(pool, v[1])}:{utf8(pool, v[2])}"
        if tag in (CONSTANT_Fieldref, CONSTANT_Methodref, CONSTANT_InterfaceMethodref):
            owner_idx, nat_idx = struct.unpack(">HH", v[1])
            owner = class_name(pool, owner_idx)
            nat = pool.get(nat_idx)
            if isinstance(nat, tuple) and nat[0] == "nat":
                return f"{owner}.{utf8(pool, nat[1])}{utf8(pool, nat[2])}"
            return f"{owner}.#{nat_idx}"
        if tag in (CONSTANT_Integer, CONSTANT_Float):
            return f"{struct.unpack('>i' if tag == CONSTANT_Integer else '>f', v[1])[0]}"
        if tag in (CONSTANT_Long, CONSTANT_Double):
            return f"{struct.unpack('>q' if tag == CONSTANT_Long else '>d', v[1])[0]}"
        if tag == CONSTANT_InvokeDynamic:
            _, nat_idx = struct.unpack(">HH", v[1])
            nat = pool.get(nat_idx)
            if isinstance(nat, tuple) and nat[0] == "nat":
                return f"indy {utf8(pool, nat[1])}{utf8(pool, nat[2])}"
    return f"#{idx}"


def walk_code(code: bytes, pool) -> None:
    r = Reader(code)
    r.u2()  # max_stack
    r.u2()  # max_locals
    code_len = r.u4()
    body = r.take(code_len)
    pc = 0
    while pc < code_len:
        op = body[pc]
        name, nbytes = OPCODES.get(op, (f"op_0x{op:02x}", 0))
        start = pc
        pc += 1
        if op in (0xaa, 0xab):  # tableswitch / lookupswitch
            pad = (4 - (pc % 4)) % 4
            sub = Reader(body[pc + pad:])
            sub.u4()  # default
            if op == 0xaa:
                low = struct.unpack(">i", body[pc + pad + 4:pc + pad + 8])[0]
                high = struct.unpack(">i", body[pc + pad + 8:pc + pad + 12])[0]
                n = high - low + 1
                consumed = pad + 12 + n * 4
            else:
                npairs = struct.unpack(">i", body[pc + pad + 4:pc + pad + 8])[0]
                consumed = pad + 8 + npairs * 8
            print(f"    {start:>5}: {name}")
            pc += consumed
            continue
        operand = body[pc:pc + nbytes]
        pc += nbytes
        extra = ""
        if nbytes >= 2:
            idx = struct.unpack(">H", operand[:2])[0]
            if op in CP_REF_OPS:
                extra = "  " + resolve_cp(pool, idx)
            elif op in LDC_OPS:
                extra = "  " + resolve_cp(pool, idx)
        elif nbytes == 1 and op in LDC_OPS:
            extra = "  " + resolve_cp(pool, operand[0])
        elif nbytes == 1:
            extra = f"  {operand[0]}"
        print(f"    {start:>5}: {name}{extra}")


def dump_method_code(data: bytes, method_name: str) -> int:
    r = Reader(data)
    if r.u4() != 0xCAFEBABE:
        raise ValueError("not a class file")
    r.u2()  # minor
    r.u2()  # major
    pool = parse_constant_pool(r)
    r.u2()  # access
    r.u2()  # this
    r.u2()  # super
    for _ in range(r.u2()):
        r.u2()  # interface
    for _ in range(r.u2()):  # fields
        r.u2(); r.u2(); r.u2()
        skip_attributes(r, pool)
    found = 0
    for _ in range(r.u2()):  # methods
        m_access = r.u2()
        m_name = utf8(pool, r.u2())
        m_desc = utf8(pool, r.u2())
        code = skip_attributes(r, pool, collect_code=True)
        if m_name == method_name:
            found += 1
            print(f"  method {m_name}{m_desc} "
                  f"[{flags_to_str(m_access, MEMBER_FLAGS)}]")
            if code:
                walk_code(code, pool)
            else:
                print("    (no Code attribute)")
            print()
    return found


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
        if rest[0] == "--code":
            if len(rest) != 3:
                print("usage: --code <class-name> <method-name>", file=sys.stderr)
                return 2
            entry = normalise(rest[1])
            data = zf.read(entry)
            print(f"class {rest[1]}")
            if dump_method_code(data, rest[2]) == 0:
                print(f"ERROR: no method {rest[2]} in {rest[1]}", file=sys.stderr)
                return 1
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
