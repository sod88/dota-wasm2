#!/usr/bin/env python3
"""Rust vs C++ parity check: drives both engines through their C ABI with identical inputs and
fails at the first difference.

    python3 tools/parity.py            # quick run (a few minutes of game time per match)
    python3 tools/parity.py --full     # every config played to the end (or 45 minutes)

Checked, for many seeds / team sizes / heroes / fog settings / mods, with random player commands:
  * the state hash after every single tick,
  * every snapshot table (units, heroes, projectiles, zones, events, globals) byte for byte,
  * the static info JSON and the exported content, byte for byte,
  * the error messages for valid, invalid and randomly corrupted mod files.

Needs both native libraries:  cargo build --release   and   cmake --build cpp/build
"""
import json, os, random, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from enginelib import Engine  # noqa: E402

EXT = {'darwin': 'dylib', 'win32': 'dll'}.get(sys.platform, 'so')
RUST = os.path.join(ROOT, f'target/release/libmoba_engine.{EXT}')
CPP = os.path.join(ROOT, f'cpp/build/libmoba_engine_cpp.{EXT}')


class Mismatch(Exception):
    pass


def first_diff(a, b):
    n = min(len(a), len(b))
    for i in range(n):
        if a[i] != b[i]:
            return i
    return n


def same(what, a, b, ctx):
    if a != b:
        i = first_diff(a, b)
        sa, sb = a[max(0, i - 60):i + 60], b[max(0, i - 60):i + 60]
        raise Mismatch(f'{ctx}: {what} differs at byte {i} (lengths {len(a)} vs {len(b)})\n  rust: {sa!r}\n  c++ : {sb!r}')


def compare_snapshots(r, c, ctx):
    gr, tr = r.snapshot()
    gc, tc = c.snapshot()
    for name in tr:
        if tr[name] != tc.get(name):
            # decode the first differing row for a readable report
            fields = {'units': 'unit_fields', 'heroes': 'hero_fields', 'projs': 'proj_fields', 'zones': 'zone_fields',
                      'events': 'event_fields'}.get(name)
            detail = ''
            if fields:
                rows_r, rows_c = r.table(tr[name], r.info[fields]), c.table(tc[name], c.info[fields])
                for k, (x, y) in enumerate(zip(rows_r, rows_c)):
                    if x != y:
                        diff = {f: (x[f], y[f]) for f in x if x[f] != y[f]}
                        detail = f' row {k}: {diff}'
                        break
                else:
                    detail = f' rows {len(rows_r)} vs {len(rows_c)}'
            raise Mismatch(f'{ctx}: snapshot table "{name}" differs.{detail}')


def random_command(rng, info):
    """A random player command, as (function name, args); applied identically to both engines."""
    k = rng.random()
    x, y = rng.uniform(0, 2400), rng.uniform(0, 2400)
    if k < 0.35: return 'moba_cmd_move', (x, y)
    if k < 0.5: return 'moba_cmd_attack_move', (x, y)
    if k < 0.6: return 'moba_cmd_attack', (rng.randrange(1, 400),)
    if k < 0.72: return 'moba_cmd_cast', (rng.choice([0, 1, 2, 3, 10, 11, 12]), rng.randrange(0, 400), x, y)
    if k < 0.8: return 'moba_cmd_level_up', (rng.randrange(0, 4),)
    if k < 0.88: return 'moba_cmd_buy', (rng.randrange(0, len(info['items']) + 2),)
    if k < 0.92: return 'moba_cmd_sell', (rng.randrange(0, 7),)
    if k < 0.96: return 'moba_cmd_stop', ()
    if k < 0.98: return 'moba_cmd_hold', ()
    return 'moba_set_bot', (rng.randrange(0, 2),)


def play(r, c, cfg, mods, minutes, cmd_seed, snap_every=15):
    ctx = f'{cfg} mods={[m[0] for m in mods]}'
    for e in (r, c):
        e.clear_mods()
        for name, text in mods:
            errs = e.load_mod(text)
            assert not errs, f'{name}: {errs}'
    same('content export', r.export_content(), c.export_content(), ctx)
    ir, ic = r.new(**cfg), c.new(**cfg)
    same('info JSON', r.info_bytes, c.info_bytes, ctx)
    rng = random.Random(cmd_seed)
    ticks = int(minutes * 60 * 30)
    for t in range(ticks):
        if cmd_seed is not None and rng.random() < 0.08:
            fn, args = random_command(rng, ir)
            getattr(r.lib, fn)(*args)
            getattr(c.lib, fn)(*args)
        r.step(1)
        c.step(1)
        hr, hc = r.hash(), c.hash()
        if hr != hc:
            compare_snapshots(r, c, f'{ctx} tick {t + 1}')
            raise Mismatch(f'{ctx}: state hash differs at tick {t + 1}: {hr:016x} vs {hc:016x}')
        if t % snap_every == 0:
            compare_snapshots(r, c, f'{ctx} tick {t + 1}')
        if t % 900 == 0:
            g, _ = r.snapshot()
            c.snapshot()
            if g['winner'] >= 0:
                return t + 1, int(g['winner'])
    g, _ = r.snapshot()
    c.snapshot()
    return ticks, int(g['winner'])


def mod_error_parity(r, c):
    """Valid, invalid and fuzzed mods must give identical results (and messages) in both engines."""
    cases = json.load(open(os.path.join(ROOT, 'tests/mod_cases.json')))
    texts = []
    for case in cases:
        texts.append(case['mod_text'] if 'mod_text' in case else json.dumps(case['mod'], indent=2))
    for f in sorted(os.listdir(os.path.join(ROOT, 'web/mods'))):
        if f.endswith('.json') and f != 'index.json':
            texts.append(open(os.path.join(ROOT, 'web/mods', f), encoding='utf-8').read())
    texts.append(r.export_content())
    rng = random.Random(1234)
    base = list(texts)
    for _ in range(1500):  # random corruption: delete / insert / replace bytes and tokens
        t = bytearray(rng.choice(base).encode())
        for _ in range(rng.randint(1, 4)):
            op = rng.random()
            p = rng.randrange(0, max(1, len(t)))
            if op < 0.3 and t: del t[p:p + rng.randint(1, 3)]
            elif op < 0.6: t[p:p] = rng.choice([b'"', b',', b'}', b']', b'{', b'-1', b'1e9', b'null', b'"x"', b'\\u0000', b'\xff', b'0.0000001'])
            elif t: t[p] = rng.randrange(32, 127)
        texts.append(bytes(t))
    n_ok = 0
    for t in texts:
        data = t if isinstance(t, bytes) else t.encode()
        r.clear_mods(); c.clear_mods()
        er, ec = r.load_mod(data), c.load_mod(data)
        if er != ec:
            raise Mismatch(f'mod errors differ for input {data[:200]!r}\n  rust: {er}\n  c++ : {ec}')
        if not er:
            n_ok += 1
            same('export after mod', r.export_content(), c.export_content(), f'mod {data[:60]!r}')
    r.clear_mods(); c.clear_mods()
    return len(texts), n_ok


def main():
    full = '--full' in sys.argv
    r, c = Engine(RUST), Engine(CPP)
    assert r.lib.moba_abi_version() == c.lib.moba_abi_version() == 2
    t0 = time.time()
    n, ok = mod_error_parity(r, c)
    print(f'mod files: {n} inputs ({ok} valid) give identical results and messages  [{time.time() - t0:.1f}s]')

    mods_dir = os.path.join(ROOT, 'web/mods')
    m = {f[:-5]: (f[:-5], open(os.path.join(mods_dir, f), encoding='utf-8').read()) for f in os.listdir(mods_dir) if f.endswith('.json') and f != 'index.json'}
    plans = [
        (dict(seed=3, hero=0, per_team=1, fog=1, bot=1), [], None),
        (dict(seed=5, hero=1, per_team=2, fog=1, bot=1), [], None),
        (dict(seed=7, hero=2, per_team=3, fog=0, bot=1), [], None),
        (dict(seed=11, hero=0, per_team=5, fog=1, bot=1), [], None),
        (dict(seed=21, hero=0, per_team=1, fog=1, bot=0), [], 99),            # a "human" issuing random commands
        (dict(seed=22, hero=2, per_team=2, fog=1, bot=0), [], 7),
        (dict(seed=9, hero=3, per_team=2, fog=1, bot=1), [m['nightblade']], None),
        (dict(seed=4, hero=3, per_team=3, fog=1, bot=0), [m['nightblade'], m['gold-rush'], m['wild-jungle']], 5),
        (dict(seed=8, hero=1, per_team=2, fog=1, bot=1), [m['gold-rush']], None),
        (dict(seed=13, hero=0, per_team=3, fog=1, bot=1), [m['wild-jungle']], None),
    ]
    minutes = 45 if full else 6
    total_ticks = 0
    for cfg, mods, cmd_seed in plans:
        t1 = time.time()
        ticks, winner = play(r, c, cfg, mods, minutes, cmd_seed)
        total_ticks += ticks
        res = {-1: 'no winner yet', 0: 'Radiant won', 1: 'Dire won'}[winner]
        print(f'identical for {ticks:6d} ticks ({ticks / 1800:5.1f} min): {cfg["per_team"]}v{cfg["per_team"]} seed {cfg["seed"]:<3} '
              f'hero {cfg["hero"]} fog {cfg["fog"]} {"random cmds " if cmd_seed is not None else "bots only   "}'
              f'mods={[x[0] for x in mods]}  {res}  [{time.time() - t1:.1f}s]')
    print(f'\nPARITY OK: {total_ticks} ticks compared tick-by-tick in {time.time() - t0:.0f}s')


if __name__ == '__main__':
    try:
        main()
    except Mismatch as e:
        print('\nPARITY FAILURE\n' + str(e))
        sys.exit(1)
