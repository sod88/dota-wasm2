#!/usr/bin/env python3
"""Generate cpp/src/data.cpp (the C++ engine's built-in content) from the Rust engine's content export.

    python3 tools/gen_cpp_builtin.py [path/to/libmoba_engine.so]  > cpp/src/data.cpp

The built-in heroes, items and rules are defined once, in src/data.rs. This script asks the Rust
engine for them (moba_content_export) and writes them out as C++ so both engines start from exactly
the same numbers. Numbers are copied as the exported decimal text, which reads back bit-identical.
The parity test (tools/parity.py) checks that both engines export byte-identical content.
"""
import json, sys, os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from enginelib import Engine  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
lib = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'target/release/libmoba_engine.so')
e = Engine(lib)
e.clear_mods()
text = e.export_content()
d = json.loads(text, parse_float=lambda s: ('f', s), parse_int=lambda s: ('i', s))


def num(v):
    kind, s = v
    return s


def flt(v):
    s = num(v)
    if '.' not in s and 'e' not in s.lower():
        s += '.0'
    return s + 'f'


def cstr(s):
    return json.dumps(s, ensure_ascii=False)


def levels(v):
    vals = [flt(x) for x in v] if isinstance(v, list) else [flt(v)] * 4
    return '{{' + ', '.join(vals) + '}}'


CAST = {'none': 'None', 'target': 'Target', 'line': 'Line', 'point': 'PointAoE', 'self_aoe': 'SelfAoE', 'self_buff': 'SelfBuff', 'blink': 'Blink'}
DTYPE = {'physical': 'Physical', 'magical': 'Magical', 'pure': 'Pure'}
FX = {'stun': 'FX_STUN', 'slow': 'FX_SLOW', 'root': 'FX_ROOT', 'haste': 'FX_HASTE', 'aspd': 'FX_ASPD'}
ACT = {'none': 'ACT_NONE', 'salve': 'ACT_SALVE', 'mana': 'ACT_MANA', 'blink': 'ACT_BLINK'}
PRIM = {'str': 0, 'agi': 1, 'int': 2}
ITEM_STATS = {'str': 'str_', 'agi': 'agi', 'int': 'int_', 'dmg': 'dmg', 'armor': 'armor', 'hp': 'hp', 'mana': 'mana',
              'aspd': 'aspd', 'ms': 'ms', 'hpreg': 'hpreg', 'mpreg': 'mpreg', 'cd': 'cd'}

out = []
w = out.append
w('// The built-in game content: heroes, abilities, items, creeps, neutrals, towers and rules.')
w('//')
w('// GENERATED from the Rust engine (src/data.rs) by tools/gen_cpp_builtin.py, so both engines start from')
w('// exactly the same numbers. Edit src/data.rs and regenerate, or change content with a mod instead.')
w('#include "content.hpp"')
w('')
w('namespace moba {')
w('')
w('Content make_builtin_content() {')
w('    Content c;')
w('')
w('    // ---- rules')
U32_RULES = {'wave_melee', 'wave_ranged', 'wave_siege', 'siege_every'}
for k, v in d['rules'].items():
    w(f'    c.rules.{k} = {num(v) + "u" if k in U32_RULES else flt(v)};')
w('')
w('    // ---- XP needed to reach each level (its length is the level cap)')
w('    c.xp_table = {' + ', '.join(flt(x) for x in d['xp_table']) + '};')
w('')
w('    // ---- items (index 0 is the empty slot)')
w('    c.items.push_back(ItemDef());')
item_ids = {}
for i, it in enumerate(d['items'], start=1):
    item_ids[it['name']] = i
    parts = [f'it.name = {cstr(it["name"])};', f'it.cost = {flt(it["cost"])};']
    for k, field in ITEM_STATS.items():
        if k in it:
            parts.append(f'it.{field} = {flt(it[k])};')
    if 'active' in it:
        parts.append(f'it.active = {ACT[it["active"]]};')
    if it.get('stack'):
        parts.append('it.stack = true;')
    if 'color' in it:
        parts.append(f'it.color = {cstr(it["color"])};')
    w('    { ItemDef it; ' + ' '.join(parts) + ' c.items.push_back(it); }')
w('')


def creep(var, cd, kind):
    w(f'    {var}.name = {cstr(cd["name"])};')
    w(f'    {var}.kind = Kind::{kind};')
    fields = ['hp', 'dmg_min', 'dmg_max', 'armor', 'range', 'bat', 'ms', 'proj_speed', 'radius', 'vision', 'gold', 'xp', 'bldg_mult']
    w('    ' + ' '.join(f'{var}.{k} = {flt(cd[k])};' for k in fields))


w('    // ---- lane creeps')
for slot, kind in [('melee', 'Melee'), ('ranged', 'Ranged'), ('siege', 'Siege')]:
    creep(f'c.{slot}', d['creeps'][slot], kind)
w('')
w('    // ---- jungle neutrals and camp compositions (by tier: small, medium, large)')
neutral_ids = {}
for i, n in enumerate(d['neutrals']):
    neutral_ids[n['name']] = i
    w('    {')
    w('    CreepDef n;')
    creep('n', n, 'Neutral')
    w('    c.neutrals.push_back(n);')
    w('    }')
w('    c.camp_comp = {' + ', '.join('{' + ', '.join(str(neutral_ids[x]) for x in tier) + '}' for tier in d['camps']) + '};')
w('')
w('    // ---- towers (tiers 1-3) and the Ancient')
for i, t in enumerate(d['towers']):
    w(f'    c.towers[{i}] = TowerDef{{{flt(t["hp"])}, {flt(t["dmg_min"])}, {flt(t["dmg_max"])}, {flt(t["armor"])}, {flt(t["range"])}, '
      f'{flt(t["bat"])}, {flt(t["gold"])}, {flt(t["lane_frac"])}}};')
a = d['ancient']
w(f'    c.ancient = AncientDef{{{flt(a["hp"])}, {flt(a["armor"])}, {flt(a["radius"])}, {flt(a["vision"])}}};')
w('')
w('    // ---- heroes and their four abilities each (index = hero * 4 + slot, slot 3 is the ultimate)')
for h in d['heroes']:
    w('    {')
    w('        HeroDef h;')
    w(f'        h.name = {cstr(h["name"])};')
    w(f'        h.title = {cstr(h["title"])};')
    w(f'        h.primary = {PRIM[h["primary"]]};')
    w('        ' + ' '.join(f'h.{f} = {flt(h[k])};' for k, f in [('str', 'str_'), ('agi', 'agi'), ('int', 'int_')]))
    w('        ' + ' '.join(f'h.{k} = {flt(h[k])};' for k in ['gain_str', 'gain_agi', 'gain_int']))
    w('        ' + ' '.join(f'h.{k} = {flt(h[k])};' for k in ['dmg_min', 'dmg_max', 'range', 'bat', 'ms', 'proj_speed', 'radius']))
    w('        h.build = {' + ', '.join(str(item_ids[x]) for x in h['build']) + '};  // ' + ', '.join(h['build']))
    w('        h.skill_order = {{' + ', '.join(num(x) for x in h['skill_order']) + '}};')
    if 'glyph' in h:
        w(f'        h.glyph = {cstr(h["glyph"])};')
    if 'color' in h:
        w(f'        h.color = {cstr(h["color"])};')
    w('        c.heroes.push_back(h);')
    w('    }')
    for ab in h['abilities']:
        w('    {')
        w('        AbilityDef a;')
        w(f'        a.name = {cstr(ab["name"])};')
        w(f'        a.desc = {cstr(ab["desc"])};')
        fx = ' | '.join(FX[x] for x in ab['fx']) or '0'
        w(f'        a.cast = Cast::{CAST[ab["cast"]]}; a.dtype = DmgType::{DTYPE[ab["dtype"]]}; a.fx = {fx};')
        w(f'        a.max_level = {num(ab["max_level"])}; a.req_level = {{{{' + ', '.join(num(x) for x in ab['req_level']) + '}};')
        for k in ['range', 'radius', 'mana', 'cd', 'dmg', 'dur', 'pct', 'pct2']:
            w(f'        a.{k} = {levels(ab[k])};')
        w(f'        a.speed = {flt(ab["speed"])}; a.delay = {flt(ab["delay"])}; a.aoe_on_land = {"true" if ab["aoe_on_land"] else "false"};')
        if 'icon' in ab:
            w(f'        a.icon = {cstr(ab["icon"])};')
        if 'color' in ab:
            w(f'        a.color = {cstr(ab["color"])};')
        w('        c.abilities.push_back(a);')
        w('    }')
w('    return c;')
w('}')
w('')
w('}  // namespace moba')
print('\n'.join(out))
