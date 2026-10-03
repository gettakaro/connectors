#!/usr/bin/env python3
"""Re-pin the native Conan Exiles library to a new server build.

The library finds three (or more) raw engine globals with a byte-signature scan at startup and
accepts a build only when its identity is pinned in core/pins (pins.json + pins.cpp) and the scan
reproduces the pinned addresses. Everything else comes from UE reflection at runtime, so a new
build needs two things checked: the anchors (this tool re-derives them from the binary) and the
reflected names the core relies on (this tool diffs them against a reflection dump of the new
build, from the probe library).

Subcommands (all read-only unless --write is given):

  pin       --binary NEW [--previous OLD] [--anchor NAME=ADDR ...] [--build STEAMBUILD]
            [--dump DUMP.json[.gz] | --probe probe.sh] [--write] [--fixture OUT.anchors]
            Re-derive every anchor for NEW, check it, diff reflection, print the pins.json entry.
            ELF (Linux, absolute addresses) and PE (Windows, RVAs; give --anchor hints from the
            W0 runtime autodetect when no Windows signature exists yet).
  reflect   --dump DUMP | --probe probe.sh     only the reflection diff against the manifest
  code      the manifest (tools/reflection-manifest.json) covers every name the core looks up
  catalog   --catalog DIR                       pins.json agrees with catalog/conan-exiles
  fixture   --binary B --out F.anchors [--build N]   write a small anchor fixture for CI
  fixtures  every pinned build has a fixture and the fixture agrees with pins.json
  baseline  --dump DUMP                          refresh the manifest's recorded values

Exit codes: 0 clean; 3 re-derived automatically (signatures changed: review, then copy into
pins.cpp); 1 needs a human (see the NEEDS A HUMAN list); 2 usage error.
"""
import argparse
import gzip
import json
import re
import struct
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
NATIVE = HERE.parent
sys.path.insert(0, str(HERE))
import sigderive as sd  # noqa: E402

PINS = NATIVE / "core/pins/pins.json"
PINS_CPP = NATIVE / "core/pins/pins.cpp"
MANIFEST = HERE / "reflection-manifest.json"
FIXTURES = NATIVE / "tests/fixtures/anchors"

EXIT_CLEAN, EXIT_HUMAN, EXIT_USAGE, EXIT_REDERIVED = 0, 1, 2, 3


# --------------------------------------------------------------------------- small helpers
def hx(v):
    return "0x%x" % v


def hexb(b):
    return " ".join("%02x" % x for x in b)


def load_json(path):
    path = Path(path)
    raw = path.read_bytes()
    if raw[:2] == b"\x1f\x8b":
        raw = gzip.decompress(raw)
    return json.loads(raw.decode("utf-8"))


def platform_of(img):
    return "linux" if img.kind == "elf" else "windows"


def to_pin(img, va):
    """Address as stored in pins.json: absolute on the non-PIE ELF, an RVA on the PE."""
    return va - img.base if img.kind == "pe" else va


def from_pin(img, value):
    return value + img.base if img.kind == "pe" else value


def parse_pattern(pattern):
    return [None if t == "??" else int(t, 16) for t in pattern.split()]


def longest_literal_run(pat):
    best, cur = (0, 0), None
    for i, b in enumerate(pat + [None]):
        if b is not None and cur is None:
            cur = i
        if b is None and cur is not None:
            if i - cur > best[1]:
                best = (cur, i - cur)
            cur = None
    return best


def find_all(hay, needle, limit):
    out, i = [], hay.find(needle)
    while i >= 0 and len(out) < limit:
        out.append(i)
        i = hay.find(needle, i + 1)
    return out


def prologue_from_cpp(path=PINS_CPP):
    """kProcessEventPrologue from pins.cpp: the exact bytes the inline detour relocates."""
    try:
        m = re.search(r"kProcessEventPrologue\[[^\]]*\]\s*=\s*\{([^}]*)\}", Path(path).read_text())
    except OSError:
        return None
    return bytes(int(v, 16) for v in re.findall(r"0x([0-9a-fA-F]{1,2})", m.group(1))) if m else None


# --------------------------------------------------------------------------- anchor work
def resolve_sig(img, sig):
    """(status, value_va, match_off, detail) for one pins.json signature on img."""
    hits = sd.matches(img, sig["pattern"], 3)
    if len(hits) != 1:
        return ("no match" if not hits else "%d matches" % len(hits)), None, None, ""
    if img.kind == "pe" and sig["capture"] == "abs32":
        return "abs32 capture on a relocatable PE", None, hits[0], ""
    return "ok", sd.resolve(img, hits[0], sig["capture"], sig["offset"]), hits[0], ""


def xref_sites(img, target, kinds, limit=400):
    """Offsets in .text of 4-byte fields that reference target (rip32: field + 4 + disp == target;
    abs32: field == target). Fast without numpy: for each 64 KiB block the high half of every
    possible rip displacement takes at most two values, so a bytes.find on those two bytes finds
    every candidate."""
    text, base, out = img.text, img.text_addr, []
    if "abs32" in kinds and target < 1 << 32:
        for o in find_all(text, struct.pack("<I", target), limit):
            out.append((o, "abs32"))
    if "rip32" in kinds:
        block = 0x10000
        for b0 in range(0, len(text), block):
            lo_disp = target - (base + b0 + block + 4)
            hi_disp = target - (base + b0 + 4)
            if hi_disp < -(1 << 31) or lo_disp >= 1 << 31:
                continue
            highs = {(v >> 16) & 0xFFFF for v in (lo_disp, hi_disp)}
            seg_lo, seg_hi = max(0, b0 - 2), min(len(text), b0 + block + 4)
            for h in highs:
                needle = struct.pack("<H", h)
                j = text.find(needle, seg_lo, seg_hi)
                while j >= 0:
                    f = j - 2
                    if b0 <= f < b0 + block and f + 4 <= len(text):
                        v, = struct.unpack_from("<i", text, f)
                        if base + f + 4 + v == target:
                            out.append((f, "rip32"))
                    j = text.find(needle, j + 1, seg_hi)
            if len(out) >= limit:
                break
    return out[:limit]


def derive_start(img, va, max_len=48):
    pat = sd.shortest_unique(img, img.off(va), max_len)
    return {"pattern": pat, "capture": "start", "offset": 0} if pat else None


def derive_data(img, target_va, max_sites=200):
    """Shortest unique pattern over a code site that references target_va."""
    kinds = ("rip32",) if img.kind == "pe" else ("rip32", "abs32")
    best = None
    for f, kind in xref_sites(img, target_va, kinds, max_sites):
        for back in (3, 2, 4, 1, 5, 6, 7, 8):
            s = f - back
            if s < 0:
                continue
            pat = sd.shortest_unique(img, s, 48, back)
            if pat and (best is None or len(pat.split()) < len(best["pattern"].split())):
                best = {"pattern": pat, "capture": kind, "offset": back}
                break
        if best and len(best["pattern"].split()) <= 16:
            break
    return best


def relocate(old, new, sig, old_off, min_votes=3):
    """Find the old match site in the new binary: masked windows of the old bytes around it are
    searched in the new binary, and every window that matches exactly once votes for a new site.
    A site needs min_votes agreeing windows; a single window landing on some other function's
    generic prologue must not move an anchor. Returns (new match offset, votes) or (None, votes)."""
    span = max(64, len(sig["pattern"].split()) + 16)
    lo = max(0, old_off - 160)
    buf = old.text[lo:old_off + span + 160]
    mask = sd.mask_for(old, lo, buf)
    rel = old_off - lo
    votes = {}
    for length in (64, 40):
        for d in range(-160, span + 160 - length, 16):
            s = rel + d
            if s < 0 or s + length > len(buf):
                continue
            while s < len(buf) and mask[s]:
                s += 1
            if s + length > len(buf):
                continue
            pat = sd.pattern_text(buf[s:s + length], mask[s:s + length])
            if pat.split().count("??") * 2 > length:
                continue
            hits = sd.matches(new, pat, 2)
            if len(hits) == 1:
                site = hits[0] - (s - rel)
                votes[site] = votes.get(site, 0) + 1
    if not votes:
        return None, 0
    site, n = max(votes.items(), key=lambda kv: kv[1])
    return (site if n >= min_votes else None), n


def rederive(new, sig, old=None, hint=None):
    """(signature, value_va, how) or (None, None, why)."""
    data = sig["capture"] != "start"
    if hint is not None:
        va = from_pin(new, hint)
        out = derive_data(new, va) if data else derive_start(new, va)
        if out:
            return out, va, "from the --anchor hint"
        return None, None, "no unique pattern near the --anchor hint"
    if old is None:
        return None, None, "give --previous <old binary> or --anchor %s=<address>" % sig["anchor"]
    status, _, old_off, _ = resolve_sig(old, sig)
    if status != "ok":
        return None, None, "the current signature does not match the --previous binary either (%s)" % status
    new_off, votes = relocate(old, new, sig, old_off)
    if new_off is None:
        return None, None, ("the code around the old match was not found in the new binary (best site had %d of the "
                            "3 agreeing windows needed)" % votes)
    if data:
        va = sd.resolve(new, new_off, sig["capture"], sig["offset"])
        pat = sd.shortest_unique(new, new_off, max(48, sig["offset"] + 8), sig["offset"])
        out = {"pattern": pat, "capture": sig["capture"], "offset": sig["offset"]} if pat else derive_data(new, va)
    else:
        va = new.text_addr + new_off + sig["offset"]
        out = derive_start(new, va)
    if not out:
        return None, None, "relocated to %s but no unique pattern there" % hx(va)
    return out, va, "relocated from the --previous binary (%d agreeing windows)" % votes


# --------------------------------------------------------------------------- reflection
class DumpSource:
    def __init__(self, path):
        d = load_json(path)
        self.meta = d.get("meta", {})
        self.layout = d.get("layout", {})
        self.structs = {s["name"]: s for s in d.get("structs", [])}
        self.label = "dump %s" % Path(path).name

    def struct(self, name):
        return self.structs.get(name)

    def field_types(self):
        return {p["type"] for s in self.structs.values() for p in s.get("properties", [])}


class ProbeSource:
    """Live: the probe REPL's read-only /class endpoint (off the game thread)."""

    def __init__(self, cmd):
        self.cmd, self.cache, self.meta, self.layout = cmd, {}, {}, {}
        self.label = "live probe (%s)" % cmd
        try:
            self.meta = json.loads(subprocess.check_output([cmd, "GET", "/health"], timeout=60))
        except Exception as e:  # noqa: BLE001 - any failure means "no live instance"
            raise SystemExit("probe not reachable through %s: %s" % (cmd, e))

    def struct(self, name):
        if name not in self.cache:
            try:
                out = subprocess.check_output([self.cmd, "GET", "/class?name=" + name], timeout=120)
                d = json.loads(out)
                self.cache[name] = d if d.get("name") == name else None
            except Exception:  # noqa: BLE001
                self.cache[name] = None
        return self.cache[name]

    def field_types(self):
        return {p["type"] for s in self.cache.values() if s for p in s.get("properties", [])}


def lookup(src, e):
    """The live record for a manifest entry, or None."""
    if e["kind"] in ("class", "struct"):
        return src.struct(e["name"])
    owner = src.struct(e["owner"])
    if not owner:
        return None
    key = "properties" if e["kind"] == "property" else "functions"
    for item in owner.get(key, []):
        if item["name"].lower() == e["name"].lower():
            return item
    return None


def observed(e, rec):
    if e["kind"] in ("class", "struct"):
        return {"size": rec.get("size")}
    if e["kind"] == "property":
        return {"type": rec.get("type"), "size": rec.get("size"), "offset": rec.get("offset")}
    return {"parmsSize": rec.get("parmsSize"),
            "params": [[p["name"], p["type"], p["offset"], p["size"]] for p in rec.get("params", [])]}


def reflect_diff(src, manifest):
    """(rows, blockers). rows: (status, entry label, detail)."""
    rows, blockers = [], []
    entries = manifest["entries"]
    for e in entries:
        if e["kind"] == "fieldclass":
            continue
        label = e["name"] if e["kind"] in ("class", "struct") else "%s.%s" % (e["owner"], e["name"])
        label = "%-8s %s" % (e["kind"], label)
        rec = lookup(src, e)
        if rec is None:
            rows.append(("MISSING", label, "used by " + e["usedBy"]))
            blockers.append("%s is gone (used by %s)" % (label.strip(), e["usedBy"]))
            continue
        now, was = observed(e, rec), e.get("baseline", {})
        changed = {k: (was.get(k), now.get(k)) for k in now if k in was and was[k] != now[k]}
        if not changed:
            rows.append(("ok", label, ""))
            continue
        desc = ", ".join("%s %s -> %s" % (k, a, b) for k, (a, b) in sorted(changed.items()) if k != "params")
        if "params" in changed:
            desc = (desc + ", " if desc else "") + "parameter layout changed"
        # A fixed layout is a constant in the code, and a property's type and size are checked by the
        # core at runtime (PropertyOffset refuses a mismatch): both need a person. Anything else of a
        # runtime-resolved entry (a moved offset, a grown class) is only information.
        hard = e.get("layout") == "fixed" or (e["kind"] == "property" and ("type" in changed or "size" in changed))
        if hard:
            rows.append(("CHANGED", label, desc))
            blockers.append("%s: %s (%s layout; used by %s)" % (label.strip(), desc, e.get("layout", "runtime"),
                                                                e["usedBy"]))
        else:
            rows.append(("moved", label, desc + " (resolved at runtime)"))
    types = src.field_types()
    for e in entries:
        if e["kind"] == "fieldclass":
            ok = e["name"] in types
            rows.append(("ok" if ok else "MISSING", "%-8s %s" % ("field", e["name"]), ""))
            if not ok:
                blockers.append("property type %s not seen" % e["name"])
    return rows, blockers


def layout_crosscheck(src, anchors, img):
    """The probe dump records the globals it ran with; on the same build they must agree."""
    keys = {"processEvent": "ProcessEvent", "objObjects": "GObjObjects", "nameBlocks": "FNamePoolBlocks"}
    out = []
    if src.meta.get("buildId") and src.meta.get("buildId") != img.identity:
        return [("info", "dump is from build-id %s, not this binary; address cross-check skipped" %
                 src.meta.get("buildId"))]
    for name, va in anchors.items():
        text = src.layout.get(keys.get(name, ""), "")
        m = re.match(r"\s*(0x[0-9a-fA-F]+)", text)
        if not m:
            continue
        live = int(m.group(1), 16)
        out.append(("ok" if live == to_pin(img, va) else "MISMATCH", "%s: scan %s, probe %s" %
                    (name, hx(to_pin(img, va)), hx(live))))
    return out


# --------------------------------------------------------------------------- the pin command
def cmd_pin(a):
    pins = load_json(a.pins)
    img = sd.Image(a.binary)
    plat = platform_of(img)
    old = sd.Image(a.previous) if a.previous else None
    hints = {}
    for h in a.anchor or []:
        k, _, v = h.partition("=")
        if not v:
            raise SystemExit("--anchor wants NAME=ADDRESS, got %r" % h)
        hints[k] = int(v, 0)
    sigs = list(pins["signatures"].get(plat, []))
    names = [s["anchor"] for s in sigs]
    for k in hints:  # a hint for an anchor that has no signature yet (a new platform, a 4th anchor)
        if k not in names:
            cap = "start" if k in a.start_anchors.split(",") else "rip32"
            sigs.append({"anchor": k, "pattern": "", "capture": cap, "offset": 0})
            names.append(k)
    builds = [b for b in pins["builds"] if b["platform"] == plat]
    pinned = next((b for b in builds if b["buildId"] == img.identity), None)
    prev_pin = None
    if old is not None:
        prev_pin = next((b for b in builds if b["buildId"] == old.identity), None)
    if prev_pin is None and builds:
        prev_pin = builds[-1]

    human, notes = [], []
    print("repin: %s" % a.binary)
    print("  format     %s (%s), .text %s, %d MB" % (img.kind.upper(), plat, hx(img.text_addr), len(img.text) >> 20))
    print("  identity   %s%s" % (img.identity or "(none)", "  (%s code id)" % plat if img.kind == "pe" else ""))
    if pinned:
        print("  pinned     yes: Steam build %s" % pinned["build"])
    else:
        print("  pinned     no%s" % (" (pins.json has %s)" % ", ".join("%s=%s" % (b["build"], b["buildId"][:12])
                                                                        for b in builds) if builds else ""))
    if not img.identity:
        human.append("the binary has no build identity; the library cannot pin it")

    print("\nanchors")
    anchors, new_sigs, changed_sigs = {}, [], False
    for sig in sigs:
        name = sig["anchor"]
        if sig["pattern"]:
            status, va, _, _ = resolve_sig(img, sig)
        else:
            status, va = "no signature yet", None
        if status == "ok":
            anchors[name] = va
            new_sigs.append(sig)
            how = "signature unchanged, matches once"
        else:
            out, va, how = rederive(img, sig, old, hints.get(name))
            if out is None:
                print("  %-13s NEEDS HUMAN  %s: %s" % (name, status, how))
                human.append("%s: %s; %s" % (name, status, how))
                new_sigs.append(sig)
                continue
            anchors[name] = va
            new_sigs.append(dict(out, anchor=name))
            changed_sigs = True
            how = "%s -> re-derived %s; new pattern (%s offset %d)" % (status, how, out["capture"], out["offset"])
        prev = prev_pin["anchors"].get(name) if prev_pin else None
        moved = ""
        if prev is not None:
            delta = to_pin(img, va) - int(prev, 16)
            moved = "unchanged vs %s" % prev_pin["build"] if not delta else "moved %+#x vs %s" % (delta, prev_pin["build"])
        print("  %-13s %-12s %s; %s" % (name, hx(to_pin(img, va)), how, moved))
        if hints.get(name) is not None and to_pin(img, va) != hints[name]:
            human.append("%s: the scan resolves %s but the --anchor hint says %s" % (name, hx(to_pin(img, va)),
                                                                                     hx(hints[name])))

    print("\nchecks")
    for sig in new_sigs:
        name = sig["anchor"]
        if name not in anchors:
            continue
        va = anchors[name]
        sec = img.section_of(va)
        if sig["capture"] == "start":
            ok = sec == ".text"
            print("  %-4s %s is code (%s)" % ("OK" if ok else "FAIL", name, sec or "unmapped"))
        else:
            ok = img.writable(va)
            print("  %-4s %s is writable data (%s)" % ("OK" if ok else "FAIL", name, sec or "unmapped"))
        if not ok:
            human.append("%s resolves into %s; a %s anchor must be %s" % (
                name, sec or "unmapped memory", "function" if sig["capture"] == "start" else "data",
                ".text" if sig["capture"] == "start" else "writable data"))
    if "processEvent" in anchors:
        want = prologue_from_cpp()
        got = img.text[img.off(anchors["processEvent"]):][:len(want) if want else 20]
        if plat == "linux" and want:
            ok = got == want
            print("  %-4s ProcessEvent prologue equals the %d bytes the detour relocates (%s)" % (
                "OK" if ok else "FAIL", len(want), hexb(got)))
            if not ok:
                human.append("ProcessEvent prologue changed (%s): platform/linux/hook.cpp relocates exactly %s; "
                             "check every instruction is position independent, then update "
                             "kProcessEventPrologue" % (hexb(got), hexb(want)))
        else:
            print("  info ProcessEvent prologue %s (the %s hook decides what it can relocate)" % (hexb(got), plat))
    if prev_pin and old is None and not pinned:
        notes.append("no --previous binary: re-derivation of a broken signature needs it (or --anchor hints)")

    src = None
    if a.dump:
        src = DumpSource(a.dump)
    elif a.probe:
        src = ProbeSource(a.probe)
    print("\nreflection")
    if src is None:
        print("  skipped: pass --dump <probe dump of this build> or --probe <probe.sh> for a running instance")
        notes.append("reflection not checked")
    else:
        manifest = load_json(a.manifest)
        rows, blockers = reflect_diff(src, manifest)
        print("  source %s%s" % (src.label, ", build-id %s" % src.meta["buildId"] if src.meta.get("buildId") else ""))
        for status, label, detail in rows:
            if status != "ok" or a.verbose:
                print("  %-8s %s%s" % (status, label, "  " + detail if detail else ""))
        n_ok = sum(1 for r in rows if r[0] == "ok")
        print("  %d/%d manifest entries unchanged" % (n_ok, len(rows)))
        human.extend("reflection: " + b for b in blockers)
        for status, line in layout_crosscheck(src, anchors, img):
            print("  %-8s %s" % (status, line))
            if status == "MISMATCH":
                human.append("probe disagrees with the scan: " + line)

    entry = {"platform": plat, "buildId": img.identity, "build": a.build or (pinned or {}).get("build", "?"),
             "anchors": {n: hx(to_pin(img, anchors[n])) for n in names if n in anchors}}
    if a.build is None and not pinned:
        human.append("pass --build <Steam build id> so the entry names its build")
    complete = len(entry["anchors"]) == len(names)
    if pinned and complete:
        same = pinned["anchors"] == entry["anchors"]
        print("\npinned entry %s the scan" % ("matches" if same else "DISAGREES with"))
        if not same:
            human.append("pins.json pins this build-id to %s but the scan gives %s" % (pinned["anchors"],
                                                                                      entry["anchors"]))

    print("\npins.json entry")
    print("  " + json.dumps(entry))
    if changed_sigs:
        print("\nnew %s signatures (pins.json \"signatures\".%s and kLinux/kWindows in pins.cpp)" % (plat, plat))
        for s in new_sigs:
            print("  " + json.dumps({k: s[k] for k in ("anchor", "pattern", "capture", "offset")}))
    print("\npins.cpp row (kPinned; tests/drift_test.py fails until pins.cpp equals pins.json)")
    print('  {"%s", "%s", "%s", {%s}},' % (plat, entry["buildId"], entry["build"],
                                          ", ".join(entry["anchors"].get(n, "0") for n in names)))

    if a.write and complete and not human:
        if changed_sigs:
            pins["signatures"][plat] = [{k: s[k] for k in ("anchor", "pattern", "capture", "offset")}
                                        for s in new_sigs]
        pins["builds"] = [b for b in pins["builds"] if not (b["platform"] == plat and b["buildId"] == img.identity)]
        pins["builds"].append(entry)
        Path(a.pins).write_text(json.dumps(pins, indent=2) + "\n")
        print("\nwrote %s" % a.pins)
    elif a.write:
        print("\nnot written: resolve the NEEDS A HUMAN items first")
    if a.fixture and complete:
        write_fixture(img, new_sigs, entry, a.fixture)
        print("wrote fixture %s" % a.fixture)

    if a.json:
        Path(a.json).write_text(json.dumps({"entry": entry, "signatures": new_sigs, "signaturesChanged": changed_sigs,
                                            "needsHuman": human, "notes": notes}, indent=2) + "\n")
    print("\nresult")
    if human:
        print("  NEEDS A HUMAN:")
        for h in human:
            print("   - " + h)
    for n in notes:
        print("  note: " + n)
    if human:
        return EXIT_HUMAN
    if changed_sigs:
        print("  RE-DERIVED: signatures changed; review the new patterns, copy them into pins.cpp, run make test")
        return EXIT_REDERIVED
    print("  CLEAN: %s" % ("already pinned and reproduced" if pinned and complete else
                          "every anchor resolved by the current signatures; add the entry and the pins.cpp row"))
    return EXIT_CLEAN


# --------------------------------------------------------------------------- fixtures
def fixture_regions(img, sigs, decoys=48, pad=32):
    """Byte windows CI scans instead of the 200 MB binary: each signature's real match (padded),
    plus its nearest decoys (sites sharing the pattern's longest literal run, ranked by how many
    pattern bytes they match), so match-exactly-once is tested against the real near misses."""
    wins = []
    for sig in sigs:
        pat = parse_pattern(sig["pattern"])
        n = len(pat)
        for h in sd.matches(img, sig["pattern"], 3):
            wins.append((h - pad, h + n + pad))
        start, length = longest_literal_run(pat)
        needle = bytes(pat[start:start + length])
        scored = []
        for o in find_all(img.text, needle, 200000):
            s = o - start
            if s < 0 or s + n > len(img.text):
                continue
            score = sum(1 for k, b in enumerate(pat) if b is not None and img.text[s + k] == b)
            if score < sum(1 for b in pat if b is not None):
                scored.append((-score, s))
        for _, s in sorted(scored)[:decoys]:
            wins.append((s, s + n))
    wins = sorted((max(0, lo), min(len(img.text), hi)) for lo, hi in wins)
    merged = []
    for lo, hi in wins:
        if merged and lo <= merged[-1][1]:
            merged[-1][1] = max(merged[-1][1], hi)
        else:
            merged.append([lo, hi])
    return [(img.text_addr + lo, img.text[lo:hi]) for lo, hi in merged]


def write_fixture(img, sigs, entry, out):
    regions = fixture_regions(img, sigs)
    lines = ["# Anchor fixture: real bytes of a Conan Exiles server binary, windows around each signature's",
             "# match plus its nearest decoys. Generated by tools/repin.py; read by tests/pins_fixture_test.cpp.",
             "platform %s" % entry["platform"], "buildid %s" % entry["buildId"], "build %s" % entry["build"]]
    for name, v in sorted(entry["anchors"].items()):
        lines.append("expect %s %s" % (name, v))
    if img.kind == "pe":
        lines.append("imagebase %s" % hx(img.base))
    for addr, data in regions:
        for k in range(0, len(data), 64):
            lines.append("region %s %s" % (hx(addr + k), data[k:k + 64].hex()))
    Path(out).parent.mkdir(parents=True, exist_ok=True)
    Path(out).write_text("\n".join(lines) + "\n")
    return len(regions), sum(len(d) for _, d in regions)


def read_fixture(path):
    meta, expect, regions = {}, {}, []
    for line in Path(path).read_text().splitlines():
        if not line or line.startswith("#"):
            continue
        parts = line.split()
        if parts[0] == "expect":
            expect[parts[1]] = parts[2]
        elif parts[0] == "region":
            regions.append((int(parts[1], 16), bytes.fromhex(parts[2])))
        else:
            meta[parts[0]] = parts[1] if len(parts) > 1 else ""
    return meta, expect, regions


def cmd_fixture(a):
    pins = load_json(a.pins)
    img = sd.Image(a.binary)
    plat = platform_of(img)
    sigs = pins["signatures"].get(plat, [])
    anchors = {}
    for sig in sigs:
        status, va, _, _ = resolve_sig(img, sig)
        if status == "ok":
            anchors[sig["anchor"]] = hx(to_pin(img, va))
    entry = {"platform": plat, "buildId": img.identity, "build": a.build or "?", "anchors": anchors}
    if len(anchors) != len(sigs):
        print("note: %d of %d signatures resolve; the fixture records a build the library must refuse" %
              (len(anchors), len(sigs)))
    n, size = write_fixture(img, sigs, entry, a.out)
    print("wrote %s: %d regions, %d bytes, anchors %s" % (a.out, n, size, anchors))
    return EXIT_CLEAN


def cmd_fixtures(a):
    pins = load_json(a.pins)
    fails = []
    have = {}
    for f in sorted(Path(a.dir).glob("*.anchors")):
        meta, expect, _ = read_fixture(f)
        have[(meta.get("platform"), meta.get("buildid"))] = (f, expect)
    for b in pins["builds"]:
        got = have.get((b["platform"], b["buildId"]))
        if not got:
            fails.append("pinned %s build %s (%s) has no anchor fixture in %s" % (b["platform"], b["build"],
                                                                                   b["buildId"], a.dir))
        elif got[1] != b["anchors"]:
            fails.append("%s expects %s but pins.json pins %s" % (got[0].name, got[1], b["anchors"]))
    for f in fails:
        print("FAIL " + f)
    if not fails:
        print("PASS every pinned build has an anchor fixture that agrees with pins.json (%d build(s), %d fixture(s))" %
              (len(pins["builds"]), len(have)))
    return EXIT_HUMAN if fails else EXIT_CLEAN


# --------------------------------------------------------------------------- manifest <-> code
NAME_ARRAY = re.compile(r"\bk\w*Names?\w*\s*\[[^\]]*\]\s*=\s*\{(.*?)\};", re.S)
LOOKUP_CALL = re.compile(r"\b(?:Find|Lookup|Resolve)\w*(?:Class|Function|Func|Property|Prop|Struct|Enum|Name)s?\w*"
                         r"\s*\(([^;{}]*?)\)\s*[;,)]", re.S)
LITERAL = re.compile(r'"((?:[^"\\]|\\.)*)"')
IDENT = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")


def names_in_code(root):
    found = {}
    for f in sorted(Path(root).rglob("*.cpp")) + sorted(Path(root).rglob("*.h")):
        text = f.read_text(errors="replace")
        text = re.sub(r"//[^\n]*", "", text)
        for rx in (NAME_ARRAY, LOOKUP_CALL):
            for m in rx.finditer(text):
                for lit in LITERAL.findall(m.group(1)):
                    if IDENT.match(lit):
                        found.setdefault(lit, set()).add(str(f.relative_to(root)))
    return found


def manifest_names(manifest):
    out = set()
    for e in manifest["entries"]:
        for k in ("name", "owner"):
            if e.get(k):
                out.add(e[k].lower())
        for k in ("type",):
            if e.get("baseline", {}).get(k):
                out.add(e["baseline"][k].lower())
    out.update(n.lower() for n in manifest.get("engineNames", []))
    return out


def cmd_code(a):
    manifest = load_json(a.manifest)
    used = names_in_code(a.core)
    known = manifest_names(manifest)
    missing = sorted(n for n in used if n.lower() not in known)
    for n in missing:
        print("FAIL core looks up %r (%s) but %s does not list it; add an entry with its owner, kind and layout" %
              (n, ", ".join(sorted(used[n])), Path(a.manifest).name))
    if not used:
        print("FAIL found no reflected names in %s; the extractor no longer matches the code" % a.core)
        return EXIT_HUMAN
    if not missing:
        print("PASS reflection manifest covers all %d reflected names the core looks up" % len(used))
    return EXIT_HUMAN if missing else EXIT_CLEAN


def cmd_reflect(a):
    src = DumpSource(a.dump) if a.dump else ProbeSource(a.probe)
    rows, blockers = reflect_diff(src, load_json(a.manifest))
    print("reflection vs %s (%s)" % (Path(a.manifest).name, src.label))
    for status, label, detail in rows:
        if status != "ok" or a.verbose:
            print("  %-8s %s%s" % (status, label, "  " + detail if detail else ""))
    print("  %d/%d manifest entries unchanged" % (sum(1 for r in rows if r[0] == "ok"), len(rows)))
    if blockers:
        print("NEEDS A HUMAN:")
        for b in blockers:
            print("  - " + b)
        return EXIT_HUMAN
    print("CLEAN: every name the core needs is present with a compatible layout")
    return EXIT_CLEAN


def cmd_baseline(a):
    src = DumpSource(a.dump)
    manifest = load_json(a.manifest)
    missing = []
    for e in manifest["entries"]:
        if e["kind"] == "fieldclass":
            continue
        rec = lookup(src, e)
        if rec is None:
            missing.append(e)
            continue
        e["baseline"] = observed(e, rec)
    manifest["baselineFrom"] = {"build": src.meta.get("serverBuild"), "buildId": src.meta.get("buildId"),
                                "dump": Path(a.dump).name}
    Path(a.manifest).write_text(json.dumps(manifest, indent=2) + "\n")
    print("refreshed %d entries from %s%s" % (len(manifest["entries"]) - len(missing), src.label,
                                               "; MISSING: %s" % [e["name"] for e in missing] if missing else ""))
    return EXIT_HUMAN if missing else EXIT_CLEAN


# --------------------------------------------------------------------------- catalog consistency
def cmd_catalog(a):
    pins = load_json(a.pins)
    cat = Path(a.catalog)
    game = load_json(cat / "game.json")
    targets = [load_json(f) for f in sorted((cat / "targets").glob("*.json"))]
    fails, warns = [], []
    pinned = {(b["platform"], str(b["build"])) for b in pins["builds"]}
    native_platforms = {p for p, s in pins["signatures"].items() if s}
    for b in pins["builds"]:
        if not pins["signatures"].get(b["platform"]):
            fails.append("build %s is pinned for %s, which has no signatures" % (b["build"], b["platform"]))
        if not any(t["platform"] == b["platform"] and str(t["revision"]) == str(b["build"]) for t in targets):
            fails.append("pins.json pins %s build %s but catalog has no target %s-%s" % (
                b["platform"], b["build"], b["platform"], b["build"]))
        if b["platform"] not in game.get("platforms", []):
            warns.append("pins.json pins a %s build but catalog game.json lists platforms %s" % (
                b["platform"], game.get("platforms")))
    for t in targets:
        if t["platform"] not in native_platforms:
            continue
        status = t.get("support", {}).get("status", "")
        if status in ("retired", "unsupported", "withdrawn"):
            continue
        if (t["platform"], str(t["revision"])) not in pinned:
            what = "the DEFAULT target" if t.get("default") else "target"
            fails.append("%s %s (%s) is not pinned: the native library refuses that build. Re-pin it "
                         "(tools/repin.py pin) or retire the target" % (what, t["id"], status))
    for w in warns:
        print("WARN " + w)
    for f in fails:
        print("FAIL " + f)
    if not fails:
        print("PASS pins.json and catalog/%s agree (%d pinned build(s), %d target(s))" % (
            game.get("id"), len(pins["builds"]), len(targets)))
    return EXIT_HUMAN if fails else EXIT_CLEAN


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--pins", default=str(PINS))
    ap.add_argument("--manifest", default=str(MANIFEST))
    sub = ap.add_subparsers(dest="cmd")
    p = sub.add_parser("pin", help="re-derive the anchors of a new server binary")
    p.add_argument("--binary", required=True)
    p.add_argument("--previous", help="the last pinned binary, to relocate broken signatures automatically")
    p.add_argument("--anchor", action="append", metavar="NAME=ADDR",
                   help="known anchor address (absolute on ELF, RVA on PE), e.g. from the runtime autodetect")
    p.add_argument("--start-anchors", default="processEvent",
                   help="comma list of hinted anchors that are functions (others are data)")
    p.add_argument("--build", help="Steam build id of the binary, for the entry")
    p.add_argument("--dump", help="probe reflection dump of this build (.json or .json.gz)")
    p.add_argument("--probe", help="probe.sh of a running instance of this build (read-only /class queries)")
    p.add_argument("--write", action="store_true", help="update pins.json when nothing needs a human")
    p.add_argument("--fixture", help="also write the CI anchor fixture here")
    p.add_argument("--json", help="machine-readable report")
    p.add_argument("-v", "--verbose", action="store_true")
    r = sub.add_parser("reflect", help="reflection diff only")
    g = r.add_mutually_exclusive_group(required=True)
    g.add_argument("--dump")
    g.add_argument("--probe")
    r.add_argument("-v", "--verbose", action="store_true")
    c = sub.add_parser("code", help="manifest covers every reflected name in core/")
    c.add_argument("--core", default=str(NATIVE / "core"))
    k = sub.add_parser("catalog", help="pins.json agrees with the catalog targets")
    k.add_argument("--catalog", default=str(NATIVE.parent.parent.parent / "catalog/conan-exiles"))
    f = sub.add_parser("fixture", help="write a CI anchor fixture")
    f.add_argument("--binary", required=True)
    f.add_argument("--out", required=True)
    f.add_argument("--build")
    fs = sub.add_parser("fixtures", help="every pinned build has an agreeing fixture")
    fs.add_argument("--dir", default=str(FIXTURES))
    b = sub.add_parser("baseline", help="refresh the manifest's recorded values from a dump")
    b.add_argument("--dump", required=True)
    a = ap.parse_args(argv)
    if not a.cmd:
        ap.print_help()
        return EXIT_USAGE
    return {"pin": cmd_pin, "reflect": cmd_reflect, "code": cmd_code, "catalog": cmd_catalog,
            "fixture": cmd_fixture, "fixtures": cmd_fixtures, "baseline": cmd_baseline}[a.cmd](a)


if __name__ == "__main__":
    sys.exit(main())
