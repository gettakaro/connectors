#!/usr/bin/env python3
"""Fails when capabilities.json and native/src/plugin.cpp disagree on what the connector does.

Everything the plugin dispatches or emits must be listed as supported, and everything listed as
supported must exist in the plugin. testReachability is answered through SetReachability, not by name.
"""
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "native" / "src" / "plugin.cpp").read_text(encoding="utf-8")
capabilities = json.loads((ROOT / "capabilities.json").read_text(encoding="utf-8"))

actions = set(re.findall(r'call\.action == "([A-Za-z]+)"', source))
actions |= set(re.findall(r'action != "([A-Za-z]+)"', source))
if "g_client->SetReachability(" in source:
    actions.add("testReachability")
emitted = set(re.findall(r'Emit\("([a-z-]+)"', source))
declared = set(re.findall(r'"events\.([a-z-]+)"', source))

listed_actions = {k for k, v in capabilities["actions"].items() if v["status"] == "live-supported"}
listed_events = {k for k, v in capabilities["events"].items() if v["status"] == "live-supported"}

problems = []
for what, found, listed in (("action", actions, listed_actions), ("event", emitted, listed_events),
                            ("manifest event", declared, listed_events)):
    problems += [f"{what} {name} is in plugin.cpp but not supported in capabilities.json" for name in sorted(found - listed)]
    problems += [f"{what} {name} is supported in capabilities.json but not in plugin.cpp" for name in sorted(listed - found)]

for problem in problems:
    print(problem, file=sys.stderr)
if problems:
    sys.exit(1)
print(f"capabilities.json matches plugin.cpp: {len(actions)} actions, {len(emitted)} events")
