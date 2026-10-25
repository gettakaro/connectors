#!/usr/bin/env python3
"""Build locations.json, the gen_gamedata.py input, from the server export of tools/eml-locations.

    locations_json.py <export-dir> <source note> > locations.json

<export-dir> holds location-templates.tsv (template guid, code, kind, spawn target) and the
placements-<first>.tsv of each chunk range: a `#range first last total` line, then one row per
placement (chunk, template guid, then x, y, z as the integer and 1/65536 halves of the 1.0
SceneEntityTransform). Positions are metres, y up. The ranges must cover chunks 1..total exactly
once, or nothing is written.
"""
import glob, json, os, sys

exp, note = sys.argv[1], sys.argv[2]
tmpl = {}
for line in open(os.path.join(exp, 'location-templates.tsv'), encoding='utf-8'):
    f = line.rstrip('\n').split('\t')
    if len(f) >= 3 and f[0]:
        tmpl[f[0]] = (f[1], f[2], f[3] if len(f) > 3 and f[3] else None)
rows, ranges, totals = [], [], set()
for p in sorted(glob.glob(os.path.join(exp, 'placements-*.tsv')), key=lambda s: int(s.rsplit('-', 1)[1][:-4])):
    lines = open(p).read().splitlines()
    head = lines[0].split('\t') if lines else []
    if len(head) != 4 or head[0] != '#range':
        sys.exit(f'{p}: no #range line (exported by an older tools/eml-locations?)')
    ranges.append((int(head[1]), int(head[2]), p))
    totals.add(int(head[3]))
    for line in lines[1:]:
        f = line.split()
        if len(f) < 8:
            continue
        chunk, g = int(f[0]), f[1]
        x, y, z = (int(f[i]) + int(f[i + 1]) / 65536 for i in (2, 4, 6))
        code, kind, spawn = tmpl[g]
        rows.append({'code': code, 'templateGuid': g, 'kind': kind, 'spawnType': spawn,
                     'x': round(x, 2), 'y': round(y, 2), 'z': round(z, 2), 'chunk': chunk})
if len(totals) != 1:
    sys.exit(f'chunk totals differ between files: {sorted(totals)}')
want = 1
for first, last, p in sorted(ranges):
    if first != want:
        sys.exit(f'{p}: starts at chunk {first}, expected {want} (gap or overlap)')
    want = last + 1
if want - 1 != totals.pop():
    sys.exit(f'ranges end at chunk {want - 1}, the world has more')
json.dump({'source': note, 'locations': rows}, sys.stdout, indent=1)
print(len(rows), 'placements', file=sys.stderr)
