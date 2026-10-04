#!/usr/bin/env python3
"""Drift test: the tables compiled into the library must equal their published JSON copies.

  core/conan/capabilities.json == conan::RegistryJson()   (coverage registry)
  core/pins/pins.json          == pins::TableJson()       (startup signatures and pinned builds)

and the registry must cover exactly Takaro's 19 actions and 6 event types.
Usage: drift_test.py <harness binary>
"""
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ACTIONS = {'getPlayer', 'getPlayers', 'getPlayerLocation', 'getPlayerInventory', 'giveItem', 'listItems',
           'listEntities', 'listLocations', 'executeConsoleCommand', 'sendMessage', 'teleportPlayer',
           'testReachability', 'kickPlayer', 'banPlayer', 'unbanPlayer', 'listBans', 'shutdown', 'getMapInfo',
           'getMapTile'}
EVENTS = {'log', 'player-connected', 'player-disconnected', 'chat-message', 'player-death', 'entity-killed'}
STATUSES = {'live-supported', 'schema-fallback', 'unsupported', 'not-requested', 'pending'}


def main():
    harness = sys.argv[1]
    failures = []
    compiled = json.loads(subprocess.check_output([harness, '--registry'], text=True))
    published = json.loads((ROOT / 'core/conan/capabilities.json').read_text())
    for key in ('functions', 'events'):
        if compiled[key] != published[key]:
            diff = {k for k in set(compiled[key]) | set(published[key])
                    if compiled[key].get(k) != published[key].get(k)}
            failures.append(f'capabilities.json {key} differs from coverage.cpp: {sorted(diff)}')
    if set(published['functions']) != ACTIONS:
        failures.append(f'functions are not exactly the 19 Takaro actions: {set(published["functions"]) ^ ACTIONS}')
    if set(published['events']) != EVENTS:
        failures.append(f'events are not exactly the 6 Takaro events: {set(published["events"]) ^ EVENTS}')
    for section in ('functions', 'events'):
        for name, row in published[section].items():
            if row['status'] not in STATUSES:
                failures.append(f'{name}: status {row["status"]!r}')
            if row['status'] == 'pending':
                failures.append(f'{name}: still pending; classify it before a release')
            if row['implementation'] != 'native':
                failures.append(f'{name}: implementation {row["implementation"]!r}')
            if row['status'] == 'live-supported' and row['implementation'] != 'native':
                failures.append(f'{name}: live-supported without a native implementation')
            if not row['reason'] or not row['verification']:
                failures.append(f'{name}: reason and verification are required')

    pins_compiled = json.loads(subprocess.check_output([harness, '--pins'], text=True))
    pins_published = json.loads((ROOT / 'core/pins/pins.json').read_text())
    pins_published.pop('_comment', None)
    if pins_compiled != pins_published:
        failures.append('core/pins/pins.json differs from the table in core/pins/pins.cpp')

    for f in failures:
        print('FAIL ' + f)
    if not failures:
        print('PASS capabilities.json equals the compiled coverage registry (19 actions, 6 events)')
        print('PASS pins.json equals the compiled signature and build table')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
