//! C-ABI bridge between the simulation and JavaScript (no `wasm-bindgen` needed).
//!
//! JavaScript never sees Rust structs. It drives the engine through a few `extern "C"`
//! functions that only take numbers, and reads results as flat `f32` tables straight out of
//! WebAssembly memory:
//!
//! ```text
//!   moba_new(seed, hero, per_team, fog, bot)    start / restart a match
//!   moba_step(n)                                advance n fixed ticks (30 Hz)
//!   moba_cmd_*(...)                             queue commands for the player's hero
//!   moba_snapshot()                             rebuild the tables below (also drains events)
//!   moba_units_ptr() / heroes / projs / zones / events / global   -> pointer to f32 rows
//!   moba_info_ptr() / moba_info_len()           static JSON: layouts, heroes, abilities, items, map
//!   moba_terrain_ptr()                          GRID*GRID bytes of terrain codes
//!
//!   moba_alloc(len) -> ptr                      staging buffer for a mod file (JS writes the bytes)
//!   moba_mod_load(len) -> errors                parse + apply the staged mod; 0 = ok (stackable)
//!   moba_mod_error_ptr() / moba_mod_error_len() newline-separated error messages of the last load
//!   moba_mod_clear()                            back to the built-in content
//!   moba_mod_count()                            number of mods currently applied
//!   moba_content_export() -> len, moba_content_ptr()   the active content as a mod file
//! ```
//!
//! Mods only take effect at the next `moba_new`.
//!
//! Row layouts are defined once, by the `*_FIELDS` name lists below, and are exported in the
//! info JSON — so JS looks columns up by name and never hard-codes an index.
//!
//! Fog of war is enforced here: enemy units, projectiles, zones and events that the player's
//! team cannot currently see are simply not written into the tables.

use crate::content::{self, Content};
use crate::data::*;
use crate::game::*;
use crate::map::*;
use crate::math::Vec2;
use crate::unit::Cmd;
use std::fmt::Write as _;
use std::sync::Mutex;

pub const ABI_VERSION: u32 = 2;

/// One row per visible unit.
pub const UNIT_FIELDS: &[&str] = &[
    "id", "kind", "def", "team", "alive", "x", "y", "facing", "radius", "hp", "max_hp", "mana", "max_mana", "order", "target",
    "visible", "swing_t", "swing_len", "status", "level", "hero", "range", "vision", "armor", "speed", "dmg",
];
/// One row per hero (both teams) — scoreboard and the player's HUD.
pub const HERO_FIELDS: &[&str] = &[
    "unit", "def", "team", "level", "xp", "xp_prev", "xp_next", "skill_pts", "gold", "networth", "kills", "deaths", "assists",
    "last_hits", "denies", "respawn", "bot", "alive", "ab0", "ab1", "ab2", "ab3", "cd0", "cd1", "cd2", "cd3", "it0", "it1", "it2",
    "it3", "it4", "it5", "ch0", "ch1", "ch2", "ch3", "ch4", "ch5", "icd0", "icd1", "icd2", "icd3", "icd4", "icd5", "str", "agi",
    "int", "dmg_min", "dmg_max", "armor", "ms", "atk_time", "hp_regen", "mana_regen",
];
pub const PROJ_FIELDS: &[&str] = &["id", "x", "y", "dx", "dy", "fx", "team", "radius", "ability", "homing", "speed"];
pub const ZONE_FIELDS: &[&str] = &["id", "x", "y", "radius", "t_left", "t_total", "team", "ability", "fx"];
/// The 8 raw engine event slots plus a `visible` flag computed from the player's vision.
pub const EVENT_FIELDS: &[&str] = &["type", "a", "b", "x", "y", "v", "w", "t", "visible"];
pub const GLOBAL_FIELDS: &[&str] = &[
    "time", "tick", "wave_no", "next_wave", "winner", "player", "player_team", "fog", "n_units", "n_heroes", "n_projs", "n_zones",
    "n_events", "near_shop", "player_hero",
];

/// Bits of the unit `status` column.
pub const ST_STUN: u32 = 1;
pub const ST_ROOT: u32 = 2;
pub const ST_SLOW: u32 = 4;
pub const ST_HASTE: u32 = 8;
pub const ST_ASPD: u32 = 16;
pub const ST_SALVE: u32 = 32;
pub const ST_MANA: u32 = 64;

/// Keep at most this many undelivered events if JS stops taking snapshots (background tab).
const MAX_EVENTS: usize = 4096;

struct Bridge {
    game: Game,
    info: Vec<u8>,
    units: Vec<f32>,
    heroes: Vec<f32>,
    projs: Vec<f32>,
    zones: Vec<f32>,
    events: Vec<f32>,
    global: Vec<f32>,
}

static BRIDGE: Mutex<Option<Bridge>> = Mutex::new(None);

fn with<R>(f: impl FnOnce(&mut Bridge) -> R) -> Option<R> {
    let mut guard = BRIDGE.lock().unwrap_or_else(|e| e.into_inner());
    guard.as_mut().map(f)
}

#[inline]
fn b2f(b: bool) -> f32 {
    if b {
        1.0
    } else {
        0.0
    }
}

/// Can the player's team currently see world point `p`?
fn seen(g: &Game, p: Vec2) -> bool {
    !g.fog || g.units.iter().any(|u| u.alive && u.team == g.player_team && u.pos.dist_sq(p) <= u.vision * u.vision)
}

fn is_positional(ev_type: f32) -> bool {
    [EV_DAMAGE, EV_KILL, EV_CAST, EV_ATTACK, EV_BUILDING, EV_GOLD, EV_LEVEL, EV_RESPAWN, EV_ASSIST, EV_BLINK].contains(&ev_type)
}

impl Bridge {
    fn snapshot(&mut self) {
        let Bridge { game: g, units, heroes, projs, zones, events, global, .. } = self;
        // Unit indices shift when dead units are swept, so vision must be rebuilt for *now*.
        g.compute_vision();
        let pt = g.player_team;

        // ---- units (enemies in the fog are omitted; buildings are always listed, flagged hidden)
        units.clear();
        for ui in 0..g.units.len() {
            let u = &g.units[ui];
            if !u.alive {
                continue;
            }
            let visible = u.team == pt || g.visible_to(pt, ui);
            if !visible && !u.is_building() {
                continue;
            }
            let (level, hero) = if u.hero >= 0 { (g.heroes[u.hero as usize].level as f32, u.hero as f32) } else { (0.0, -1.0) };
            let mut status = 0u32;
            for (on, bit) in [
                (u.stun_t > 0.0, ST_STUN),
                (u.root_t > 0.0, ST_ROOT),
                (u.slow_t > 0.0, ST_SLOW),
                (u.haste_t > 0.0, ST_HASTE),
                (u.aspd_t > 0.0, ST_ASPD),
                (u.salve_t > 0.0, ST_SALVE),
                (u.mana_pot_t > 0.0, ST_MANA),
            ] {
                if on {
                    status |= bit;
                }
            }
            units.extend_from_slice(&[
                u.id as f32,
                u.kind as u8 as f32,
                u.def as f32,
                u.team as f32,
                1.0,
                u.pos.x,
                u.pos.y,
                u.facing,
                u.radius,
                u.hp,
                u.max_hp,
                u.mana,
                u.max_mana,
                u.order.code(),
                u.target as f32,
                b2f(visible),
                u.swing_t,
                u.swing_len,
                status as f32,
                level,
                hero,
                u.range,
                u.vision,
                u.armor,
                u.speed(),
                u.avg_damage(),
            ]);
        }

        // ---- heroes
        heroes.clear();
        for h in &g.heroes {
            let ui = match g.idx(h.unit) {
                Some(i) => i,
                None => continue,
            };
            let u = &g.units[ui];
            let lvl = h.level as usize;
            let max_level = g.content.xp_table.len();
            let xp_prev = g.content.xp_table[(lvl - 1).min(max_level - 1)];
            let xp_next = g.content.xp_table[lvl.min(max_level - 1)];
            heroes.extend_from_slice(&[
                h.unit as f32,
                h.def as f32,
                h.team as f32,
                h.level as f32,
                h.xp,
                xp_prev,
                xp_next,
                h.skill_pts as f32,
                h.gold,
                h.networth,
                h.kills as f32,
                h.deaths as f32,
                h.assists as f32,
                h.last_hits as f32,
                h.denies as f32,
                if u.alive { 0.0 } else { h.respawn_t.max(0.0) },
                b2f(h.bot),
                b2f(u.alive),
            ]);
            heroes.extend(h.ab_level.iter().map(|&v| v as f32));
            heroes.extend(h.ab_cd.iter().map(|&v| v.max(0.0)));
            heroes.extend(h.items.iter().map(|s| s.id as f32));
            heroes.extend(h.items.iter().map(|s| s.charges as f32));
            heroes.extend(h.items.iter().map(|s| s.cd.max(0.0)));
            let b = h.item_bonus(g.content);
            heroes.extend_from_slice(&[
                h.base_str + b.str_,
                h.base_agi + b.agi,
                h.base_int + b.int,
                u.dmg_min,
                u.dmg_max,
                u.armor,
                u.speed(),
                u.attack_time(),
                u.hp_regen,
                u.mana_regen,
            ]);
        }

        // ---- projectiles & zones
        projs.clear();
        for p in &g.projs {
            if !seen(g, p.pos) {
                continue;
            }
            projs.extend_from_slice(&[p.id as f32, p.pos.x, p.pos.y, p.dir.x, p.dir.y, p.fx as f32, p.team as f32, p.radius, p.ability as f32, b2f(p.homing != 0), p.speed]);
        }
        zones.clear();
        for z in &g.zones {
            if z.team != pt && !seen(g, z.pos) {
                continue;
            }
            zones.extend_from_slice(&[z.id as f32, z.pos.x, z.pos.y, z.radius, z.t_left, z.t_total, z.team as f32, z.ability as f32, z.fx as f32]);
        }

        // ---- events (drained)
        events.clear();
        let mut evs = std::mem::take(&mut g.events);
        if evs.len() > MAX_EVENTS {
            evs.drain(..evs.len() - MAX_EVENTS);
        }
        for e in &evs {
            let vis = !is_positional(e[0]) || seen(g, Vec2::new(e[3], e[4]));
            events.extend_from_slice(e);
            events.push(b2f(vis));
        }

        // ---- global
        let player_hero = g.player_idx().map(|i| g.units[i].hero).unwrap_or(-1);
        let near_shop = g.player_idx().map(|i| g.near_shop(i)).unwrap_or(false);
        global.clear();
        global.extend_from_slice(&[
            g.time,
            g.tick as f32,
            g.wave_no as f32,
            g.wave_t.max(0.0),
            g.winner as f32,
            g.player as f32,
            g.player_team as f32,
            b2f(g.fog),
            (units.len() / UNIT_FIELDS.len()) as f32,
            (heroes.len() / HERO_FIELDS.len()) as f32,
            (projs.len() / PROJ_FIELDS.len()) as f32,
            (zones.len() / ZONE_FIELDS.len()) as f32,
            (events.len() / EVENT_FIELDS.len()) as f32,
            b2f(near_shop),
            player_hero as f32,
        ]);
    }
}

// ------------------------------------------------------------------------------ static info

fn num(s: &mut String, v: f32) {
    if v.is_finite() {
        let _ = write!(s, "{}", v);
    } else {
        s.push('0');
    }
}

fn arr(s: &mut String, v: &[f32]) {
    s.push('[');
    for (i, x) in v.iter().enumerate() {
        if i > 0 {
            s.push(',');
        }
        num(s, *x);
    }
    s.push(']');
}

fn jstr(s: &mut String, v: &str) {
    s.push('"');
    for c in v.chars() {
        match c {
            '"' => s.push_str("\\\""),
            '\\' => s.push_str("\\\\"),
            '\n' => s.push_str("\\n"),
            c if (c as u32) < 0x20 => {
                let _ = write!(s, "\\u{:04x}", c as u32);
            }
            c => s.push(c),
        }
    }
    s.push('"');
}

fn name_list(s: &mut String, v: &[&str]) {
    s.push('[');
    for (i, n) in v.iter().enumerate() {
        if i > 0 {
            s.push(',');
        }
        jstr(s, n);
    }
    s.push(']');
}

fn pt_list(s: &mut String, pts: &[Vec2]) {
    s.push('[');
    for (i, p) in pts.iter().enumerate() {
        if i > 0 {
            s.push(',');
        }
        arr(s, &[p.x, p.y]);
    }
    s.push(']');
}

fn build_info(g: &Game) -> String {
    let mut s = String::with_capacity(32 * 1024);
    let _ = write!(
        s,
        "{{\"abi\":{},\"tick_hz\":{},\"world\":{},\"cell\":{},\"grid\":{},\"shop_radius\":{},\"max_level\":{},\"inv_slots\":{},\
         \"wave_interval\":{},\"xp_radius\":{},\"start_gold\":{},\"blink_dagger_range\":{}",
        ABI_VERSION, TICK_HZ, WORLD, CELL, GRID, g.content.rules.shop_radius, g.content.max_level(), INV_SLOTS, g.content.rules.wave_interval, g.content.rules.xp_radius, g.content.rules.start_gold, g.content.rules.blink_range
    );
    s.push_str(",\"xp_table\":");
    arr(&mut s, &g.content.xp_table);
    s.push_str(",\"mods\":[");
    for (i, m) in g.content.mods.iter().enumerate() {
        if i > 0 {
            s.push(',');
        }
        let _ = write!(s, "{{\"name\":{},\"version\":{},\"author\":{},\"description\":{}}}", crate::json::quote(&m.name), crate::json::quote(&m.version), crate::json::quote(&m.author), crate::json::quote(&m.description));
    }
    s.push(']');

    // field layouts
    for (key, fields) in [
        ("unit_fields", UNIT_FIELDS),
        ("hero_fields", HERO_FIELDS),
        ("proj_fields", PROJ_FIELDS),
        ("zone_fields", ZONE_FIELDS),
        ("event_fields", EVENT_FIELDS),
        ("global_fields", GLOBAL_FIELDS),
    ] {
        let _ = write!(s, ",\"{}\":", key);
        name_list(&mut s, fields);
    }

    // enums / codes
    let _ = write!(
        s,
        ",\"kind\":{{\"hero\":{},\"melee\":{},\"ranged\":{},\"siege\":{},\"neutral\":{},\"tower\":{},\"ancient\":{}}}",
        Kind::Hero as u8, Kind::Melee as u8, Kind::Ranged as u8, Kind::Siege as u8, Kind::Neutral as u8, Kind::Tower as u8, Kind::Ancient as u8
    );
    let _ = write!(
        s,
        ",\"cast\":{{\"none\":{},\"target\":{},\"line\":{},\"point\":{},\"self_aoe\":{},\"self_buff\":{},\"blink\":{}}}",
        Cast::None as u8, Cast::Target as u8, Cast::Line as u8, Cast::PointAoE as u8, Cast::SelfAoE as u8, Cast::SelfBuff as u8, Cast::Blink as u8
    );
    let _ = write!(
        s,
        ",\"status\":{{\"stun\":{},\"root\":{},\"slow\":{},\"haste\":{},\"aspd\":{},\"salve\":{},\"mana\":{}}}",
        ST_STUN, ST_ROOT, ST_SLOW, ST_HASTE, ST_ASPD, ST_SALVE, ST_MANA
    );
    let _ = write!(
        s,
        ",\"fx\":{{\"stun\":{},\"slow\":{},\"root\":{},\"haste\":{},\"aspd\":{}}}",
        FX_STUN, FX_SLOW, FX_ROOT, FX_HASTE, FX_ASPD
    );
    let _ = write!(
        s,
        ",\"act\":{{\"none\":{},\"salve\":{},\"mana\":{},\"blink\":{}}}",
        ACT_NONE, ACT_SALVE, ACT_MANA, ACT_BLINK
    );
    let _ = write!(
        s,
        ",\"ev\":{{\"damage\":{},\"kill\":{},\"heal\":{},\"level\":{},\"building\":{},\"cast\":{},\"gold\":{},\"gameover\":{},\"wave\":{},\"respawn\":{},\"attack\":{},\"item\":{},\"learn\":{},\"blink\":{},\"assist\":{}}}",
        EV_DAMAGE, EV_KILL, EV_HEAL, EV_LEVEL, EV_BUILDING, EV_CAST, EV_GOLD, EV_GAMEOVER, EV_WAVE, EV_RESPAWN, EV_ATTACK, EV_ITEM, EV_LEARN, EV_BLINK, EV_ASSIST
    );

    // names
    s.push_str(",\"kind_names\":");
    name_list(&mut s, &["Hero", g.content.melee.name, g.content.ranged.name, g.content.siege.name, "Neutral", "Tower", "Ancient"]);
    s.push_str(",\"neutral_names\":");
    name_list(&mut s, &g.content.neutrals.iter().map(|n| n.name).collect::<Vec<_>>());

    // map
    s.push_str(",\"bases\":");
    pt_list(&mut s, &[BASE_RADIANT, BASE_DIRE]);
    s.push_str(",\"lanes\":[");
    for (i, l) in g.map.lanes.iter().enumerate() {
        if i > 0 {
            s.push(',');
        }
        pt_list(&mut s, l);
    }
    s.push_str("],\"camps\":[");
    for (i, c) in g.map.camps.iter().enumerate() {
        if i > 0 {
            s.push(',');
        }
        let _ = write!(s, "{{\"x\":{},\"y\":{},\"tier\":{}}}", c.pos.x, c.pos.y, c.tier);
    }
    s.push(']');

    // heroes
    s.push_str(",\"heroes\":[");
    for (i, h) in g.content.heroes.iter().enumerate() {
        if i > 0 {
            s.push(',');
        }
        s.push_str("{\"name\":");
        jstr(&mut s, h.name);
        s.push_str(",\"title\":");
        jstr(&mut s, h.title);
        let _ = write!(
            s,
            ",\"primary\":{},\"str\":{},\"agi\":{},\"int\":{},\"gain_str\":{},\"gain_agi\":{},\"gain_int\":{},\"dmg_min\":{},\"dmg_max\":{},\"range\":{},\"bat\":{},\"ms\":{},\"proj_speed\":{},\"radius\":{},\"glyph\":{},\"color\":{}}}",
            h.primary, h.str_, h.agi, h.int, h.gain_str, h.gain_agi, h.gain_int, h.dmg_min, h.dmg_max, h.range, h.bat, h.ms, h.proj_speed, h.radius, crate::json::quote(h.glyph), crate::json::quote(h.color)
        );
    }

    // abilities: index = hero * 4 + slot
    s.push_str("],\"abilities\":[");
    for (i, a) in g.content.abilities.iter().enumerate() {
        if i > 0 {
            s.push(',');
        }
        s.push_str("{\"name\":");
        jstr(&mut s, a.name);
        s.push_str(",\"desc\":");
        jstr(&mut s, a.desc);
        let _ = write!(
            s,
            ",\"cast\":{},\"dtype\":{},\"fx\":{},\"max_level\":{},\"speed\":{},\"delay\":{},\"aoe_on_land\":{},\"icon\":{},\"color\":{}",
            a.cast as u8, a.dtype as u8, a.fx, a.max_level, a.speed, a.delay, a.aoe_on_land, crate::json::quote(a.icon), crate::json::quote(a.color)
        );
        let req = a.req_level.map(|v| v as f32);
        for (k, v) in [
            ("req_level", &req),
            ("range", &a.range),
            ("radius", &a.radius),
            ("mana", &a.mana),
            ("cd", &a.cd),
            ("dmg", &a.dmg),
            ("dur", &a.dur),
            ("pct", &a.pct),
            ("pct2", &a.pct2),
        ] {
            let _ = write!(s, ",\"{}\":", k);
            arr(&mut s, v);
        }
        s.push('}');
    }

    // items (index 0 = empty slot)
    s.push_str("],\"items\":[");
    for (i, it) in g.content.items.iter().enumerate() {
        if i > 0 {
            s.push(',');
        }
        s.push_str("{\"name\":");
        jstr(&mut s, it.name);
        let _ = write!(
            s,
            ",\"cost\":{},\"str\":{},\"agi\":{},\"int\":{},\"dmg\":{},\"armor\":{},\"hp\":{},\"mana\":{},\"aspd\":{},\"ms\":{},\"hpreg\":{},\"mpreg\":{},\"active\":{},\"cd\":{},\"stack\":{},\"color\":{}}}",
            it.cost, it.str_, it.agi, it.int, it.dmg, it.armor, it.hp, it.mana, it.aspd, it.ms, it.hpreg, it.mpreg, it.active, it.cd, it.stack, crate::json::quote(it.color)
        );
    }
    s.push_str("]}");
    s
}

// ------------------------------------------------------------------------------ exports

#[no_mangle]
pub extern "C" fn moba_abi_version() -> u32 {
    ABI_VERSION
}

/// Start (or restart) a match. `hero` picks the player's hero, `per_team` is 1..=5,
/// `fog == 0` is sandbox mode (the player's team sees the whole map), `bot != 0` makes
/// the bot AI drive the player's hero too (watch mode).
#[no_mangle]
pub extern "C" fn moba_new(seed: u32, hero: u32, per_team: u32, fog: u32, bot: u32) {
    let content = active_content();
    let cfg = Config {
        seed,
        player_hero: (hero % content.heroes.len() as u32) as u8,
        heroes_per_team: per_team.clamp(1, 5) as u8,
        fog: fog != 0,
        player_is_bot: bot != 0,
    };
    let game = Game::with_content(cfg, content);
    let info = build_info(&game).into_bytes();
    let mut b = Bridge { game, info, units: vec![], heroes: vec![], projs: vec![], zones: vec![], events: vec![], global: vec![] };
    b.snapshot();
    *BRIDGE.lock().unwrap_or_else(|e| e.into_inner()) = Some(b);
}

// ------------------------------------------------------------------------------ mods

/// Largest mod file the staging buffer accepts.
const MAX_STAGE: usize = 8 << 20;

struct ModState {
    /// Content after all successfully loaded mods (`None` = built-in).
    pending: Option<Content>,
    /// Bumped whenever `pending` changes; lets `moba_new` reuse one leaked copy.
    version: u64,
    leaked: Option<(u64, &'static Content)>,
    stage: Vec<u8>,
    error: Vec<u8>,
    export: Vec<u8>,
}

static MODS: Mutex<ModState> = Mutex::new(ModState { pending: None, version: 0, leaked: None, stage: Vec::new(), error: Vec::new(), export: Vec::new() });

fn mods() -> std::sync::MutexGuard<'static, ModState> {
    MODS.lock().unwrap_or_else(|e| e.into_inner())
}

/// The content the next match uses. A modded copy is leaked once per change (matches borrow it
/// for `'static`), so repeated restarts with the same mods allocate nothing.
pub fn active_content() -> &'static Content {
    let mut st = mods();
    if st.pending.is_none() {
        return content::builtin();
    }
    if let Some((v, r)) = st.leaked {
        if v == st.version {
            return r;
        }
    }
    let r: &'static Content = Box::leak(Box::new(st.pending.clone().unwrap_or_else(Content::builtin)));
    let v = st.version;
    st.leaked = Some((v, r));
    r
}

/// Reserve `len` bytes for a mod file and return their address (0 if too large).
#[no_mangle]
pub extern "C" fn moba_alloc(len: usize) -> usize {
    if len > MAX_STAGE {
        return 0;
    }
    let mut st = mods();
    st.stage.clear();
    st.stage.resize(len.max(1), 0);
    st.stage.as_ptr() as usize
}

/// Apply the first `len` staged bytes as a mod on top of the current content.
/// Returns 0 on success, otherwise the number of error messages (see `moba_mod_error_*`).
/// A failed mod changes nothing.
#[no_mangle]
pub extern "C" fn moba_mod_load(len: usize) -> u32 {
    let mut st = mods();
    let n = len.min(st.stage.len());
    let result = {
        let base = st.pending.clone().unwrap_or_else(Content::builtin);
        content::apply_mod(&base, &st.stage[..n])
    };
    match result {
        Ok(c) => {
            st.pending = Some(c);
            st.version += 1;
            st.error.clear();
            0
        }
        Err(list) => {
            st.error = list.join("\n").into_bytes();
            list.len().max(1) as u32
        }
    }
}

#[no_mangle]
pub extern "C" fn moba_mod_error_ptr() -> usize {
    mods().error.as_ptr() as usize
}

#[no_mangle]
pub extern "C" fn moba_mod_error_len() -> usize {
    mods().error.len()
}

/// Drop all mods: the next match uses the built-in content again.
#[no_mangle]
pub extern "C" fn moba_mod_clear() {
    let mut st = mods();
    st.pending = None;
    st.version += 1;
    st.error.clear();
}

#[no_mangle]
pub extern "C" fn moba_mod_count() -> u32 {
    mods().pending.as_ref().map(|c| c.mods.len() as u32).unwrap_or(0)
}

/// Serialise the active content as a mod file; returns its length (read it at `moba_content_ptr`).
#[no_mangle]
pub extern "C" fn moba_content_export() -> usize {
    let text = {
        let st = mods();
        match &st.pending {
            Some(c) => c.to_mod_json(),
            None => content::builtin().to_mod_json(),
        }
    };
    let mut st = mods();
    st.export = text.into_bytes();
    st.export.len()
}

#[no_mangle]
pub extern "C" fn moba_content_ptr() -> usize {
    mods().export.as_ptr() as usize
}

/// Advance the simulation by `n` ticks (1/30 s each).
#[no_mangle]
pub extern "C" fn moba_step(n: u32) {
    with(|b| {
        for _ in 0..n.min(600) {
            b.game.step();
        }
        if b.game.events.len() > 2 * MAX_EVENTS {
            let cut = b.game.events.len() - MAX_EVENTS;
            b.game.events.drain(..cut);
        }
    });
}

/// Rebuild all tables from the current state and drain the event queue.
#[no_mangle]
pub extern "C" fn moba_snapshot() {
    with(|b| b.snapshot());
}

macro_rules! ptr_getter {
    ($name:ident, $field:ident) => {
        #[no_mangle]
        pub extern "C" fn $name() -> usize {
            with(|b| b.$field.as_ptr() as usize).unwrap_or(0)
        }
    };
}
ptr_getter!(moba_units_ptr, units);
ptr_getter!(moba_heroes_ptr, heroes);
ptr_getter!(moba_projs_ptr, projs);
ptr_getter!(moba_zones_ptr, zones);
ptr_getter!(moba_events_ptr, events);
ptr_getter!(moba_global_ptr, global);
ptr_getter!(moba_info_ptr, info);

#[no_mangle]
pub extern "C" fn moba_info_len() -> usize {
    with(|b| b.info.len()).unwrap_or(0)
}

/// `GRID * GRID` terrain codes (see `map::T_*`), row-major.
#[no_mangle]
pub extern "C" fn moba_terrain_ptr() -> usize {
    with(|b| b.game.map.terrain.as_ptr() as usize).unwrap_or(0)
}

/// Low / high 32 bits of the deterministic state hash (desync and replay checks).
#[no_mangle]
pub extern "C" fn moba_hash_lo() -> u32 {
    with(|b| b.game.state_hash() as u32).unwrap_or(0)
}
#[no_mangle]
pub extern "C" fn moba_hash_hi() -> u32 {
    with(|b| (b.game.state_hash() >> 32) as u32).unwrap_or(0)
}

// ---- commands for the player's hero (applied at the start of the next tick)

fn command(make: impl FnOnce(u32) -> Cmd) {
    with(|b| {
        let id = b.game.player;
        b.game.push_cmd(make(id));
    });
}

#[no_mangle]
pub extern "C" fn moba_cmd_move(x: f32, y: f32) {
    command(|unit| Cmd::Move { unit, to: Vec2::new(x, y) });
}
#[no_mangle]
pub extern "C" fn moba_cmd_attack_move(x: f32, y: f32) {
    command(|unit| Cmd::AttackMove { unit, to: Vec2::new(x, y) });
}
#[no_mangle]
pub extern "C" fn moba_cmd_attack(target: u32) {
    command(|unit| Cmd::Attack { unit, target });
}
#[no_mangle]
pub extern "C" fn moba_cmd_stop() {
    command(|unit| Cmd::Stop { unit });
}
#[no_mangle]
pub extern "C" fn moba_cmd_hold() {
    command(|unit| Cmd::Hold { unit });
}
/// `slot` 0..=3 casts an ability, 10..=15 uses an inventory item. `target` is a unit id
/// (0 if none); `x`,`y` is the aimed point.
#[no_mangle]
pub extern "C" fn moba_cmd_cast(slot: u32, target: u32, x: f32, y: f32) {
    command(|unit| Cmd::Cast { unit, slot: slot as u8, target, point: Vec2::new(x, y) });
}
#[no_mangle]
pub extern "C" fn moba_cmd_level_up(slot: u32) {
    command(|unit| Cmd::LevelUp { unit, slot: slot as u8 });
}
#[no_mangle]
pub extern "C" fn moba_cmd_buy(item: u32) {
    command(|unit| Cmd::Buy { unit, item: item as u8 });
}
#[no_mangle]
pub extern "C" fn moba_cmd_sell(slot: u32) {
    command(|unit| Cmd::Sell { unit, slot: slot as u8 });
}

/// Hand the player's hero to the bot AI (`on != 0`) or take control back.
#[no_mangle]
pub extern "C" fn moba_set_bot(on: u32) {
    with(|b| {
        if let Some(i) = b.game.player_idx() {
            let hi = b.game.units[i].hero;
            if hi >= 0 {
                b.game.heroes[hi as usize].bot = on != 0;
            }
        }
    });
}

// ------------------------------------------------------------------------------ tests

#[cfg(test)]
mod tests {
    use super::*;

    fn bridge(cfg: Config) -> Bridge {
        let game = Game::new(cfg);
        let info = build_info(&game).into_bytes();
        let mut b = Bridge { game, info, units: vec![], heroes: vec![], projs: vec![], zones: vec![], events: vec![], global: vec![] };
        b.snapshot();
        b
    }

    #[test]
    fn rows_match_declared_layouts() {
        let mut b = bridge(Config { heroes_per_team: 3, ..Config::default() });
        for _ in 0..900 {
            b.game.step();
        }
        b.snapshot();
        assert_eq!(b.units.len() % UNIT_FIELDS.len(), 0);
        assert_eq!(b.heroes.len(), 6 * HERO_FIELDS.len());
        assert_eq!(b.projs.len() % PROJ_FIELDS.len(), 0);
        assert_eq!(b.zones.len() % ZONE_FIELDS.len(), 0);
        assert_eq!(b.events.len() % EVENT_FIELDS.len(), 0);
        assert_eq!(b.global.len(), GLOBAL_FIELDS.len());
    }

    #[test]
    fn info_is_valid_json_shape() {
        let b = bridge(Config::default());
        let s = String::from_utf8(b.info.clone()).unwrap();
        // brackets / braces balance outside of strings, and there are no NaN/inf tokens
        let (mut depth, mut in_str, mut esc) = (0i32, false, false);
        for c in s.chars() {
            if in_str {
                if esc {
                    esc = false;
                } else if c == '\\' {
                    esc = true;
                } else if c == '"' {
                    in_str = false;
                }
                continue;
            }
            match c {
                '"' => in_str = true,
                '{' | '[' => depth += 1,
                '}' | ']' => depth -= 1,
                _ => {}
            }
            assert!(depth >= 0);
        }
        assert_eq!(depth, 0);
        assert!(!in_str);
        assert!(!s.contains("NaN") && !s.contains("inf"));
        assert!(s.contains("\"Executioner's Axe\""));
    }

    #[test]
    fn fog_hides_enemy_units_but_not_buildings() {
        let mut b = bridge(Config::default());
        b.snapshot();
        let stride = UNIT_FIELDS.len();
        let col = |n: &str| UNIT_FIELDS.iter().position(|f| *f == n).unwrap();
        let (team_c, kind_c, vis_c) = (col("team"), col("kind"), col("visible"));
        let mut enemy_buildings = 0;
        for row in b.units.chunks(stride) {
            if row[team_c] == 1.0 {
                // every enemy row is either a building, or genuinely visible
                if row[kind_c] >= Kind::Tower as u8 as f32 {
                    enemy_buildings += 1;
                    assert_eq!(row[vis_c], 0.0, "distant enemy buildings are flagged hidden");
                } else {
                    assert_eq!(row[vis_c], 1.0, "hidden enemy units must not be exported");
                }
            }
        }
        assert_eq!(enemy_buildings, 10, "9 towers + ancient are always listed");
    }

    #[test]
    fn sandbox_mode_reveals_everything() {
        let b = bridge(Config { fog: false, ..Config::default() });
        let stride = UNIT_FIELDS.len();
        let vis_c = UNIT_FIELDS.iter().position(|f| *f == "visible").unwrap();
        assert!(b.units.chunks(stride).all(|r| r[vis_c] == 1.0));
        // the enemy hero (spawned in the Dire base) is exported although nobody can "see" it
        let hero_c = UNIT_FIELDS.iter().position(|f| *f == "kind").unwrap();
        assert_eq!(b.units.chunks(stride).filter(|r| r[hero_c] == 0.0).count(), 2);
    }

    #[test]
    fn snapshot_drains_events() {
        let mut b = bridge(Config::default());
        b.game.run(300); // first creep wave spawns at 8 s -> EV_WAVE
        b.snapshot();
        assert!(b.events.len() >= EVENT_FIELDS.len());
        b.snapshot();
        assert!(b.events.is_empty());
    }
}
