"""ctypes wrapper around the engine's C ABI. Works with either implementation
(Rust: target/release/libmoba_engine.so, C++: cpp/build/libmoba_engine_cpp.so) because both export
exactly the same functions."""
import ctypes, json, struct

u32, f32, sz = ctypes.c_uint32, ctypes.c_float, ctypes.c_size_t
_SIG = {
    'moba_abi_version': ([], u32), 'moba_new': ([u32] * 5, None), 'moba_step': ([u32], None), 'moba_snapshot': ([], None),
    'moba_hash_lo': ([], u32), 'moba_hash_hi': ([], u32),
    'moba_cmd_move': ([f32, f32], None), 'moba_cmd_attack_move': ([f32, f32], None), 'moba_cmd_attack': ([u32], None),
    'moba_cmd_stop': ([], None), 'moba_cmd_hold': ([], None), 'moba_cmd_cast': ([u32, u32, f32, f32], None),
    'moba_cmd_level_up': ([u32], None), 'moba_cmd_buy': ([u32], None), 'moba_cmd_sell': ([u32], None), 'moba_set_bot': ([u32], None),
    'moba_alloc': ([sz], sz), 'moba_mod_load': ([sz], u32), 'moba_mod_error_len': ([], sz), 'moba_mod_clear': ([], None),
    'moba_mod_count': ([], u32), 'moba_content_export': ([], sz), 'moba_info_len': ([], sz),
}
_PTRS = ['units', 'heroes', 'projs', 'zones', 'events', 'global', 'info', 'terrain', 'mod_error', 'content']
TABLES = [('units', 'n_units', 'unit_fields'), ('heroes', 'n_heroes', 'hero_fields'), ('projs', 'n_projs', 'proj_fields'),
          ('zones', 'n_zones', 'zone_fields'), ('events', 'n_events', 'event_fields')]


class Engine:
    def __init__(self, path):
        self.lib = ctypes.CDLL(path)
        for n, (a, r) in _SIG.items():
            f = getattr(self.lib, n); f.argtypes = a; f.restype = r
        for n in _PTRS:
            f = getattr(self.lib, f'moba_{n}_ptr'); f.argtypes = []; f.restype = sz
        self.info = None

    # ---- mods
    def load_mod(self, data):
        """Returns the list of error messages (empty = loaded)."""
        if isinstance(data, str): data = data.encode()
        ptr = self.lib.moba_alloc(len(data))
        assert ptr, 'moba_alloc refused'
        ctypes.memmove(ptr, data, len(data))
        n = self.lib.moba_mod_load(len(data))
        if n == 0: return []
        return ctypes.string_at(self.lib.moba_mod_error_ptr(), self.lib.moba_mod_error_len()).decode().split('\n')

    def clear_mods(self): self.lib.moba_mod_clear()
    def mod_count(self): return self.lib.moba_mod_count()

    def export_content(self):
        n = self.lib.moba_content_export()
        return ctypes.string_at(self.lib.moba_content_ptr(), n).decode()

    # ---- match
    def new(self, seed=1, hero=0, per_team=1, fog=1, bot=1):
        self.lib.moba_new(seed, hero, per_team, fog, bot)
        self.info_bytes = ctypes.string_at(self.lib.moba_info_ptr(), self.lib.moba_info_len())
        self.info = json.loads(self.info_bytes)
        return self.info

    def step(self, n): self.lib.moba_step(n)

    def hash(self): return (self.lib.moba_hash_hi() << 32) | self.lib.moba_hash_lo()

    def snapshot(self):
        """Raw bytes of every table (what the browser reads), plus the global row decoded."""
        self.lib.moba_snapshot()
        info = self.info
        gf = info['global_fields']
        graw = ctypes.string_at(self.lib.moba_global_ptr(), 4 * len(gf))
        g = dict(zip(gf, struct.unpack(f'{len(gf)}f', graw)))
        out = {'global': graw}
        for name, cnt, fields in TABLES:
            nbytes = int(g[cnt]) * len(info[fields]) * 4
            out[name] = ctypes.string_at(getattr(self.lib, f'moba_{name}_ptr')(), nbytes) if nbytes else b''
        return g, out

    def table(self, raw, fields):
        k = len(fields)
        vals = struct.unpack(f'{len(raw)//4}f', raw)
        return [dict(zip(fields, vals[i:i + k])) for i in range(0, len(vals), k)]
