#!/usr/bin/env python3
"""Generate src/entitylabels.cpp: the in-game English name of every entity template that has one.

The dedicated server ships no localisation, so every name comes from the game client. Export it
offline with the Enshrouded Mod Loader CLI (`emm run -e`) and the export mod in tools/eml-export/
(see gen_item_labels.py). The export writes, among others:

  templates-en.tsv       template guid, code, the template's own En_Us name, the component that
                         names it (boss health bar, NPC setup or dialog, loca tag), model, arsenal, faction
  logic-spawns-en.tsv    template guid, the name the Logic Spawner (1.0 building menu) lists it under
  item-entities-en.tsv   template guid, item code, item name, field: the item that places the entity
                         (farm animal, pet, baby animal, NPC) or that the fishing minigame catches
  loca-en.tsv            the whole En_Us table

A template gets the first of these that names it:

  client      its own name: a boss or mini-boss health bar, an NPC
  logicSpawn  the Logic Spawner entry that places it ("Rat", "Scavenger Scab", "Hollow Wretch")
  item        the item that is it ("Common Goat", "Hophare", a fish by its catch)
  model       the name of the named templates that use the same model, when they agree: what the
              player sees is that creature (a goat in the wild is the Common Goat it becomes in a pen)
  sibling     the name of the named templates whose code is the same once spawn and bookkeeping
              suffixes and the look number on a word are dropped (`Enemy_Scavenger_Melee07_Night_Patrol_Guard`
              is an `Enemy_Scavenger_Melee`), same faction, when they agree
  arsenal     the name of the named templates with the same attack arsenal and faction that share a
              creature word with its code, when they agree

model, sibling and arsenal repeat until nothing new is named. A template none of these names has no
honest in-game name: it is left out of listEntities, and an entity-killed or player-death event for
it carries the in-game word for its faction (Fell, Hollow, Scavenger, ...; kFactionLabels) or
"Unknown". Never a name built from the code.

    gen_entity_labels.py <export-dir> [build note] [--audit <file.tsv>]

The template table is src/gamedata.cpp's kEntities (the server's templates, gen_gamedata.py).
--audit writes every template: code, name, source, and whether listEntities lists it.
"""
import os
import re
import sys
import uuid
import json
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.normpath(os.path.join(HERE, '..', 'src'))
OUT = os.path.join(SRC, 'entitylabels.cpp')
NULL = '00000000-0000-0000-0000-000000000000'

# Words that say how a template is spawned, tracked or placed, not which creature it is.
NOISE = {'hook', 'hookpte', 'ag2', 'bosshealthbar', 'bosshealtbar', 'healthbar', 'summonstone', 'cave', '8k',
         'patrol', 'night', 'guard', 'leader', 'parent', 'guardparent', 'bonepile', 'dungeon', 'lore',
         'questentity', 'knowledgetrigger', 'showcase', 'spawnanim', 'sleep', 'start', 'add', 'spawntube',
         'spawncalcified', 'short', 'blind', 'stare', 'variation', 'variant', 'size', 'l', 's', 'm', 'xl',
         'mace', 'tame', 'tamed'}
# Words too generic to say two codes are the same creature.
GENERIC = {'enemy', 'animal', 'wildlife', 'wildbeast', 'fog', 'fogger', 'scavenger', 'skeleton', 'vukah', 'gen',
           'elite', 'hover'}
# The in-game word for each faction, as the client writes it (perks "Damage against Hollow",
# "Fell Curse", "Vukah and Wildlife", the Drak'Dal camps); checked against the En_Us table.
FACTION_WORDS = {'Fogger': 'Fell', 'Skeleton': 'Hollow', 'Scavenger': 'Scavenger', 'Vukah': 'Vukah',
                 'Drak': "Drak'Dal", 'WildBeast': 'Wildlife', 'Wildlife': 'Wildlife', 'FarmAnimal': 'Wildlife'}

# The listEntities filter (src/entity.cpp IsCatalogueEntity), for the audit only.
NOT_A_CREATURE = ['collider', 'colldier', 'weakspot', 'protection', 'attachment', '_weapon_', 'modular', 'trap',
                  'zone_', 'projectile', 'vfx', 'prop_', 'shroud_gem_forge', 'treasurecontainer', 'fishingfloat',
                  'home_base', 'placement', 'movingplatform', 'static_attack', 'utility', 'jumppad', 'test',
                  'debug', 'deprecated', 'depricated', 'donotuse', 'notfinished', 'disfunctional', '_wip']
NOT_A_CREATURE_PREFIX = ['base_', 'attack_', '1_player', 'player_', 'automatedplayer', 'fakeplayer_',
                         'companion_test', '_deprecated', 'z_']


def is_catalogue(code):
    l = code.lower()
    return not any(l.startswith(p) for p in NOT_A_CREATURE_PREFIX) and not any(s in l for s in NOT_A_CREATURE)


def c(s):
    return json.dumps(s, ensure_ascii=True)


def clean(name):
    """The client's markup (<npc>, <lorename>, ...) is styling, not text."""
    return ' '.join(re.sub(r'</?[A-Za-z]+>', '', name).split())


def tsv(path, cols):
    for line in open(path, encoding='utf-8'):
        f = line.rstrip('\n').split('\t')
        if len(f) >= cols and f[0]:
            yield f


def key(code, level):
    """The code without what does not make it another creature. Level 0 keeps tiers and numbers
    on their own (Fish_T1_common_02 is another fish than _01), 1 drops those numbers
    (Ranger_Armored_T3_02 is the T3_01 ranger), 2 also drops the tier. A number glued to a word is
    always a look (Melee07, Clothed05)."""
    out = []
    for tok in code.lower().split('_'):
        if not tok or tok in NOISE or re.fullmatch(r'v\d+', tok):
            continue
        if tok.isdigit():
            if level == 0:
                out.append(tok)
        elif re.fullmatch(r't\d', tok):
            if level < 2:
                out.append(tok)
        else:
            out.append(re.sub(r'\d+$', '', tok))
    return ' '.join(out)


def words(code):
    return {re.sub(r'\d+$', '', t) for t in code.lower().split('_')} - NOISE - GENERIC - {''}


def server_entities():
    src = open(os.path.join(SRC, 'gamedata.cpp')).read()
    rx = r'\{\{((?:0x[0-9a-f]{2},?){16})\},"([^"]*)","([^"]*)","([^"]*)","([^"]*)"\}'
    return [dict(guid=str(uuid.UUID(bytes=bytes(int(x, 16) for x in m.group(1).split(',')))), code=m.group(2),
                 faction=m.group(5)) for m in re.finditer(rx, src)]


def unique(names):
    return next(iter(names)) if len(names) == 1 else None


def main():
    args = sys.argv[1:]
    audit = None
    if '--audit' in args:
        i = args.index('--audit')
        audit = args[i + 1]
        del args[i:i + 2]
    exp = args[0]
    note = args[1] if len(args) > 1 else ''

    tmpl = {}  # every client template: guid -> dict
    for f in tsv(os.path.join(exp, 'templates-en.tsv'), 2):
        f += [''] * (7 - len(f))
        tmpl[f[0].lower()] = dict(code=f[1], name=clean(f[2]), model=f[4], arsenal=f[5], faction=f[6])
    spawns = {}
    for f in tsv(os.path.join(exp, 'logic-spawns-en.tsv'), 2):
        spawns.setdefault(f[0].lower(), set()).add(clean(f[1]))
    items = defaultdict(set)
    for f in tsv(os.path.join(exp, 'item-entities-en.tsv'), 3):
        if clean(f[2]):
            items[f[0].lower()].add(clean(f[2]))
    loca = {f[1] for f in tsv(os.path.join(exp, 'loca-en.tsv'), 2)}
    for word in FACTION_WORDS.values():
        if word not in loca:
            sys.exit('faction word %r is not in the En_Us table' % word)

    # Direct names, over every client template (a model or arsenal match may go through a template
    # the server table does not carry, such as a placement helper).
    named = {}
    for g, t in tmpl.items():
        if t['name']:
            named[g] = (t['name'], 'client')
        elif g in spawns and len(spawns[g]) == 1:
            named[g] = (next(iter(spawns[g])), 'logicSpawn')
        elif g in items and len(items[g]) == 1:
            named[g] = (next(iter(items[g])), 'item')

    ents = server_entities()
    missing = [e['code'] for e in ents if e['guid'] not in tmpl]
    if missing:
        print('warning: %d server templates are not in the client export, e.g. %s' % (len(missing), missing[:3]))
    for _ in range(4):
        by_model, by_ars = defaultdict(set), defaultdict(set)
        by_key = [defaultdict(set) for _ in range(3)]
        for g, (n, _src) in named.items():
            t = tmpl[g]
            if t['model']:
                by_model[t['model']].add(n)
            for level in range(3):
                by_key[level][(key(t['code'], level), t['faction'])].add(n)
            if t['arsenal']:
                by_ars[(t['arsenal'], t['faction'])].add((n, frozenset(words(t['code']))))
        new = {}
        for e in ents:
            g = e['guid']
            if g in named or g not in tmpl:
                continue
            t = tmpl[g]
            n = t['model'] and unique(by_model.get(t['model'], ()))
            if n:
                new[g] = (n, 'model')
                continue
            n = None
            for level in range(3):
                n = n or unique(by_key[level].get((key(t['code'], level), t['faction']), ()))
            if n:
                new[g] = (n, 'sibling')
                continue
            if t['arsenal']:
                mine = words(t['code'])
                cand = {n for n, w in by_ars.get((t['arsenal'], t['faction']), ()) if w & mine}
                if len(cand) == 1:
                    new[g] = (next(iter(cand)), 'arsenal')
        if not new:
            break
        named.update(new)

    rows = sorted(((e['guid'], named[e['guid']]) for e in ents if e['guid'] in named),
                  key=lambda kv: uuid.UUID(kv[0]).bytes)
    seen = set()
    rows = [r for r in rows if not (r[0] in seen or seen.add(r[0]))]
    o = ['// GENERATED by tools/gen_entity_labels.py - do not edit.', '#include "entity.h"', '',
         '// source: game client En_Us localisation' + (', ' + note if note else ''),
         'const EntityLabel kEntityLabels[] = {']
    for g, (name, src) in rows:
        o.append('  {{%s},%s,%s},' % (','.join('0x%02x' % b for b in uuid.UUID(g).bytes), c(name), c(src)))
    o += ['};', 'const size_t kEntityLabelCount = sizeof(kEntityLabels) / sizeof(kEntityLabels[0]);', '',
          '// The in-game word for a faction, for a template without a name of its own.',
          'const FactionLabel kFactionLabels[] = {']
    o += ['  {%s,%s},' % (c(f), c(w)) for f, w in sorted(FACTION_WORDS.items())]
    o += ['};', 'const size_t kFactionLabelCount = sizeof(kFactionLabels) / sizeof(kFactionLabels[0]);', '']
    open(OUT, 'w').write('\n'.join(o))

    by = defaultdict(int)
    for _, (_, src) in rows:
        by[src] += 1
    cat = [e for e in ents if is_catalogue(e['code'])]
    print('wrote %s: %d of %d templates named (%s)' % (OUT, len(rows), len(ents),
                                                        ', '.join('%s %d' % kv for kv in sorted(by.items()))))
    print('listEntities: %d of %d catalogue templates named, %d left out without an in-game name' %
          (sum(1 for e in cat if e['guid'] in named), len(cat), sum(1 for e in cat if e['guid'] not in named)))
    if audit:
        with open(audit, 'w') as f:
            f.write('code\ttemplateGuid\tname\tsource\tlistEntities\tfaction\n')
            for e in sorted(ents, key=lambda e: e['code'].lower()):
                n, src = named.get(e['guid'], ('', ''))
                if not n:
                    n, src = FACTION_WORDS.get(e['faction'], 'Unknown'), 'faction' if e['faction'] in FACTION_WORDS \
                        else 'unknown'
                listed = 'yes' if is_catalogue(e['code']) and e['guid'] in named else \
                    ('excluded: no in-game name' if is_catalogue(e['code']) else 'excluded: not a creature')
                f.write('\t'.join([e['code'], e['guid'], n, src, listed, e['faction']]) + '\n')


main()
