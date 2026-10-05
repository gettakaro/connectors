#!/usr/bin/env python3
"""tools/repin.py against synthetic server binaries, so CI proves the re-pin logic without the
200 MB binary.

The synthetic images hold the pinned 25639945 signatures from pins.json (wildcards filled so the
captures point at the real anchors, the detour prologue from pins.cpp) inside deterministic filler,
placed at their real addresses in a minimal ELF (and a minimal PE for the Windows path), then
mutated the way a new server build changes them. No server machine code is committed: when
CONAN_ANCHOR_FIXTURES names the private anchor-fixture directory (our own runner), the real code
bytes of linux-25639945.anchors (with their nearest decoys) are used instead.

  same build            pinned, every anchor reproduced                      -> exit 0
  shifted data          globals moved, code bytes unchanged                  -> exit 0, new addresses
  moved code            every function moved, globals unchanged              -> exit 0, new addresses
  broken signature      a literal byte inside the pattern changed            -> exit 1 without help,
                                                                                exit 3 with --previous
                                                                                or --anchor hints
  changed prologue      the detour's 20 relocated bytes changed               -> exit 1 (needs a human)
  PE (Windows)          no Windows signatures yet; RVA hints from the W0       -> RVAs, PE code id; abs32
                        runtime autodetect                                        refused on a PE
plus the reflection diff, the code <-> manifest check, the catalog check and the fixture check.
Usage: repin_test.py   (python 3.7+, no third-party modules)
"""
import copy
import io
import json
import os
import random
import shutil
import struct
import sys
import tempfile
from contextlib import redirect_stdout
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import repin  # noqa: E402

FIXTURE_DIR = os.environ.get("CONAN_ANCHOR_FIXTURES", "")
FIXTURE = Path(FIXTURE_DIR) / "linux-25639945.anchors" if FIXTURE_DIR else None
PE_SITE, NB_SITE = 0x3F12340, 0x3D1207F  # the two real match sites of build 25639945
OBJ, NAMES = 0xC35A580, 0xC2A5D40
BSS_LO, BSS_SIZE = 0xC200000, 0x200000

failures, passes = [], [0]


def check(ok, what):
    if ok:
        passes[0] += 1
    else:
        failures.append(what)
        print("  FAIL " + what)


def text_from_fixture():
    """(text_lo, bytearray) holding the fixture regions near the two match sites."""
    _, _, regions = repin.read_fixture(FIXTURE)
    lo_lim, hi_lim = NB_SITE - 0x1000, PE_SITE + 0x1000
    regions = [(a, d) for a, d in regions if lo_lim <= a < hi_lim]
    lo = min(a for a, _ in regions) & ~0xFFF
    hi = max(a + len(d) for a, d in regions)
    buf = bytearray(b"\xcc" * (hi - lo + 0x100))
    for a, d in regions:
        buf[a - lo:a - lo + len(d)] = d
    return lo, buf


def text_from_signatures():
    """(text_lo, bytearray): the pinned Linux signatures at the real match sites in seeded filler."""
    sigs = {s["anchor"]: s for s in json.loads((ROOT / "core/pins/pins.json").read_text())["signatures"]["linux"]}
    prologue = repin.prologue_from_cpp(ROOT / "core/pins/pins.cpp")
    rng = random.Random(25639945)
    lo = (NB_SITE - 0x1000) & ~0xFFF
    hi = PE_SITE + 0x1000
    buf = bytearray(rng.getrandbits(8) for _ in range(hi - lo + 0x100))

    def place(site, pattern):
        for i, tok in enumerate(pattern.split()):
            if tok != "??":
                buf[site - lo + i] = int(tok, 16)

    place(PE_SITE, sigs["processEvent"]["pattern"])
    buf[PE_SITE - lo:PE_SITE - lo + len(prologue)] = prologue
    f = PE_SITE + sigs["objObjects"]["offset"]
    put32(buf, lo, f, OBJ - (f + 4), True)
    place(NB_SITE, sigs["nameBlocks"]["pattern"])
    put32(buf, lo, NB_SITE + sigs["nameBlocks"]["offset"], NAMES)
    return lo, buf


def put32(buf, lo, addr, value, signed=False):
    struct.pack_into("<i" if signed else "<I", buf, addr - lo, value)


def get32(buf, lo, addr, signed=False):
    return struct.unpack_from("<i" if signed else "<I", buf, addr - lo)[0]


def write_elf(path, text_lo, text, build_id):
    """ELF64 with .text, .bss (writable), .note.gnu.build-id and .shstrtab; enough for sigderive."""
    shstr = b"\0.text\0.bss\0.note.gnu.build-id\0.shstrtab\0"
    note = struct.pack("<III", 4, len(build_id), 3) + b"GNU\0" + build_id
    body = bytearray(b"\0" * 0x40)
    off_text = len(body)
    body += text
    off_note = len(body)
    body += note
    off_str = len(body)
    body += shstr
    while len(body) % 8:
        body += b"\0"
    shoff = len(body)

    def sh(name, typ, flags, addr, off, size):
        return struct.pack("<IIQQQQIIQQ", name, typ, flags, addr, off, size, 0, 0, 16, 0)

    body += sh(0, 0, 0, 0, 0, 0)
    body += sh(shstr.index(b".text"), 1, 6, text_lo, off_text, len(text))
    body += sh(shstr.index(b".bss"), 8, 3, BSS_LO, 0, BSS_SIZE)
    body += sh(shstr.index(b".note.gnu.build-id"), 7, 2, 0x400200, off_note, len(note))
    body += sh(shstr.index(b".shstrtab"), 3, 0, 0, off_str, len(shstr))
    hdr = bytearray(b"\x7fELF\x02\x01\x01" + b"\0" * 9)
    hdr += struct.pack("<HHIQQQIHHHHHH", 2, 0x3E, 1, 0, 0, shoff, 0, 0x40, 0, 0, 0x40, 5, 4)
    body[0:0x40] = hdr
    Path(path).write_bytes(bytes(body))


def write_pe(path, text_rva, text, stamp=0x5F3E2A10, base=0x140000000):
    """PE32+ with .text and a writable .data; image base 0x140000000, so VA = base + RVA."""
    falign = 0x200
    raw_text = len(text) + (-len(text) % falign)
    size_of_image = (BSS_LO + BSS_SIZE + 0xFFF) & ~0xFFF
    dos = bytearray(b"MZ" + b"\0" * 0x3E)
    struct.pack_into("<I", dos, 0x3C, 0x80)
    dos += b"\0" * (0x80 - len(dos))
    coff = b"PE\0\0" + struct.pack("<HHIIIHH", 0x8664, 2, stamp, 0, 0, 240, 0x22)
    opt = bytearray(240)
    struct.pack_into("<H", opt, 0, 0x20B)
    struct.pack_into("<Q", opt, 24, base)
    struct.pack_into("<II", opt, 32, 0x1000, falign)
    struct.pack_into("<I", opt, 56, size_of_image)
    struct.pack_into("<I", opt, 60, 0x400)
    struct.pack_into("<H", opt, 70, 0x8160)
    struct.pack_into("<I", opt, 108, 16)

    def sec(name, vsize, vaddr, rawsize, rawoff, chars):
        return name.ljust(8, b"\0") + struct.pack("<IIIIIIHHI", vsize, vaddr, rawsize, rawoff, 0, 0, 0, 0, chars)

    hdrs = dos + coff + opt + sec(b".text", len(text), text_rva, raw_text, 0x400, 0x60000020)
    hdrs += sec(b".data", BSS_SIZE, BSS_LO, 0, 0, 0xC0000040)
    hdrs += b"\0" * (0x400 - len(hdrs))
    Path(path).write_bytes(bytes(hdrs) + bytes(text) + b"\0" * (raw_text - len(text)))


def run(*argv):
    out = io.StringIO()
    with redirect_stdout(out):
        code = repin.main([str(a) for a in argv])
    return code, out.getvalue()


def pins_for(tmp, builds, windows_sigs=None):
    pins = json.loads((ROOT / "core/pins/pins.json").read_text())
    pins["builds"] = builds
    if windows_sigs is not None:
        pins["signatures"]["windows"] = windows_sigs
    p = Path(tmp) / "pins.json"
    p.write_text(json.dumps(pins, indent=2))
    return p


def entry_of(out):
    lines = out.splitlines()
    return json.loads(lines[lines.index("pins.json entry") + 1].strip())


def test_binaries(tmp):
    if FIXTURE and FIXTURE.is_file():
        print("  (real code bytes from %s)" % FIXTURE)
        lo, base_text = text_from_fixture()
    else:
        print("  (synthetic bytes from the pinned signatures; set CONAN_ANCHOR_FIXTURES for the real ones)")
        lo, base_text = text_from_signatures()
    base = Path(tmp) / "base.elf"
    write_elf(base, lo, base_text, bytes.fromhex("3a05a6ef0c873f2bbf754ec495bdf2a686d3768d"))
    base_pin = {"platform": "linux", "buildId": "3a05a6ef0c873f2bbf754ec495bdf2a686d3768d", "build": "25639945",
                "anchors": {"processEvent": hex(PE_SITE), "objObjects": hex(OBJ), "nameBlocks": hex(NAMES)}}
    pins = pins_for(tmp, [base_pin])

    code, out = run("--pins", pins, "pin", "--binary", base)
    check(code == 0 and "CLEAN: already pinned and reproduced" in out, "same build: pinned and reproduced (exit %d)" % code)
    print("  %s same build: exit %d, pinned entry reproduced" % ("PASS" if code == 0 else "FAIL", code))

    # Shifted data: the globals move by +0x1080 (the 25356024 -> 25639945 step, reversed).
    t = bytearray(base_text)
    f = PE_SITE + 43
    put32(t, lo, f, get32(t, lo, f, True) + 0x1080, True)
    put32(t, lo, NB_SITE + 9, get32(t, lo, NB_SITE + 9) + 0x1080)
    shifted = Path(tmp) / "shifted.elf"
    write_elf(shifted, lo, t, b"\x11" * 20)
    code, out = run("--pins", pins, "pin", "--binary", shifted, "--previous", base, "--build", "99000001")
    e = entry_of(out)
    ok = code == 0 and e["anchors"] == {"processEvent": hex(PE_SITE), "objObjects": hex(OBJ + 0x1080),
                                        "nameBlocks": hex(NAMES + 0x1080)} and e["buildId"] == "11" * 20
    check(ok, "shifted data: new addresses, signatures unchanged (exit %d, %s)" % (code, e))
    check("moved +0x1080 vs 25639945" in out, "shifted data: the report names the move")
    print("  %s shifted data: exit %d, objObjects %s nameBlocks %s" % ("PASS" if ok else "FAIL", code,
                                                                     e["anchors"].get("objObjects"),
                                                                     e["anchors"].get("nameBlocks")))

    # --write appends the entry keyed by build-id, and only then.
    code, out = run("--pins", pins, "pin", "--binary", shifted, "--build", "99000001", "--write")
    written = json.loads(pins.read_text())
    check(code == 0 and any(b["buildId"] == "11" * 20 for b in written["builds"]) and len(written["builds"]) == 2,
          "--write adds the entry keyed by build-id")
    pins = pins_for(tmp, [base_pin])

    # Moved code: everything moves by +0x100; the rip32 displacement compensates, abs32 does not change.
    moved, lo_m = bytearray(base_text), lo + 0x100
    f = PE_SITE + 0x100 + 43
    put32(moved, lo_m, f, get32(moved, lo_m, f, True) - 0x100, True)
    movedp = Path(tmp) / "moved.elf"
    write_elf(movedp, lo_m, moved, b"\x22" * 20)
    code, out = run("--pins", pins, "pin", "--binary", movedp, "--build", "99000002")
    e = entry_of(out)
    ok = code == 0 and e["anchors"] == {"processEvent": hex(PE_SITE + 0x100), "objObjects": hex(OBJ),
                                        "nameBlocks": hex(NAMES)}
    check(ok, "moved code: ProcessEvent follows, data stays (exit %d, %s)" % (code, e))
    print("  %s moved code: exit %d, processEvent %s" % ("PASS" if ok else "FAIL", code, e["anchors"].get("processEvent")))

    # Broken signature: mov rbx,rdx -> mov rsi,rdx inside the shared ProcessEvent pattern (after the
    # 20 relocated prologue bytes).
    t = bytearray(base_text)
    t[PE_SITE + 22 - lo] = 0xD6
    broken = Path(tmp) / "broken.elf"
    write_elf(broken, lo, t, b"\x33" * 20)
    code, out = run("--pins", pins, "pin", "--binary", broken, "--build", "99000003")
    check(code == 1 and "NEEDS A HUMAN" in out and "give --previous" in out,
          "broken signature without help: needs a human (exit %d)" % code)
    print("  %s broken signature, no help: exit %d (needs a human)" % ("PASS" if code == 1 else "FAIL", code))
    code, out = run("--pins", pins, "pin", "--binary", broken, "--previous", base, "--build", "99000003")
    e = entry_of(out)
    ok = code == 3 and e["anchors"] == base_pin["anchors"] and "RE-DERIVED" in out
    check(ok, "broken signature with --previous: re-derived to the same anchors (exit %d, %s)" % (code, e))
    new_sigs = [json.loads(l.strip()) for l in out.split("new linux signatures")[1].split("\n\n")[0].splitlines()[1:]]
    for s in new_sigs:
        hits = repin.sd.matches(repin.sd.Image(str(broken)), s["pattern"], 3)
        check(len(hits) == 1, "re-derived %s pattern matches the new binary exactly once" % s["anchor"])
    print("  %s broken signature with --previous: exit %d, %d new signature(s) each matching once" % (
        "PASS" if ok else "FAIL", code, len(new_sigs)))
    code, out = run("--pins", pins, "pin", "--binary", broken, "--build", "99000003",
                    "--anchor", "processEvent=%#x" % PE_SITE, "--anchor", "objObjects=%#x" % OBJ)
    e = entry_of(out)
    ok = code == 3 and e["anchors"] == base_pin["anchors"]
    check(ok, "broken signature with --anchor hints: re-derived (exit %d, %s)" % (code, e))
    print("  %s broken signature with --anchor hints: exit %d" % ("PASS" if ok else "FAIL", code))

    # Changed prologue: the stack size (wildcarded in the pattern) grows, so the scan still works but
    # the detour would relocate different bytes.
    t = bytearray(base_text)
    t[PE_SITE + 16 - lo] = 0xC8
    pro = Path(tmp) / "prologue.elf"
    write_elf(pro, lo, t, b"\x44" * 20)
    code, out = run("--pins", pins, "pin", "--binary", pro, "--build", "99000004", "--write")
    check(code == 1 and "ProcessEvent prologue changed" in out and "not written" in out,
          "changed prologue: needs a human and nothing is written (exit %d)" % code)
    check(len(json.loads(pins.read_text())["builds"]) == 1, "changed prologue: pins.json untouched")
    print("  %s changed prologue: exit %d, pins.json untouched" % ("PASS" if code == 1 else "FAIL", code))

    # PE: no Windows signatures; RVA hints as the W0 runtime autodetect would print them.
    pe = Path(tmp) / "server.exe"
    write_pe(pe, lo, base_text)
    pins_w = pins_for(tmp, [base_pin], windows_sigs=[])
    code, out = run("--pins", pins_w, "pin", "--binary", pe, "--build", "25639945",
                    "--anchor", "processEvent=%#x" % PE_SITE, "--anchor", "objObjects=%#x" % OBJ)
    e = entry_of(out)
    ok = (code == 3 and e["platform"] == "windows" and e["buildId"] == "5f3e2a10-%x" % ((BSS_LO + BSS_SIZE + 0xFFF) & ~0xFFF)
          and e["anchors"] == {"processEvent": hex(PE_SITE), "objObjects": hex(OBJ)})
    check(ok, "PE: RVAs derived from hints, PE code id as identity (exit %d, %s)" % (code, e))
    print("  %s PE: exit %d, identity %s, anchors %s (RVAs)" % ("PASS" if ok else "FAIL", code, e["buildId"],
                                                               e["anchors"]))
    code, out = run("--pins", pins_w, "pin", "--binary", pe, "--build", "25639945",
                    "--anchor", "nameBlocks=%#x" % NAMES)
    check(code == 1 and "nameBlocks" in out.split("NEEDS A HUMAN")[-1],
          "PE: a data anchor reachable only through abs32 needs a human (exit %d)" % code)


def mini_dump(manifest):
    """A reflection dump with exactly the manifest's entries at their recorded values."""
    structs = {}
    for e in manifest["entries"]:
        if e["kind"] in ("class", "struct"):
            structs.setdefault(e["name"], {"name": e["name"], "properties": [], "functions": []})
            structs[e["name"]]["size"] = e["baseline"]["size"]
    for e in manifest["entries"]:
        if e["kind"] == "property":
            s = structs.setdefault(e["owner"], {"name": e["owner"], "size": 0, "properties": [], "functions": []})
            s["properties"].append(dict(name=e["name"], **e["baseline"]))
        elif e["kind"] == "function":
            s = structs.setdefault(e["owner"], {"name": e["owner"], "size": 0, "properties": [], "functions": []})
            s["functions"].append({"name": e["name"], "parmsSize": e["baseline"]["parmsSize"],
                                   "params": [{"name": n, "type": t, "offset": o, "size": z}
                                              for n, t, o, z in e["baseline"]["params"]]})
    return {"meta": {"buildId": "synthetic"}, "layout": {}, "structs": list(structs.values())}


def test_reflection(tmp):
    manifest = json.loads((ROOT / "tools/reflection-manifest.json").read_text())
    dump = mini_dump(manifest)
    p = Path(tmp) / "dump.json"
    p.write_text(json.dumps(dump))
    code, out = run("reflect", "--dump", p)
    check(code == 0 and "CLEAN" in out, "reflection: the manifest's own values are clean (exit %d)" % code)

    d = copy.deepcopy(dump)
    by = {s["name"]: s for s in d["structs"]}
    by["GameStateBase"]["properties"][0]["offset"] += 8          # runtime layout: information only
    by["GameStateBase"]["size"] += 64                             # a grown class: information only
    p.write_text(json.dumps(d))
    code, out = run("reflect", "--dump", p)
    check(code == 0 and "moved" in out, "reflection: a moved runtime offset is information, not a blocker")

    d = copy.deepcopy(dump)
    by = {s["name"]: s for s in d["structs"]}
    by["ChatRpcData"]["properties"] = [dict(q, offset=q["offset"] + 8) if q["name"] == "Message" else q
                                       for q in by["ChatRpcData"]["properties"]]
    by["PlayerState"]["properties"] = []
    by["ConanPlayerController"]["properties"][0]["type"] = "TextProperty"
    p.write_text(json.dumps(d))
    code, out = run("reflect", "--dump", p)
    check(code == 1, "reflection: blockers give exit 1 (got %d)" % code)
    check("ChatRpcData.Message: offset 104 -> 112 (fixed layout" in out, "reflection: a moved fixed offset needs a human")
    check("PlayerState.PlayerNamePrivate is gone" in out, "reflection: a missing property needs a human")
    check("UserIDFromURLOptions: type StrProperty -> TextProperty" in out, "reflection: a changed type needs a human")
    print("  %s reflection diff: moved runtime offset = info; moved fixed offset, missing property, changed type = "
          "needs a human" % ("PASS" if not failures else "FAIL"))


def test_code_and_catalog(tmp):
    core = Path(tmp) / "core"
    shutil.copytree(str(ROOT / "core"), str(core))
    code, out = run("code", "--core", core)
    check(code == 0, "code: the real core is covered by the manifest")
    (core / "conan/newlane.cpp").write_text(
        'const char* const kInventoryNames[2] = {"ItemInventoryX", "ItemListX"};\n'
        'void f() { FindFunctionByName("KismetSystemLibrary", "ExecuteConsoleCommandX"); }\n'
        'void g() { FindProperty(ClassOf(pc), "NestedArgX", "IntProperty", 4, p); }\n'
        'void h() { Fn(rx::ClassOf(pawn), "WrappedFnX", {{"ParamX", "IntProperty", 4}}, e); }\n'
        'void i() { HookDispatch::Subscription s; s.baseClass = "HookBaseX"; s.function = "HookFnX"; '
        's.params = {"HookParamX"}; sub("SubBaseX", "SubFnX", Phase::After, {"SubParamX"}, OnX); }\n')
    code, out = run("code", "--core", core)
    want = ["ItemInventoryX", "ExecuteConsoleCommandX", "NestedArgX", "WrappedFnX", "ParamX", "HookBaseX", "HookFnX",
            "HookParamX", "SubBaseX", "SubFnX", "SubParamX"]
    check(code == 1 and all("'%s'" % w in out for w in want),
          "code: a new lookup missing from the manifest fails (arrays, nested calls, wrappers, hook subscriptions): %s"
          % [w for w in want if "'%s'" % w not in out])
    print("  %s code <-> manifest: real core covered; an unlisted new lookup fails" % ("PASS" if code == 1 else "FAIL"))

    cat = Path(tmp) / "catalog"
    (cat / "targets").mkdir(parents=True)
    (cat / "game.json").write_text(json.dumps({"id": "conan-exiles", "platforms": ["linux"]}))

    def target(build, default):
        (cat / "targets" / ("linux-%s.json" % build)).write_text(json.dumps(
            {"id": "linux-%s" % build, "platform": "linux", "revision": str(build), "default": default,
             "support": {"status": "candidate"}}))

    pins = pins_for(tmp, json.loads((ROOT / "core/pins/pins.json").read_text())["builds"])
    target(25639945, True)
    code, _ = run("--pins", pins, "catalog", "--catalog", cat)
    check(code == 0, "catalog: pinned default target passes")
    target(25700000, False)
    code, out = run("--pins", pins, "catalog", "--catalog", cat)
    check(code == 1 and "linux-25700000" in out, "catalog: an unpinned new target fails (re-pin needed)")
    (cat / "targets/linux-25700000.json").unlink()
    (cat / "targets/linux-25639945.json").unlink()
    target(25700000, True)
    code, out = run("--pins", pins, "catalog", "--catalog", cat)
    check(code == 1 and "DEFAULT target" in out and "no target linux-25639945" in out,
          "catalog: unpinned default and a pin without target both fail")
    print("  %s catalog: pinned default passes; an unpinned target, an unpinned default and a pin without target fail"
          % ("PASS" if code == 1 else "FAIL"))

    fx = Path(tmp) / "fixtures"
    fx.mkdir()
    code, out = run("fixtures", "--dir", fx)
    check(code == 1 and "has no anchor fixture" in out, "fixtures: a pinned build without a fixture fails")


def main():
    tmp = tempfile.mkdtemp(prefix="repin-test-")
    try:
        print("== synthetic binaries")
        test_binaries(tmp)
        print("== reflection diff")
        test_reflection(tmp)
        print("== code, catalog, fixtures")
        test_code_and_catalog(tmp)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    for f in failures:
        print("FAIL " + f)
    print("%s repin tests: %d/%d checks" % ("FAIL" if failures else "PASS", passes[0], passes[0] + len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
