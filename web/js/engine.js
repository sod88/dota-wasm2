// Thin wrapper around the WebAssembly module. Two interchangeable engines implement it:
//   moba_engine.wasm      the Rust engine      (src/, built by ./build.sh)
//   moba_engine_cpp.wasm  the C++ engine       (cpp/, built by ./build-cpp.sh)
// Both export exactly the same C functions (ABI version 2), so this file does not care which one it got.
//
// The engine exposes flat f32 tables; the *layout* of every table is exported as JSON
// (`info.unit_fields`, ...), so columns are looked up by name here and nothing is hard-coded.

const ABI = 2;
export const ENGINES = {
  rust: { file: 'moba_engine.wasm', label: 'Rust', build: './build.sh' },
  cpp: { file: 'moba_engine_cpp.wasm', label: 'C++', build: './build-cpp.sh' },
};

const colMap = (names) => Object.fromEntries(names.map((n, i) => [n, i]));
const utf8 = new TextDecoder();

/**
 * Imports for a module that expects some (e.g. a C++ build made with Emscripten or wasi-sdk asks
 * for a few WASI system calls). The engines never do I/O, so these are inert stand-ins; text written
 * to stdout / stderr (an abort message, say) is forwarded to the console.
 */
function stubImports(module, getMemory) {
  const imports = {};
  for (const imp of WebAssembly.Module.imports(module)) {
    const ns = (imports[imp.module] ||= {});
    if (imp.kind === 'memory') { ns[imp.name] = new WebAssembly.Memory({ initial: 256, maximum: 32768 }); continue; }
    if (imp.kind !== 'function') throw new Error(`unsupported wasm import ${imp.module}.${imp.name} (${imp.kind})`);
    if (imp.name === 'proc_exit') ns[imp.name] = (code) => { throw new Error(`engine exited (code ${code})`); };
    else if (imp.name === 'fd_write') {
      ns[imp.name] = (fd, iovs, n, written) => {
        const mem = getMemory(), dv = new DataView(mem.buffer);
        let text = '', total = 0;
        for (let i = 0; i < n; i++) {
          const p = dv.getUint32(iovs + i * 8, true), l = dv.getUint32(iovs + i * 8 + 4, true);
          text += utf8.decode(new Uint8Array(mem.buffer, p, l));
          total += l;
        }
        dv.setUint32(written, total, true);
        if (text.trim()) console.warn(`[engine fd ${fd}] ${text.trimEnd()}`);
        return 0;
      };
    } else ns[imp.name] = () => 0;
  }
  return imports;
}

export class Engine {
  /** Fetch + instantiate one of the engines. Throws a readable error if the .wasm hasn't been built. */
  static async load(kind = 'rust') {
    const meta = ENGINES[kind];
    if (!meta) throw new Error(`Unknown engine "${kind}"`);
    let res;
    try {
      res = await fetch(meta.file, { cache: 'no-cache' });
    } catch (e) {
      throw new Error(`Could not fetch ${meta.file}: ${e.message}`);
    }
    if (!res.ok) throw new Error(`Could not load ${meta.file} (HTTP ${res.status}). Build it first with ${meta.build}`);
    const module = await WebAssembly.compile(await res.arrayBuffer());
    let exports = null;
    const instance = await WebAssembly.instantiate(module, stubImports(module, () => exports.memory));
    exports = instance.exports;
    if (typeof exports._initialize === 'function') exports._initialize(); // C++ static constructors (WASI reactor)
    return new Engine(exports, kind);
  }

  constructor(exports, kind = 'rust') {
    this.x = exports;
    this.kind = kind;
    this.label = ENGINES[kind]?.label ?? kind;
    const v = exports.moba_abi_version();
    if (v !== ABI) throw new Error(`The ${this.label} engine speaks ABI ${v}, this page expects ${ABI}. Rebuild it.`);
    this.info = null;
  }

  bytes(ptr, len) {
    return new Uint8Array(this.x.memory.buffer, ptr, len); // re-read buffer: memory.grow() detaches old views
  }

  /** Start or restart a match with the current mods. Re-reads the static info (heroes, items, layouts...). */
  start({ seed = (Math.random() * 2 ** 31) >>> 0, hero = 0, perTeam = 1, fog = true, bot = false } = {}) {
    this.x.moba_new(seed >>> 0, hero, perTeam, fog ? 1 : 0, bot ? 1 : 0);
    const info = JSON.parse(utf8.decode(this.bytes(this.x.moba_info_ptr(), this.x.moba_info_len())));
    info.U = colMap(info.unit_fields);
    info.H = colMap(info.hero_fields);
    info.P = colMap(info.proj_fields);
    info.Z = colMap(info.zone_fields);
    info.E = colMap(info.event_fields);
    info.G = colMap(info.global_fields);
    this.info = info;
    this.terrain = this.bytes(this.x.moba_terrain_ptr(), info.grid * info.grid).slice();
    return info;
  }

  // ---- mods ------------------------------------------------------------------------------------

  /**
   * Apply a mod (JSON text) on top of the content loaded so far. Takes effect at the next start().
   * Returns [] on success, otherwise the engine's error messages (and nothing changes).
   */
  loadMod(text) {
    const data = new TextEncoder().encode(text);
    const ptr = this.x.moba_alloc(data.length);
    if (!ptr) return ['mod file is too large'];
    this.bytes(ptr, data.length).set(data);
    if (this.x.moba_mod_load(data.length) === 0) return [];
    return utf8.decode(this.bytes(this.x.moba_mod_error_ptr(), this.x.moba_mod_error_len())).split('\n');
  }

  /** Back to the built-in game. */
  clearMods() { this.x.moba_mod_clear(); }

  modCount() { return this.x.moba_mod_count(); }

  /** The complete current content (built-in + mods) as a mod file — a starting point for new mods. */
  exportContent() {
    const n = this.x.moba_content_export();
    return utf8.decode(this.bytes(this.x.moba_content_ptr(), n));
  }

  // ---- simulation -------------------------------------------------------------------------------

  step(n) {
    this.x.moba_step(n);
  }

  /** Rebuild the tables in WASM memory and return a self-contained copy of them. */
  snapshot() {
    const x = this.x;
    x.moba_snapshot();
    const buf = x.memory.buffer;
    const { G, U, H } = this.info;
    const g = new Float32Array(buf, x.moba_global_ptr(), this.info.global_fields.length).slice();
    const tbl = (ptr, rows, fields) => (rows ? new Float32Array(buf, ptr, rows * fields.length).slice() : new Float32Array(0));
    const f = {
      g,
      time: g[G.time],
      tick: g[G.tick],
      winner: g[G.winner],
      player: g[G.player],
      playerTeam: g[G.player_team],
      nUnits: g[G.n_units],
      nHeroes: g[G.n_heroes],
      units: tbl(x.moba_units_ptr(), g[G.n_units], this.info.unit_fields),
      heroes: tbl(x.moba_heroes_ptr(), g[G.n_heroes], this.info.hero_fields),
      projs: tbl(x.moba_projs_ptr(), g[G.n_projs], this.info.proj_fields),
      zones: tbl(x.moba_zones_ptr(), g[G.n_zones], this.info.zone_fields),
      events: tbl(x.moba_events_ptr(), g[G.n_events], this.info.event_fields),
      index: new Map(),
      heroIndex: new Map(),
    };
    const us = this.info.unit_fields.length;
    for (let i = 0; i < f.nUnits; i++) f.index.set(f.units[i * us + U.id], i);
    const hs = this.info.hero_fields.length;
    for (let i = 0; i < f.nHeroes; i++) f.heroIndex.set(f.heroes[i * hs + H.unit], i);
    return f;
  }

  // ---- commands for the player's hero
  move(x, y) { this.x.moba_cmd_move(x, y); }
  attackMove(x, y) { this.x.moba_cmd_attack_move(x, y); }
  attack(id) { this.x.moba_cmd_attack(id); }
  stop() { this.x.moba_cmd_stop(); }
  hold() { this.x.moba_cmd_hold(); }
  /** slot 0..3 = ability, 10..15 = inventory item */
  cast(slot, target, x, y) { this.x.moba_cmd_cast(slot, target, x, y); }
  levelUp(slot) { this.x.moba_cmd_level_up(slot); }
  buy(item) { this.x.moba_cmd_buy(item); }
  sell(slot) { this.x.moba_cmd_sell(slot); }
  setBot(on) { this.x.moba_set_bot(on ? 1 : 0); }
  hash() { return ((BigInt(this.x.moba_hash_hi()) << 32n) | BigInt(this.x.moba_hash_lo())).toString(16).padStart(16, '0'); }
}
