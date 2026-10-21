// Thin wrapper around the WebAssembly module. Two interchangeable engines implement it:
//   moba_engine.wasm      the Rust engine      (src/, built by ./build.sh)
//   moba_engine_cpp.wasm  the C++ engine       (cpp/, built by ./build-cpp.sh)
// Both export exactly the same C functions (ABI version 2), so this file does not care which one it got.
//
// The engine exposes flat f32 tables; the *layout* of every table is exported as JSON
// (`info.unit_fields`, ...), so columns are looked up by name here and nothing is hard-coded.
// The column names are also listed below as types, so a misspelt column is a compile error; start()
// checks that the engine really has every one of them.
//
// This file is TypeScript: ./build-web.sh compiles it to web/js/engine.js (./serve.sh does that for you).

const ABI = 2;

export type EngineKind = 'rust' | 'cpp';
export const ENGINES: Record<EngineKind, { file: string; label: string; build: string }> = {
  rust: { file: 'moba_engine.wasm', label: 'Rust', build: './build.sh' },
  cpp: { file: 'moba_engine_cpp.wasm', label: 'C++', build: './build-cpp.sh' },
};

// ---- the C interface (src/ffi.rs, cpp/src/ffi.cpp) ------------------------------------------------

type Ptr = number; // byte offset into the module's memory

export interface Exports {
  memory: WebAssembly.Memory;
  _initialize?: () => void; // C++ static constructors (WASI reactor)
  moba_abi_version(): number;
  moba_new(seed: number, hero: number, perTeam: number, fog: number, bot: number): void;
  moba_alloc(len: number): Ptr;
  moba_mod_load(len: number): number;
  moba_mod_error_ptr(): Ptr;
  moba_mod_error_len(): number;
  moba_mod_clear(): void;
  moba_mod_count(): number;
  moba_content_export(): number;
  moba_content_ptr(): Ptr;
  moba_step(n: number): void;
  moba_snapshot(): void;
  moba_units_ptr(): Ptr;
  moba_heroes_ptr(): Ptr;
  moba_projs_ptr(): Ptr;
  moba_zones_ptr(): Ptr;
  moba_events_ptr(): Ptr;
  moba_global_ptr(): Ptr;
  moba_info_ptr(): Ptr;
  moba_info_len(): number;
  moba_terrain_ptr(): Ptr;
  moba_hash_lo(): number;
  moba_hash_hi(): number;
  moba_cmd_move(x: number, y: number): void;
  moba_cmd_attack_move(x: number, y: number): void;
  moba_cmd_attack(target: number): void;
  moba_cmd_stop(): void;
  moba_cmd_hold(): void;
  moba_cmd_cast(slot: number, target: number, x: number, y: number): void;
  moba_cmd_level_up(slot: number): void;
  moba_cmd_buy(item: number): void;
  moba_cmd_sell(slot: number): void;
  moba_set_bot(on: number): void;
}

// ---- table layouts (the *_FIELDS lists in src/ffi.rs) ------------------------------------------

const UNIT_FIELDS = [
  'id', 'kind', 'def', 'team', 'alive', 'x', 'y', 'facing', 'radius', 'hp', 'max_hp', 'mana', 'max_mana', 'order',
  'target', 'visible', 'swing_t', 'swing_len', 'status', 'level', 'hero', 'range', 'vision', 'armor', 'speed', 'dmg',
] as const;
const HERO_FIELDS = [
  'unit', 'def', 'team', 'level', 'xp', 'xp_prev', 'xp_next', 'skill_pts', 'gold', 'networth', 'kills', 'deaths',
  'assists', 'last_hits', 'denies', 'respawn', 'bot', 'alive',
  'ab0', 'ab1', 'ab2', 'ab3', 'cd0', 'cd1', 'cd2', 'cd3',
  'it0', 'it1', 'it2', 'it3', 'it4', 'it5', 'ch0', 'ch1', 'ch2', 'ch3', 'ch4', 'ch5',
  'icd0', 'icd1', 'icd2', 'icd3', 'icd4', 'icd5',
  'str', 'agi', 'int', 'dmg_min', 'dmg_max', 'armor', 'ms', 'atk_time', 'hp_regen', 'mana_regen',
] as const;
const PROJ_FIELDS = ['id', 'x', 'y', 'dx', 'dy', 'fx', 'team', 'radius', 'ability', 'homing', 'speed'] as const;
const ZONE_FIELDS = ['id', 'x', 'y', 'radius', 't_left', 't_total', 'team', 'ability', 'fx'] as const;
const EVENT_FIELDS = ['type', 'a', 'b', 'x', 'y', 'v', 'w', 't', 'visible'] as const;
const GLOBAL_FIELDS = [
  'time', 'tick', 'wave_no', 'next_wave', 'winner', 'player', 'player_team', 'fog',
  'n_units', 'n_heroes', 'n_projs', 'n_zones', 'n_events', 'near_shop', 'player_hero',
] as const;

export type UnitField = (typeof UNIT_FIELDS)[number];
export type HeroField = (typeof HERO_FIELDS)[number];
export type ProjField = (typeof PROJ_FIELDS)[number];
export type ZoneField = (typeof ZONE_FIELDS)[number];
export type EventField = (typeof EVENT_FIELDS)[number];
export type GlobalField = (typeof GLOBAL_FIELDS)[number];

/** Column name -> index within a row. */
export type Columns<F extends string> = Record<F, number>;

// ---- static info (build_info() in src/ffi.rs) -------------------------------------------------

type Codes<K extends string> = Record<K, number>;

export interface HeroDef {
  name: string; title: string; primary: number; str: number; agi: number; int: number;
  gain_str: number; gain_agi: number; gain_int: number; dmg_min: number; dmg_max: number;
  range: number; bat: number; ms: number; proj_speed: number; radius: number; glyph: string; color: string;
}

/** Per-level values are arrays indexed by ability level - 1. */
export interface AbilityDef {
  name: string; desc: string; cast: number; dtype: number; fx: number; max_level: number;
  speed: number; delay: number; aoe_on_land: number; icon: string; color: string;
  req_level: number[]; range: number[]; radius: number[]; mana: number[]; cd: number[];
  dmg: number[]; dur: number[]; pct: number[]; pct2: number[];
}

export interface ItemDef {
  name: string; cost: number; str: number; agi: number; int: number; dmg: number; armor: number;
  hp: number; mana: number; aspd: number; ms: number; hpreg: number; mpreg: number;
  active: number; cd: number; stack: number; color: string;
}

export interface ModInfo { name: string; version: string; author: string; description: string }

type Point = [number, number];

export interface Info {
  abi: number; tick_hz: number; world: number; cell: number; grid: number;
  shop_radius: number; max_level: number; inv_slots: number; wave_interval: number;
  xp_radius: number; start_gold: number; blink_dagger_range: number;
  xp_table: number[];
  mods: ModInfo[];
  unit_fields: string[]; hero_fields: string[]; proj_fields: string[];
  zone_fields: string[]; event_fields: string[]; global_fields: string[];
  kind: Codes<'hero' | 'melee' | 'ranged' | 'siege' | 'neutral' | 'tower' | 'ancient'>;
  cast: Codes<'none' | 'target' | 'line' | 'point' | 'self_aoe' | 'self_buff' | 'blink'>;
  status: Codes<'stun' | 'root' | 'slow' | 'haste' | 'aspd' | 'salve' | 'mana'>;
  fx: Codes<'stun' | 'slow' | 'root' | 'haste' | 'aspd'>;
  act: Codes<'none' | 'salve' | 'mana' | 'blink'>;
  ev: Codes<'damage' | 'kill' | 'heal' | 'level' | 'building' | 'cast' | 'gold' | 'gameover' | 'wave' | 'respawn'
    | 'attack' | 'item' | 'learn' | 'blink' | 'assist'>;
  kind_names: string[];
  neutral_names: string[];
  bases: Point[];
  lanes: Point[][];
  camps: { x: number; y: number; tier: number }[];
  heroes: HeroDef[];
  abilities: AbilityDef[]; // index = hero * 4 + slot
  items: ItemDef[]; // index 0 = empty slot
}

/** The info JSON plus column lookups, added by start(). */
export interface GameInfo extends Info {
  U: Columns<UnitField>;
  H: Columns<HeroField>;
  P: Columns<ProjField>;
  Z: Columns<ZoneField>;
  E: Columns<EventField>;
  G: Columns<GlobalField>;
}

/** One snapshot of the match: copies of the engine's tables, safe to keep across steps. */
export interface Frame {
  g: Float32Array;
  time: number;
  tick: number;
  winner: number;
  player: number;
  playerTeam: number;
  nUnits: number;
  nHeroes: number;
  units: Float32Array; // nUnits rows of info.unit_fields.length
  heroes: Float32Array;
  projs: Float32Array;
  zones: Float32Array;
  events: Float32Array;
  /** unit id -> row in `units` */
  index: Map<number, number>;
  /** unit id of a hero -> row in `heroes` */
  heroIndex: Map<number, number>;
}

export interface StartOptions { seed?: number; hero?: number; perTeam?: number; fog?: boolean; bot?: boolean }

/** Column lookup for one table; throws when the engine lacks a column this page relies on. */
function colMap<F extends string>(table: string, names: string[], expected: readonly F[]): Columns<F> {
  const map = Object.fromEntries(names.map((n, i) => [n, i])) as Columns<F>;
  const missing = expected.filter((n) => !(n in map));
  if (missing.length) throw new Error(`The engine's ${table} table has no column ${missing.join(', ')}. Rebuild it.`);
  return map;
}

const utf8 = new TextDecoder();

type ImportFn = (...args: number[]) => number | void;

/**
 * Imports for a module that expects some (e.g. a C++ build made with Emscripten or wasi-sdk asks
 * for a few WASI system calls). The engines never do I/O, so these are inert stand-ins; text written
 * to stdout / stderr (an abort message, say) is forwarded to the console.
 */
function stubImports(module: WebAssembly.Module, getMemory: () => WebAssembly.Memory): WebAssembly.Imports {
  const imports: Record<string, Record<string, ImportFn | WebAssembly.Memory>> = {};
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
  readonly x: Exports;
  readonly kind: EngineKind;
  readonly label: string;
  /** Set by start(); null until the first match. */
  info: GameInfo | null = null;
  terrain: Uint8Array = new Uint8Array(0);

  /** Fetch + instantiate one of the engines. Throws a readable error if the .wasm hasn't been built. */
  static async load(kind: EngineKind = 'rust'): Promise<Engine> {
    const meta = ENGINES[kind];
    if (!meta) throw new Error(`Unknown engine "${kind}"`);
    let res;
    try {
      res = await fetch(meta.file, { cache: 'no-cache' });
    } catch (e) {
      throw new Error(`Could not fetch ${meta.file}: ${(e as Error).message}`);
    }
    if (!res.ok) throw new Error(`Could not load ${meta.file} (HTTP ${res.status}). Build it first with ${meta.build}`);
    const module = await WebAssembly.compile(await res.arrayBuffer());
    let exports: Exports | null = null;
    const instance = await WebAssembly.instantiate(module, stubImports(module, () => exports!.memory));
    exports = instance.exports as unknown as Exports;
    if (typeof exports._initialize === 'function') exports._initialize();
    return new Engine(exports, kind);
  }

  constructor(exports: Exports, kind: EngineKind = 'rust') {
    this.x = exports;
    this.kind = kind;
    this.label = ENGINES[kind]?.label ?? kind;
    const v = exports.moba_abi_version();
    if (v !== ABI) throw new Error(`The ${this.label} engine speaks ABI ${v}, this page expects ${ABI}. Rebuild it.`);
  }

  bytes(ptr: Ptr, len: number): Uint8Array {
    return new Uint8Array(this.x.memory.buffer, ptr, len); // re-read buffer: memory.grow() detaches old views
  }

  /** Start or restart a match with the current mods. Re-reads the static info (heroes, items, layouts...). */
  start({ seed = (Math.random() * 2 ** 31) >>> 0, hero = 0, perTeam = 1, fog = true, bot = false }: StartOptions = {}): GameInfo {
    this.x.moba_new(seed >>> 0, hero, perTeam, fog ? 1 : 0, bot ? 1 : 0);
    const raw = JSON.parse(utf8.decode(this.bytes(this.x.moba_info_ptr(), this.x.moba_info_len()))) as Info;
    const info: GameInfo = {
      ...raw,
      U: colMap('unit', raw.unit_fields, UNIT_FIELDS),
      H: colMap('hero', raw.hero_fields, HERO_FIELDS),
      P: colMap('projectile', raw.proj_fields, PROJ_FIELDS),
      Z: colMap('zone', raw.zone_fields, ZONE_FIELDS),
      E: colMap('event', raw.event_fields, EVENT_FIELDS),
      G: colMap('global', raw.global_fields, GLOBAL_FIELDS),
    };
    this.info = info;
    this.terrain = this.bytes(this.x.moba_terrain_ptr(), info.grid * info.grid).slice();
    return info;
  }

  // ---- mods ------------------------------------------------------------------------------------

  /**
   * Apply a mod (JSON text) on top of the content loaded so far. Takes effect at the next start().
   * Returns [] on success, otherwise the engine's error messages (and nothing changes).
   */
  loadMod(text: string): string[] {
    const data = new TextEncoder().encode(text);
    const ptr = this.x.moba_alloc(data.length);
    if (!ptr) return ['mod file is too large'];
    this.bytes(ptr, data.length).set(data);
    if (this.x.moba_mod_load(data.length) === 0) return [];
    return utf8.decode(this.bytes(this.x.moba_mod_error_ptr(), this.x.moba_mod_error_len())).split('\n');
  }

  /** Back to the built-in game. */
  clearMods(): void { this.x.moba_mod_clear(); }

  modCount(): number { return this.x.moba_mod_count(); }

  /** The complete current content (built-in + mods) as a mod file — a starting point for new mods. */
  exportContent(): string {
    const n = this.x.moba_content_export();
    return utf8.decode(this.bytes(this.x.moba_content_ptr(), n));
  }

  // ---- simulation -------------------------------------------------------------------------------

  step(n: number): void {
    this.x.moba_step(n);
  }

  /** Rebuild the tables in WASM memory and return a self-contained copy of them. */
  snapshot(): Frame {
    const x = this.x, info = this.info;
    if (!info) throw new Error('snapshot() before start()');
    x.moba_snapshot();
    const buf = x.memory.buffer;
    const { G, U, H } = info;
    const g = new Float32Array(buf, x.moba_global_ptr(), info.global_fields.length).slice();
    const tbl = (ptr: Ptr, rows: number, fields: string[]) =>
      (rows ? new Float32Array(buf, ptr, rows * fields.length).slice() : new Float32Array(0));
    const f: Frame = {
      g,
      time: g[G.time],
      tick: g[G.tick],
      winner: g[G.winner],
      player: g[G.player],
      playerTeam: g[G.player_team],
      nUnits: g[G.n_units],
      nHeroes: g[G.n_heroes],
      units: tbl(x.moba_units_ptr(), g[G.n_units], info.unit_fields),
      heroes: tbl(x.moba_heroes_ptr(), g[G.n_heroes], info.hero_fields),
      projs: tbl(x.moba_projs_ptr(), g[G.n_projs], info.proj_fields),
      zones: tbl(x.moba_zones_ptr(), g[G.n_zones], info.zone_fields),
      events: tbl(x.moba_events_ptr(), g[G.n_events], info.event_fields),
      index: new Map(),
      heroIndex: new Map(),
    };
    const us = info.unit_fields.length;
    for (let i = 0; i < f.nUnits; i++) f.index.set(f.units[i * us + U.id], i);
    const hs = info.hero_fields.length;
    for (let i = 0; i < f.nHeroes; i++) f.heroIndex.set(f.heroes[i * hs + H.unit], i);
    return f;
  }

  // ---- commands for the player's hero
  move(x: number, y: number): void { this.x.moba_cmd_move(x, y); }
  attackMove(x: number, y: number): void { this.x.moba_cmd_attack_move(x, y); }
  attack(id: number): void { this.x.moba_cmd_attack(id); }
  stop(): void { this.x.moba_cmd_stop(); }
  hold(): void { this.x.moba_cmd_hold(); }
  /** slot 0..3 = ability, 10..15 = inventory item */
  cast(slot: number, target: number, x: number, y: number): void { this.x.moba_cmd_cast(slot, target, x, y); }
  levelUp(slot: number): void { this.x.moba_cmd_level_up(slot); }
  buy(item: number): void { this.x.moba_cmd_buy(item); }
  sell(slot: number): void { this.x.moba_cmd_sell(slot); }
  setBot(on: boolean): void { this.x.moba_set_bot(on ? 1 : 0); }
  /** The engine's 64-bit state hash as 16 hex digits. wasm hands u32 results to JS as signed i32, hence >>> 0. */
  hash(): string {
    const hi = BigInt(this.x.moba_hash_hi() >>> 0), lo = BigInt(this.x.moba_hash_lo() >>> 0);
    return ((hi << 32n) | lo).toString(16).padStart(16, '0');
  }
}
