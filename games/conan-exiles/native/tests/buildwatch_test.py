#!/usr/bin/env python3
"""tools/buildwatch.py against the committed app_info of app 443030 (public build 25639945, saved
2026-10-03) and a fake `gh`, so no test ever talks to Steam or opens a real issue.
Usage: buildwatch_test.py   (python 3.7+)
"""
import io
import json
import os
import shutil
import stat
import sys
import tempfile
from contextlib import redirect_stdout
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import buildwatch  # noqa: E402

APPINFO = ROOT / "tests/fixtures/appinfo-443030.vdf"
FAKE_GH = r'''#!/usr/bin/env python3
import json, os, sys
log = os.environ["FAKE_GH_LOG"]
with open(log, "a") as f:
    f.write(json.dumps(sys.argv[1:]) + "\n")
if sys.argv[1:3] == ["issue", "list"]:
    print(os.environ.get("FAKE_GH_LIST", "[]"))
elif sys.argv[1:3] == ["issue", "create"]:
    with open(log + ".body", "w") as f:
        f.write(sys.stdin.read())
    print("https://github.com/example/repo/issues/999")
'''
failures, passes = [], [0]


def check(ok, what):
    if ok:
        passes[0] += 1
        print("  PASS " + what)
    else:
        failures.append(what)
        print("  FAIL " + what)


def run(*argv):
    out = io.StringIO()
    with redirect_stdout(out):
        code = buildwatch.main([str(a) for a in argv])
    return code, out.getvalue()


def main():
    tmp = Path(tempfile.mkdtemp(prefix="buildwatch-test-"))
    try:
        gh = tmp / "gh"
        gh.write_text(FAKE_GH)
        gh.chmod(gh.stat().st_mode | stat.S_IEXEC)
        log = tmp / "gh.log"
        os.environ["FAKE_GH_LOG"] = str(log)
        os.environ.pop("GITHUB_OUTPUT", None)

        build, manifests = buildwatch.read_build(APPINFO.read_text(), "public")
        check(build == "25639945" and manifests == {"linux": "8611640520811009059", "windows": "6353629323845191319"},
              "app_info parsed: public build %s, manifests %s" % (build, manifests))

        code, out = run("--app-info", APPINFO, "--gh", gh, "--repo", "example/repo", "--publish")
        check(code == 0 and "up to date" in out and not log.exists(), "pinned public build: no gh call, no issue")

        moved = tmp / "moved.vdf"
        moved.write_text(APPINFO.read_text().replace('"buildid"\t\t"25639945"', '"buildid"\t\t"25700001"'))
        gho = tmp / "gh-output"
        os.environ["GITHUB_OUTPUT"] = str(gho)
        code, out = run("--app-info", moved, "--gh", gh, "--repo", "example/repo")
        os.environ.pop("GITHUB_OUTPUT")
        check(code == 0 and "dry run" in out and "conan-exiles: re-pin needed (Steam build 25700001)" in out
              and not log.exists(), "moved build, dry run: prints the issue, never calls gh")
        check("buildid=25700001" in gho.read_text() and "repin_needed=true" in gho.read_text(),
              "moved build: GITHUB_OUTPUT carries buildid and repin_needed")

        code, out = run("--app-info", moved, "--gh", gh, "--repo", "example/repo", "--publish")
        calls = [json.loads(l) for l in log.read_text().splitlines()]
        body = Path(str(log) + ".body").read_text()
        check(code == 0 and [c[:2] for c in calls] == [["issue", "list"], ["issue", "create"]]
              and "conan-exiles: re-pin needed (Steam build 25700001)" in calls[1]
              and "repin.py pin --binary" in body and "8611640520811009059" in body,
              "moved build, publish (fake gh): searches, then opens one issue with the procedure")

        log.unlink()
        os.environ["FAKE_GH_LIST"] = json.dumps([{"number": 7, "title": "conan-exiles: re-pin needed (Steam build 25700001)",
                                                  "state": "CLOSED"}])
        code, out = run("--app-info", moved, "--gh", gh, "--repo", "example/repo", "--publish")
        calls = [json.loads(l) for l in log.read_text().splitlines()]
        check(code == 0 and len(calls) == 1 and "already tracks" in out,
              "an issue for the same build (even closed) is never repeated")
    finally:
        shutil.rmtree(str(tmp), ignore_errors=True)
    print("%s buildwatch tests: %d/%d checks" % ("FAIL" if failures else "PASS", passes[0], passes[0] + len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
