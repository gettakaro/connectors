#!/usr/bin/env python3
"""Steam build watch for the native Conan Exiles library.

Reads the public branch build of app 443030 (steamcmd app_info) and compares it with the builds
pinned in core/pins/pins.json for every platform that has signatures. When the public build is
not pinned, the native library refuses that server (no hook, one critical notice, structured
errors), so this opens a GitHub issue "conan-exiles: re-pin needed (Steam build N)" with the
procedure. One issue per build: an issue with the same title, open or closed, is never repeated.

  buildwatch.py                               # live app_info through steamcmd, dry run
  buildwatch.py --app-info appinfo.vdf        # from a saved `app_info_print` output
  buildwatch.py --publish --repo owner/name   # really open the issue (needs GH_TOKEN for gh)

Dry run is the default: it prints the gh command it would run and never calls gh. Writes
`buildid=` and `repin_needed=` to $GITHUB_OUTPUT when set. Exit 0 unless the build cannot be read.
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
NATIVE = HERE.parent
APP = 443030
DEPOTS = {"linux": "443032", "windows": "443031"}
TITLE = "conan-exiles: re-pin needed (Steam build %s)"


def parse_vdf(text):
    """Valve KeyValues text (what app_info_print prints) into nested dicts."""
    tokens = re.findall(r'"((?:[^"\\]|\\.)*)"|([{}])', text)
    stack, cur, key = [], {}, None
    root = cur
    for quoted, brace in tokens:
        if brace == "{":
            new = {}
            cur[key] = new
            stack.append(cur)
            cur, key = new, None
        elif brace == "}":
            cur = stack.pop() if stack else root
        elif key is None:
            key = quoted
        else:
            cur[key] = quoted
            key = None
    return root


def app_info_text(args):
    if args.app_info:
        return Path(args.app_info).read_text(errors="replace")
    cmd = ["+login", "anonymous", "+app_info_update", "1", "+app_info_print", str(APP), "+quit"]
    if shutil.which("steamcmd"):
        cmd = ["steamcmd"] + cmd
    else:
        cmd = ["docker", "run", "--rm", args.steamcmd_image] + cmd
    return subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=900,
                          universal_newlines=True).stdout


def read_build(text, branch):
    start = text.find('"%d"' % APP)
    if start < 0:
        raise SystemExit("app_info has no block for app %d (steamcmd output follows)\n%s" % (APP, text[-2000:]))
    info = parse_vdf(text[start:]).get(str(APP), {})
    depots = info.get("depots", {})
    build = depots.get("branches", {}).get(branch, {}).get("buildid")
    if not build:
        raise SystemExit("app_info has no buildid for branch %r" % branch)
    manifests = {p: depots.get(d, {}).get("manifests", {}).get(branch, {}).get("gid") for p, d in DEPOTS.items()}
    return build, manifests


def issue_body(build, branch, missing, pinned, manifests):
    rows = "\n".join("| %s | %s | %s | %s |" % (p, DEPOTS[p], manifests.get(p) or "?",
                                                ", ".join(pinned.get(p, [])) or "none")
                     for p in sorted(set(missing) | set(pinned)))
    return """Steam app %d, branch `%s`, moved to build **%s**. The native library is not pinned to it on: **%s**.
Until it is re-pinned, the library refuses that server cleanly (no hook, one critical notice in Takaro,
every action answered with a structured error); it never runs unpinned.

| Platform | Depot | Manifest (%s) | Pinned builds |
| --- | --- | --- | --- |
%s

## Re-pin
1. Get the new server binary (only that file, about 200 MB) with DepotDownloader or steamcmd
   `download_depot %d <depot> <manifest>`.
2. `games/conan-exiles/native/tools/repin.py pin --binary <new> --previous <last pinned binary> --build %s`
   - exit 0: the signatures still match once; exit 3: re-derived, review the new patterns;
     exit 1: read the NEEDS A HUMAN list.
3. Start the new build with the probe library, take a dump (`POST /dump`), and rerun with `--dump`
   (or `--probe probe.sh` on the running instance) so the reflected names are checked too.
4. `--write` updates pins.json; copy the printed row (and any new signatures) into `core/pins/pins.cpp`,
   write the anchor fixture (`--fixture $CONAN_ANCHOR_FIXTURES/<platform>-%s.anchors`, private runner copy), add the catalog target,
   run `make -C games/conan-exiles/native test`.
5. Live smoke on the new build before release (identify, chat, getPlayers).

Opened by `games/conan-exiles/native/tools/buildwatch.py` (scheduled workflow `conan-exiles-repin-watch`).
""" % (APP, branch, build, ", ".join(missing), branch, rows, APP, build, build)


def gh(args, *argv):
    return subprocess.run([args.gh] + list(argv), stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          universal_newlines=True, check=True).stdout


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--app-info", help="saved app_info_print output instead of running steamcmd")
    ap.add_argument("--steamcmd-image", default="steamcmd/steamcmd:latest")
    ap.add_argument("--branch", default="public")
    ap.add_argument("--pins", default=str(NATIVE / "core/pins/pins.json"))
    ap.add_argument("--repo", default=os.environ.get("GITHUB_REPOSITORY", ""))
    ap.add_argument("--publish", action="store_true", help="really open the issue (default: dry run)")
    ap.add_argument("--gh", default="gh", help="gh executable (tests pass a fake)")
    args = ap.parse_args(argv)

    build, manifests = read_build(app_info_text(args), args.branch)
    pins = json.loads(Path(args.pins).read_text())
    native = sorted(p for p, sigs in pins["signatures"].items() if sigs)
    pinned = {}
    for b in pins["builds"]:
        pinned.setdefault(b["platform"], []).append(str(b["build"]))
    missing = [p for p in native if build not in pinned.get(p, [])]
    print("steam app %d branch %s: build %s (manifests %s)" % (APP, args.branch, build,
                                                              ", ".join("%s=%s" % kv for kv in sorted(manifests.items()))))
    print("pinned: %s" % (", ".join("%s=%s" % (p, "/".join(v)) for p, v in sorted(pinned.items())) or "none"))
    out = os.environ.get("GITHUB_OUTPUT")
    if out:
        with open(out, "a") as f:
            f.write("buildid=%s\nrepin_needed=%s\n" % (build, "true" if missing else "false"))
    if not missing:
        print("up to date: build %s is pinned for %s" % (build, ", ".join(native)))
        return 0

    title = TITLE % build
    body = issue_body(build, args.branch, missing, pinned, manifests)
    print("RE-PIN NEEDED on %s: %s" % (", ".join(missing), title))
    if not args.publish:
        print("dry run: would check %s for an issue titled %r, then run:" % (args.repo or "<repo>", title))
        print("  %s issue create --repo %s --title %r --body-file -" % (args.gh, args.repo or "<repo>", title))
        print("---- issue body ----\n" + body)
        return 0
    if not args.repo:
        raise SystemExit("--publish needs --repo (or GITHUB_REPOSITORY)")
    existing = json.loads(gh(args, "issue", "list", "--repo", args.repo, "--state", "all", "--limit", "50",
                             "--search", 'in:title "%s"' % title, "--json", "number,title,state") or "[]")
    for i in existing:
        if i["title"] == title:
            print("issue #%s (%s) already tracks build %s; nothing to do" % (i["number"], i["state"].lower(), build))
            return 0
    url = subprocess.run([args.gh, "issue", "create", "--repo", args.repo, "--title", title, "--body-file", "-"],
                         input=body, stdout=subprocess.PIPE, universal_newlines=True, check=True).stdout.strip()
    print("opened " + url)
    return 0


if __name__ == "__main__":
    sys.exit(main())
