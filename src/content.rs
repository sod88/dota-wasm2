//! Moddable game content.
//!
//! Everything a modder can change lives in one [`Content`] value: heroes, abilities, items,
//! creeps, neutrals, camps, towers, the Ancient, the XP table and the game [`Rules`].
//! The built-in game is `Content::builtin()`; a mod is a JSON document that patches it
//! (`apply_mod`). The same format, validation rules and error messages are implemented by
//! the C++ engine (`cpp/content.cpp`), so a mod behaves identically in both.
//!
//! Mod format (all keys optional unless noted; unknown keys are errors, `"comment"` is ignored):
//! ```text
//! { "format": 1, "name": .., "version": .., "author": .., "description": ..,
//!   "replace": ["heroes" | "items" | "neutrals", ..],
//!   "rules": {..}, "xp_table": [..],
//!   "items": [..], "neutrals": [..], "camps": [[names], [names], [names]],
//!   "creeps": {"melee": {..}, "ranged": {..}, "siege": {..}},
//!   "towers": [{..} | null, ..], "ancient": {..},
//!   "heroes": [{ "name": .., .., "abilities": [{..} x4] }] }
//! ```
//! List entries are matched by `name`: an existing name patches that entry (only the keys you
//! give change), a new name appends a new entry.

use crate::data::*;
use crate::json::{self, Value};
use std::collections::BTreeMap;
use std::sync::{Mutex, OnceLock};

pub const MAX_HEROES: usize = 24;
pub const MAX_ITEMS: usize = 100;
pub const MAX_NEUTRALS: usize = 16;
pub const MAX_MOD_BYTES: usize = 1 << 20;
const MAX_ERRORS: usize = 20;

// ------------------------------------------------------------------------------ rules

macro_rules! rules {
    ($( $name:ident : $ty:ident = $def:expr, $lo:expr, $hi:expr; )*) => {
        /// Tunable game rules (see `data.rs` for what the defaults reproduce).
        #[derive(Clone, Debug, PartialEq)]
        pub struct Rules { $( pub $name: $ty, )* }

        impl Rules {
            pub fn builtin() -> Rules { Rules { $( $name: $def, )* } }

            fn patch(&mut self, cx: &mut Cx, path: &str, obj: &[(String, Value)]) {
                for (k, v) in obj {
                    if k == "comment" { continue; }
                    let p = format!("{}.{}", path, k);
                    match k.as_str() {
                        $( stringify!($name) => {
                            if let Some(n) = get_num(cx, &p, v, $lo as f64, $hi as f64) {
                                if stringify!($ty) == "u32" && n.fract() != 0.0 {
                                    cx.err(&p, "must be a whole number".to_string());
                                } else {
                                    self.$name = n as $ty;
                                }
                            }
                        } )*
                        _ => cx.err(&p, "unknown key".to_string()),
                    }
                }
            }

            fn to_value(&self) -> Value {
                Value::Obj(vec![ $( (stringify!($name).to_string(), num_value(&(self.$name as f64), stringify!($ty) == "f32", self.$name as f32)), )* ])
            }
        }
    };
}

fn num_value(d: &f64, is_f32: bool, f: f32) -> Value {
    if is_f32 {
        Value::Float(f)
    } else {
        Value::Num(*d)
    }
}

rules! {
    start_gold: f32 = START_GOLD, 0, 1000000;
    passive_gold: f32 = PASSIVE_GOLD_PER_SEC, 0, 10000;
    wave_interval: f32 = WAVE_INTERVAL, 1, 600;
    first_wave_at: f32 = FIRST_WAVE_AT, 0, 600;
    camp_respawn: f32 = CAMP_RESPAWN, 1, 3600;
    xp_radius: f32 = XP_RADIUS, 0, 5000;
    shop_radius: f32 = SHOP_RADIUS, 0, 5000;
    magic_resist: f32 = MAGIC_RESIST, 0, 0.95;
    blink_range: f32 = BLINK_DAGGER_RANGE, 0, 5000;
    salve_heal: f32 = SALVE_HEAL, 0, 100000;
    salve_time: f32 = SALVE_TIME, 0.1, 300;
    mana_potion_restore: f32 = MANA_POTION_RESTORE, 0, 100000;
    mana_potion_time: f32 = MANA_POTION_TIME, 0.1, 300;
    hero_vision: f32 = 420.0, 0, 5000;
    hp_base: f32 = 200.0, 1, 100000;
    hp_per_str: f32 = 20.0, 0, 10000;
    mana_base: f32 = 50.0, 0, 100000;
    mana_per_int: f32 = 12.0, 0, 10000;
    armor_base: f32 = 1.0, -50, 100;
    armor_per_agi: f32 = 0.15, -5, 5;
    hp_regen_base: f32 = 1.0, 0, 1000;
    hp_regen_per_str: f32 = 0.12, 0, 100;
    mana_regen_base: f32 = 0.5, 0, 1000;
    mana_regen_per_int: f32 = 0.05, 0, 100;
    respawn_base: f32 = 5.0, 0, 600;
    respawn_per_level: f32 = 2.2, 0, 100;
    respawn_max: f32 = 60.0, 0, 600;
    hero_bounty_base: f32 = 120.0, 0, 100000;
    hero_bounty_per_level: f32 = 14.0, 0, 10000;
    hero_xp_base: f32 = 60.0, 0, 100000;
    hero_xp_per_level: f32 = 22.0, 0, 10000;
    wave_melee: u32 = 3, 0, 20;
    wave_ranged: u32 = 1, 0, 20;
    wave_siege: u32 = 1, 0, 10;
    siege_every: u32 = 5, 0, 1000;
}

// ------------------------------------------------------------------------------ content

#[derive(Clone, Debug, PartialEq)]
pub struct ModInfo {
    pub name: String,
    pub version: String,
    pub author: String,
    pub description: String,
}

#[derive(Clone, Debug, PartialEq)]
pub struct Content {
    pub heroes: Vec<HeroDef>,
    /// `heroes.len() * 4`; index = hero * 4 + slot.
    pub abilities: Vec<AbilityDef>,
    /// Index 0 is "no item".
    pub items: Vec<ItemDef>,
    pub melee: CreepDef,
    pub ranged: CreepDef,
    pub siege: CreepDef,
    pub neutrals: Vec<CreepDef>,
    /// Neutral indices per camp tier (3 tiers).
    pub camp_comp: Vec<Vec<u8>>,
    pub towers: [TowerDef; 3],
    pub ancient: AncientDef,
    /// Cumulative XP to reach level `i + 1`; its length is the level cap.
    pub xp_table: Vec<f32>,
    pub rules: Rules,
    /// Mods applied so far (informational).
    pub mods: Vec<ModInfo>,
}

impl Content {
    pub fn builtin() -> Content {
        Content {
            heroes: HEROES.to_vec(),
            abilities: ABILITIES.to_vec(),
            items: ITEMS.to_vec(),
            melee: MELEE_CREEP,
            ranged: RANGED_CREEP,
            siege: SIEGE_CREEP,
            neutrals: NEUTRALS.to_vec(),
            camp_comp: CAMP_COMP.iter().map(|c| c.to_vec()).collect(),
            towers: TOWERS,
            ancient: AncientDef { hp: ANCIENT_HP, armor: ANCIENT_ARMOR, radius: ANCIENT_RADIUS, vision: ANCIENT_VISION },
            xp_table: XP_TABLE.to_vec(),
            rules: Rules::builtin(),
            mods: Vec::new(),
        }
    }

    pub fn max_level(&self) -> usize {
        self.xp_table.len()
    }

    /// The content as a mod file: applying it to the built-in content changes nothing.
    pub fn to_mod_json(&self) -> String {
        json::to_pretty(&self.to_value())
    }

    fn to_value(&self) -> Value {
        let s = |t: &str| Value::Str(t.to_string());
        let f = |v: f32| Value::Float(v);
        let lvl = |a: &[f32; 4]| {
            if a.iter().all(|x| *x == a[0]) {
                Value::Float(a[0])
            } else {
                Value::Arr(a.iter().map(|x| Value::Float(*x)).collect())
            }
        };
        let item_name = |i: u8| self.items[i as usize].name;
        let mut o: Vec<(String, Value)> = vec![
            ("format".into(), Value::Num(1.0)),
            ("name".into(), s(self.mods.last().map(|m| m.name.as_str()).unwrap_or("Built-in content"))),
            ("comment".into(), s("Every value of the built-in game. Applying this file changes nothing; copy the parts you want to change into your own mod.")),
            ("rules".into(), self.rules.to_value()),
            ("xp_table".into(), Value::Arr(self.xp_table.iter().map(|x| f(*x)).collect())),
        ];
        // items (skip the empty slot 0)
        let items: Vec<Value> = self.items[1..]
            .iter()
            .map(|it| {
                let mut v = vec![("name".to_string(), s(it.name)), ("cost".into(), f(it.cost))];
                for (k, x) in [("str", it.str_), ("agi", it.agi), ("int", it.int), ("dmg", it.dmg), ("armor", it.armor), ("hp", it.hp), ("mana", it.mana), ("aspd", it.aspd), ("ms", it.ms), ("hpreg", it.hpreg), ("mpreg", it.mpreg)] {
                    if x != 0.0 {
                        v.push((k.to_string(), f(x)));
                    }
                }
                if it.active != ACT_NONE {
                    v.push(("active".into(), s(active_name(it.active))));
                }
                if it.cd != 0.0 {
                    v.push(("cd".into(), f(it.cd)));
                }
                if it.stack {
                    v.push(("stack".into(), Value::Bool(true)));
                }
                if !it.color.is_empty() {
                    v.push(("color".into(), s(it.color)));
                }
                Value::Obj(v)
            })
            .collect();
        o.push(("items".into(), Value::Arr(items)));
        let creep = |c: &CreepDef| creep_value(c);
        o.push(("creeps".into(), Value::Obj(vec![("melee".into(), creep(&self.melee)), ("ranged".into(), creep(&self.ranged)), ("siege".into(), creep(&self.siege))])));
        o.push(("neutrals".into(), Value::Arr(self.neutrals.iter().map(creep).collect())));
        o.push((
            "camps".into(),
            Value::Arr(self.camp_comp.iter().map(|c| Value::Arr(c.iter().map(|&n| s(self.neutrals[n as usize].name)).collect())).collect()),
        ));
        o.push((
            "towers".into(),
            Value::Arr(
                self.towers
                    .iter()
                    .map(|t| {
                        Value::Obj(vec![
                            ("hp".into(), f(t.hp)),
                            ("dmg_min".into(), f(t.dmg_min)),
                            ("dmg_max".into(), f(t.dmg_max)),
                            ("armor".into(), f(t.armor)),
                            ("range".into(), f(t.range)),
                            ("bat".into(), f(t.bat)),
                            ("gold".into(), f(t.gold)),
                            ("lane_frac".into(), f(t.lane_frac)),
                        ])
                    })
                    .collect(),
            ),
        ));
        o.push(("ancient".into(), Value::Obj(vec![("hp".into(), f(self.ancient.hp)), ("armor".into(), f(self.ancient.armor)), ("radius".into(), f(self.ancient.radius)), ("vision".into(), f(self.ancient.vision))])));
        let heroes: Vec<Value> = self
            .heroes
            .iter()
            .enumerate()
            .map(|(hi, h)| {
                let abilities: Vec<Value> = (0..4)
                    .map(|sl| {
                        let a = &self.abilities[hi * 4 + sl];
                        let mut v = vec![
                            ("name".to_string(), s(a.name)),
                            ("desc".into(), s(a.desc)),
                            ("cast".into(), s(cast_name(a.cast))),
                            ("dtype".into(), s(dtype_name(a.dtype))),
                            ("fx".into(), Value::Arr(fx_names(a.fx).into_iter().map(s).collect())),
                            ("max_level".into(), Value::Num(a.max_level as f64)),
                            ("req_level".into(), Value::Arr(a.req_level.iter().map(|x| Value::Num(*x as f64)).collect())),
                            ("range".into(), lvl(&a.range)),
                            ("radius".into(), lvl(&a.radius)),
                            ("mana".into(), lvl(&a.mana)),
                            ("cd".into(), lvl(&a.cd)),
                            ("dmg".into(), lvl(&a.dmg)),
                            ("dur".into(), lvl(&a.dur)),
                            ("pct".into(), lvl(&a.pct)),
                            ("pct2".into(), lvl(&a.pct2)),
                            ("speed".into(), f(a.speed)),
                            ("delay".into(), f(a.delay)),
                            ("aoe_on_land".into(), Value::Bool(a.aoe_on_land)),
                        ];
                        if !a.icon.is_empty() {
                            v.push(("icon".into(), s(a.icon)));
                        }
                        if !a.color.is_empty() {
                            v.push(("color".into(), s(a.color)));
                        }
                        Value::Obj(v)
                    })
                    .collect();
                let mut v = vec![
                    ("name".to_string(), s(h.name)),
                    ("title".into(), s(h.title)),
                    ("primary".into(), s(["str", "agi", "int"][h.primary.min(2) as usize])),
                    ("str".into(), f(h.str_)),
                    ("agi".into(), f(h.agi)),
                    ("int".into(), f(h.int)),
                    ("gain_str".into(), f(h.gain_str)),
                    ("gain_agi".into(), f(h.gain_agi)),
                    ("gain_int".into(), f(h.gain_int)),
                    ("dmg_min".into(), f(h.dmg_min)),
                    ("dmg_max".into(), f(h.dmg_max)),
                    ("range".into(), f(h.range)),
                    ("bat".into(), f(h.bat)),
                    ("ms".into(), f(h.ms)),
                    ("proj_speed".into(), f(h.proj_speed)),
                    ("radius".into(), f(h.radius)),
                    ("build".into(), Value::Arr(h.build.iter().map(|&i| s(item_name(i))).collect())),
                    ("skill_order".into(), Value::Arr(h.skill_order.iter().map(|x| Value::Num(*x as f64)).collect())),
                ];
                if !h.glyph.is_empty() {
                    v.push(("glyph".into(), s(h.glyph)));
                }
                if !h.color.is_empty() {
                    v.push(("color".into(), s(h.color)));
                }
                v.push(("abilities".into(), Value::Arr(abilities)));
                Value::Obj(v)
            })
            .collect();
        o.push(("heroes".into(), Value::Arr(heroes)));
        Value::Obj(o)
    }
}

fn creep_value(c: &CreepDef) -> Value {
    let f = |v: f32| Value::Float(v);
    Value::Obj(vec![
        ("name".into(), Value::Str(c.name.to_string())),
        ("hp".into(), f(c.hp)),
        ("dmg_min".into(), f(c.dmg_min)),
        ("dmg_max".into(), f(c.dmg_max)),
        ("armor".into(), f(c.armor)),
        ("range".into(), f(c.range)),
        ("bat".into(), f(c.bat)),
        ("ms".into(), f(c.ms)),
        ("proj_speed".into(), f(c.proj_speed)),
        ("radius".into(), f(c.radius)),
        ("vision".into(), f(c.vision)),
        ("gold".into(), f(c.gold)),
        ("xp".into(), f(c.xp)),
        ("bldg_mult".into(), f(c.bldg_mult)),
    ])
}

// ------------------------------------------------------------------------------ enum names

const CASTS: [(&str, Cast); 7] = [
    ("none", Cast::None),
    ("target", Cast::Target),
    ("line", Cast::Line),
    ("point", Cast::PointAoE),
    ("self_aoe", Cast::SelfAoE),
    ("self_buff", Cast::SelfBuff),
    ("blink", Cast::Blink),
];
const DTYPES: [(&str, DmgType); 3] = [("physical", DmgType::Physical), ("magical", DmgType::Magical), ("pure", DmgType::Pure)];
const FXS: [(&str, u8); 5] = [("stun", FX_STUN), ("slow", FX_SLOW), ("root", FX_ROOT), ("haste", FX_HASTE), ("aspd", FX_ASPD)];
const ACTIVES: [(&str, u8); 4] = [("none", ACT_NONE), ("salve", ACT_SALVE), ("mana", ACT_MANA), ("blink", ACT_BLINK)];
const PRIMARIES: [&str; 3] = ["str", "agi", "int"];

fn cast_name(c: Cast) -> &'static str {
    CASTS.iter().find(|(_, v)| *v == c).map(|(n, _)| *n).unwrap_or("none")
}
fn dtype_name(c: DmgType) -> &'static str {
    DTYPES.iter().find(|(_, v)| *v == c).map(|(n, _)| *n).unwrap_or("magical")
}
fn active_name(a: u8) -> &'static str {
    ACTIVES.iter().find(|(_, v)| *v == a).map(|(n, _)| *n).unwrap_or("none")
}
fn fx_names(fx: u8) -> Vec<&'static str> {
    FXS.iter().filter(|(_, bit)| fx & bit != 0).map(|(n, _)| *n).collect()
}
fn names_of<T>(t: &[(&str, T)]) -> String {
    t.iter().map(|(n, _)| *n).collect::<Vec<_>>().join(", ")
}

// ------------------------------------------------------------------------------ interning

static INTERN: Mutex<BTreeMap<String, &'static str>> = Mutex::new(BTreeMap::new());

/// Mod strings must be `&'static str` (the defs are `Copy`); equal strings share one allocation.
fn intern(s: &str) -> &'static str {
    let mut m = INTERN.lock().unwrap_or_else(|e| e.into_inner());
    if let Some(v) = m.get(s) {
        return v;
    }
    let leaked: &'static str = Box::leak(s.to_string().into_boxed_str());
    m.insert(s.to_string(), leaked);
    leaked
}

static BUILTIN: OnceLock<Content> = OnceLock::new();

/// The shared built-in content (what `Game::new` uses).
pub fn builtin() -> &'static Content {
    BUILTIN.get_or_init(Content::builtin)
}

// ------------------------------------------------------------------------------ validation helpers

struct Cx {
    errs: Vec<String>,
}

impl Cx {
    fn err(&mut self, path: &str, msg: String) {
        if self.errs.len() < MAX_ERRORS {
            self.errs.push(format!("{}: {}", path, msg));
        } else if self.errs.len() == MAX_ERRORS {
            self.errs.push("... more errors omitted".to_string());
        }
    }
}

fn fmt_bound(v: f64) -> String {
    format!("{}", v)
}

fn get_num(cx: &mut Cx, path: &str, v: &Value, lo: f64, hi: f64) -> Option<f64> {
    let n = match v {
        Value::Num(n) => *n,
        other => {
            cx.err(path, format!("expected a number, found {}", other.type_name()));
            return None;
        }
    };
    if n < lo || n > hi {
        cx.err(path, format!("must be between {} and {}", fmt_bound(lo), fmt_bound(hi)));
        return None;
    }
    Some(n)
}

fn get_int(cx: &mut Cx, path: &str, v: &Value, lo: i64, hi: i64) -> Option<i64> {
    let n = get_num(cx, path, v, lo as f64, hi as f64)?;
    if n.fract() != 0.0 {
        cx.err(path, "must be a whole number".to_string());
        return None;
    }
    Some(n as i64)
}

fn get_str(cx: &mut Cx, path: &str, v: &Value, max: usize, allow_empty: bool) -> Option<String> {
    match v {
        Value::Str(s) => {
            if s.is_empty() && !allow_empty {
                cx.err(path, "must not be empty".to_string());
                None
            } else if s.len() > max {
                cx.err(path, format!("is too long (at most {} bytes)", max));
                None
            } else {
                Some(s.clone())
            }
        }
        other => {
            cx.err(path, format!("expected a string, found {}", other.type_name()));
            None
        }
    }
}

fn get_color(cx: &mut Cx, path: &str, v: &Value) -> Option<&'static str> {
    let s = get_str(cx, path, v, 7, true)?;
    let ok = s.is_empty() || (s.len() == 7 && s.starts_with('#') && s[1..].chars().all(|c| c.is_ascii_hexdigit()));
    if !ok {
        cx.err(path, "expected a colour like \"#ff8800\"".to_string());
        return None;
    }
    Some(intern(&s))
}

fn get_bool(cx: &mut Cx, path: &str, v: &Value) -> Option<bool> {
    match v {
        Value::Bool(b) => Some(*b),
        other => {
            cx.err(path, format!("expected true or false, found {}", other.type_name()));
            None
        }
    }
}

fn get_arr<'a>(cx: &mut Cx, path: &str, v: &'a Value) -> Option<&'a Vec<Value>> {
    match v {
        Value::Arr(a) => Some(a),
        other => {
            cx.err(path, format!("expected an array, found {}", other.type_name()));
            None
        }
    }
}

fn get_obj<'a>(cx: &mut Cx, path: &str, v: &'a Value) -> Option<&'a Vec<(String, Value)>> {
    match v {
        Value::Obj(o) => Some(o),
        other => {
            cx.err(path, format!("expected an object, found {}", other.type_name()));
            None
        }
    }
}

fn get_enum<T: Copy>(cx: &mut Cx, path: &str, v: &Value, table: &[(&str, T)], what: &str) -> Option<T> {
    let s = get_str(cx, path, v, 40, true)?;
    match table.iter().find(|(n, _)| *n == s) {
        Some((_, t)) => Some(*t),
        None => {
            cx.err(path, format!("unknown {} \"{}\" (expected one of: {})", what, s, names_of(table)));
            None
        }
    }
}

/// A per-level value: one number (all levels) or 1-4 numbers (the last one repeats).
fn get_levels(cx: &mut Cx, path: &str, v: &Value, lo: f64, hi: f64, out: &mut [f32; 4]) {
    match v {
        Value::Num(_) => {
            if let Some(n) = get_num(cx, path, v, lo, hi) {
                *out = [n as f32; 4];
            }
        }
        Value::Arr(a) if (1..=4).contains(&a.len()) => {
            let mut tmp = *out;
            let mut ok = true;
            for (i, x) in a.iter().enumerate() {
                match get_num(cx, &format!("{}[{}]", path, i), x, lo, hi) {
                    Some(n) => tmp[i] = n as f32,
                    None => ok = false,
                }
            }
            if ok {
                for i in a.len()..4 {
                    tmp[i] = tmp[a.len() - 1];
                }
                *out = tmp;
            }
        }
        _ => cx.err(path, "expected a number or an array of 1 to 4 numbers".to_string()),
    }
}

fn get_req_levels(cx: &mut Cx, path: &str, v: &Value, out: &mut [u8; 4]) {
    let is_arr = matches!(v, Value::Arr(_));
    let vals: Vec<&Value> = match v {
        Value::Arr(a) if (1..=4).contains(&a.len()) => a.iter().collect(),
        Value::Num(_) => vec![v],
        _ => {
            cx.err(path, "expected a number or an array of 1 to 4 numbers".to_string());
            return;
        }
    };
    let mut tmp = *out;
    let mut ok = true;
    for (i, x) in vals.iter().enumerate() {
        let p = if is_arr { format!("{}[{}]", path, i) } else { path.to_string() };
        match get_int(cx, &p, x, 1, 99) {
            Some(n) => tmp[i] = n as u8,
            None => ok = false,
        }
    }
    if ok {
        for i in vals.len()..4 {
            tmp[i] = tmp[vals.len() - 1];
        }
        *out = tmp;
    }
}

macro_rules! numf {
    ($cx:expr, $p:expr, $v:expr, $slot:expr, $lo:expr, $hi:expr) => {
        if let Some(n) = get_num($cx, $p, $v, $lo as f64, $hi as f64) {
            $slot = n as f32;
        }
    };
}

fn require(cx: &mut Cx, path: &str, seen: &[String], keys: &[&str]) {
    for k in keys {
        if !seen.iter().any(|s| s == k) {
            cx.err(path, format!("missing required key \"{}\"", k));
        }
    }
}

// ------------------------------------------------------------------------------ patching

fn patch_ability(cx: &mut Cx, path: &str, obj: &[(String, Value)], a: &mut AbilityDef, is_new: bool) {
    let mut seen: Vec<String> = Vec::new();
    for (k, v) in obj {
        if k == "comment" {
            continue;
        }
        seen.push(k.clone());
        let p = format!("{}.{}", path, k);
        match k.as_str() {
            "name" => {
                if let Some(s) = get_str(cx, &p, v, 40, false) {
                    a.name = intern(&s);
                }
            }
            "desc" => {
                if let Some(s) = get_str(cx, &p, v, 300, true) {
                    a.desc = intern(&s);
                }
            }
            "cast" => {
                if let Some(c) = get_enum(cx, &p, v, &CASTS, "cast type") {
                    a.cast = c;
                }
            }
            "dtype" => {
                if let Some(c) = get_enum(cx, &p, v, &DTYPES, "damage type") {
                    a.dtype = c;
                }
            }
            "fx" => {
                if let Some(arr) = get_arr(cx, &p, v) {
                    let mut bits = 0u8;
                    let mut ok = true;
                    for (i, x) in arr.iter().enumerate() {
                        match get_enum(cx, &format!("{}[{}]", p, i), x, &FXS, "effect") {
                            Some(b) => bits |= b,
                            None => ok = false,
                        }
                    }
                    if ok {
                        a.fx = bits;
                    }
                }
            }
            "max_level" => {
                if let Some(n) = get_int(cx, &p, v, 1, 4) {
                    a.max_level = n as u8;
                }
            }
            "req_level" => get_req_levels(cx, &p, v, &mut a.req_level),
            "range" => get_levels(cx, &p, v, 0.0, 5000.0, &mut a.range),
            "radius" => get_levels(cx, &p, v, 0.0, 2000.0, &mut a.radius),
            "mana" => get_levels(cx, &p, v, 0.0, 100000.0, &mut a.mana),
            "cd" => get_levels(cx, &p, v, 0.0, 10000.0, &mut a.cd),
            "dmg" => get_levels(cx, &p, v, 0.0, 1000000.0, &mut a.dmg),
            "dur" => get_levels(cx, &p, v, 0.0, 300.0, &mut a.dur),
            "pct" => get_levels(cx, &p, v, 0.0, 1000.0, &mut a.pct),
            "pct2" => get_levels(cx, &p, v, 0.0, 1000.0, &mut a.pct2),
            "speed" => numf!(cx, &p, v, a.speed, 0, 10000),
            "delay" => numf!(cx, &p, v, a.delay, 0, 30),
            "aoe_on_land" => {
                if let Some(b) = get_bool(cx, &p, v) {
                    a.aoe_on_land = b;
                }
            }
            "icon" => {
                if let Some(s) = get_str(cx, &p, v, 24, true) {
                    a.icon = intern(&s);
                }
            }
            "color" => {
                if let Some(c) = get_color(cx, &p, v) {
                    a.color = c;
                }
            }
            _ => cx.err(&p, "unknown key".to_string()),
        }
    }
    if is_new {
        require(cx, path, &seen, &["name", "cast"]);
    }
}

fn find_key<'a>(obj: &'a [(String, Value)], key: &str) -> Option<&'a Value> {
    obj.iter().find(|(k, _)| k == key).map(|(_, v)| v)
}

/// Name of a list entry (`name` is required and must be a string).
fn entry_name(cx: &mut Cx, path: &str, obj: &[(String, Value)]) -> Option<String> {
    match find_key(obj, "name") {
        None => {
            cx.err(path, "missing required key \"name\"".to_string());
            None
        }
        Some(v) => get_str(cx, &format!("{}.name", path), v, 40, false),
    }
}

fn patch_hero(cx: &mut Cx, path: &str, obj: &[(String, Value)], c_items: &[ItemDef], h: &mut HeroDef, abilities: &mut [AbilityDef], is_new: bool) {
    let mut seen: Vec<String> = Vec::new();
    for (k, v) in obj {
        if k == "comment" {
            continue;
        }
        seen.push(k.clone());
        let p = format!("{}.{}", path, k);
        match k.as_str() {
            "name" => {} // already used to find the entry
            "title" => {
                if let Some(s) = get_str(cx, &p, v, 80, true) {
                    h.title = intern(&s);
                }
            }
            "primary" => {
                let tbl: Vec<(&str, u8)> = PRIMARIES.iter().enumerate().map(|(i, n)| (*n, i as u8)).collect();
                if let Some(x) = get_enum(cx, &p, v, &tbl, "primary attribute") {
                    h.primary = x;
                }
            }
            "str" => numf!(cx, &p, v, h.str_, 0, 1000),
            "agi" => numf!(cx, &p, v, h.agi, 0, 1000),
            "int" => numf!(cx, &p, v, h.int, 0, 1000),
            "gain_str" => numf!(cx, &p, v, h.gain_str, 0, 100),
            "gain_agi" => numf!(cx, &p, v, h.gain_agi, 0, 100),
            "gain_int" => numf!(cx, &p, v, h.gain_int, 0, 100),
            "dmg_min" => numf!(cx, &p, v, h.dmg_min, 0, 100000),
            "dmg_max" => numf!(cx, &p, v, h.dmg_max, 0, 100000),
            "range" => numf!(cx, &p, v, h.range, 0, 5000),
            "bat" => numf!(cx, &p, v, h.bat, 0.1, 20),
            "ms" => numf!(cx, &p, v, h.ms, 0, 2000),
            "proj_speed" => numf!(cx, &p, v, h.proj_speed, 0, 10000),
            "radius" => numf!(cx, &p, v, h.radius, 1, 500),
            "glyph" => {
                if let Some(s) = get_str(cx, &p, v, 24, true) {
                    h.glyph = intern(&s);
                }
            }
            "color" => {
                if let Some(c) = get_color(cx, &p, v) {
                    h.color = c;
                }
            }
            "build" => {
                if let Some(arr) = get_arr(cx, &p, v) {
                    if arr.len() > 24 {
                        cx.err(&p, "has too many items (at most 24)".to_string());
                    } else {
                        let mut ids: Vec<u8> = Vec::new();
                        let mut ok = true;
                        for (i, x) in arr.iter().enumerate() {
                            let ip = format!("{}[{}]", p, i);
                            if let Some(n) = get_str(cx, &ip, x, 40, false) {
                                match c_items.iter().position(|it| it.name == n) {
                                    Some(idx) if idx > 0 => ids.push(idx as u8),
                                    _ => {
                                        cx.err(&ip, format!("unknown item \"{}\"", n));
                                        ok = false;
                                    }
                                }
                            } else {
                                ok = false;
                            }
                        }
                        if ok {
                            h.build = Box::leak(ids.into_boxed_slice());
                        }
                    }
                }
            }
            "skill_order" => {
                if let Some(arr) = get_arr(cx, &p, v) {
                    if arr.len() != 3 {
                        cx.err(&p, "expected exactly 3 ability numbers (0, 1 or 2)".to_string());
                    } else {
                        let mut tmp = h.skill_order;
                        let mut ok = true;
                        for (i, x) in arr.iter().enumerate() {
                            match get_int(cx, &format!("{}[{}]", p, i), x, 0, 2) {
                                Some(n) => tmp[i] = n as u8,
                                None => ok = false,
                            }
                        }
                        if ok {
                            h.skill_order = tmp;
                        }
                    }
                }
            }
            "abilities" => {
                if let Some(arr) = get_arr(cx, &p, v) {
                    if arr.len() > 4 || (is_new && arr.len() != 4) {
                        cx.err(&p, if is_new { "a new hero needs exactly 4 abilities".to_string() } else { "expected at most 4 abilities".to_string() });
                    } else {
                        for (i, av) in arr.iter().enumerate() {
                            if matches!(av, Value::Null) && !is_new {
                                continue;
                            }
                            let ap = format!("{}[{}]", p, i);
                            if let Some(ao) = get_obj(cx, &ap, av) {
                                patch_ability(cx, &ap, ao, &mut abilities[i], is_new);
                            }
                        }
                    }
                }
            }
            _ => cx.err(&p, "unknown key".to_string()),
        }
    }
    if is_new {
        require(cx, path, &seen, &["abilities"]);
    }
    if h.dmg_min > h.dmg_max {
        cx.err(path, "dmg_min must not exceed dmg_max".to_string());
    }
}

fn patch_item(cx: &mut Cx, path: &str, obj: &[(String, Value)], it: &mut ItemDef, is_new: bool) {
    let mut seen: Vec<String> = Vec::new();
    for (k, v) in obj {
        if k == "comment" {
            continue;
        }
        seen.push(k.clone());
        let p = format!("{}.{}", path, k);
        match k.as_str() {
            "name" => {}
            "cost" => numf!(cx, &p, v, it.cost, 0, 1000000),
            "str" => numf!(cx, &p, v, it.str_, -1000, 1000),
            "agi" => numf!(cx, &p, v, it.agi, -1000, 1000),
            "int" => numf!(cx, &p, v, it.int, -1000, 1000),
            "dmg" => numf!(cx, &p, v, it.dmg, -10000, 10000),
            "armor" => numf!(cx, &p, v, it.armor, -100, 100),
            "hp" => numf!(cx, &p, v, it.hp, -100000, 100000),
            "mana" => numf!(cx, &p, v, it.mana, -100000, 100000),
            "aspd" => numf!(cx, &p, v, it.aspd, -1000, 1000),
            "ms" => numf!(cx, &p, v, it.ms, -1000, 1000),
            "hpreg" => numf!(cx, &p, v, it.hpreg, -1000, 1000),
            "mpreg" => numf!(cx, &p, v, it.mpreg, -1000, 1000),
            "active" => {
                if let Some(a) = get_enum(cx, &p, v, &ACTIVES, "active effect") {
                    it.active = a;
                }
            }
            "cd" => numf!(cx, &p, v, it.cd, 0, 10000),
            "stack" => {
                if let Some(b) = get_bool(cx, &p, v) {
                    it.stack = b;
                }
            }
            "color" => {
                if let Some(c) = get_color(cx, &p, v) {
                    it.color = c;
                }
            }
            _ => cx.err(&p, "unknown key".to_string()),
        }
    }
    if is_new {
        require(cx, path, &seen, &["cost"]);
    }
}

fn patch_creep(cx: &mut Cx, path: &str, obj: &[(String, Value)], c: &mut CreepDef, is_new: bool, name_ok: bool) {
    let mut seen: Vec<String> = Vec::new();
    for (k, v) in obj {
        if k == "comment" {
            continue;
        }
        seen.push(k.clone());
        let p = format!("{}.{}", path, k);
        match k.as_str() {
            "name" => {
                if name_ok {
                    if let Some(s) = get_str(cx, &p, v, 40, false) {
                        c.name = intern(&s);
                    }
                }
            }
            "hp" => numf!(cx, &p, v, c.hp, 1, 10000000),
            "dmg_min" => numf!(cx, &p, v, c.dmg_min, 0, 100000),
            "dmg_max" => numf!(cx, &p, v, c.dmg_max, 0, 100000),
            "armor" => numf!(cx, &p, v, c.armor, -100, 100),
            "range" => numf!(cx, &p, v, c.range, 0, 5000),
            "bat" => numf!(cx, &p, v, c.bat, 0.1, 20),
            "ms" => numf!(cx, &p, v, c.ms, 0, 2000),
            "proj_speed" => numf!(cx, &p, v, c.proj_speed, 0, 10000),
            "radius" => numf!(cx, &p, v, c.radius, 1, 500),
            "vision" => numf!(cx, &p, v, c.vision, 0, 5000),
            "gold" => numf!(cx, &p, v, c.gold, 0, 1000000),
            "xp" => numf!(cx, &p, v, c.xp, 0, 1000000),
            "bldg_mult" => numf!(cx, &p, v, c.bldg_mult, 0, 100),
            _ => cx.err(&p, "unknown key".to_string()),
        }
    }
    if is_new {
        require(cx, path, &seen, &["hp", "dmg_min", "dmg_max"]);
    }
    if c.dmg_min > c.dmg_max {
        cx.err(path, "dmg_min must not exceed dmg_max".to_string());
    }
}

fn patch_tower(cx: &mut Cx, path: &str, obj: &[(String, Value)], t: &mut TowerDef) {
    for (k, v) in obj {
        if k == "comment" {
            continue;
        }
        let p = format!("{}.{}", path, k);
        match k.as_str() {
            "hp" => numf!(cx, &p, v, t.hp, 1, 10000000),
            "dmg_min" => numf!(cx, &p, v, t.dmg_min, 0, 100000),
            "dmg_max" => numf!(cx, &p, v, t.dmg_max, 0, 100000),
            "armor" => numf!(cx, &p, v, t.armor, -100, 100),
            "range" => numf!(cx, &p, v, t.range, 0, 5000),
            "bat" => numf!(cx, &p, v, t.bat, 0.1, 20),
            "gold" => numf!(cx, &p, v, t.gold, 0, 1000000),
            "lane_frac" => numf!(cx, &p, v, t.lane_frac, 0, 1),
            _ => cx.err(&p, "unknown key".to_string()),
        }
    }
    if t.dmg_min > t.dmg_max {
        cx.err(path, "dmg_min must not exceed dmg_max".to_string());
    }
}

fn patch_ancient(cx: &mut Cx, path: &str, obj: &[(String, Value)], a: &mut AncientDef) {
    for (k, v) in obj {
        if k == "comment" {
            continue;
        }
        let p = format!("{}.{}", path, k);
        match k.as_str() {
            "hp" => numf!(cx, &p, v, a.hp, 1, 10000000),
            "armor" => numf!(cx, &p, v, a.armor, -100, 100),
            "radius" => numf!(cx, &p, v, a.radius, 1, 500),
            "vision" => numf!(cx, &p, v, a.vision, 0, 5000),
            _ => cx.err(&p, "unknown key".to_string()),
        }
    }
}

const TOP_KEYS: [&str; 15] = ["format", "name", "version", "author", "description", "replace", "rules", "xp_table", "items", "creeps", "neutrals", "camps", "towers", "ancient", "heroes"];

/// Apply one mod (JSON text) on top of `base`. On any error nothing is applied and the
/// returned list holds up to 20 messages, each starting with the path of the offending value.
pub fn apply_mod(base: &Content, bytes: &[u8]) -> Result<Content, Vec<String>> {
    if bytes.len() > MAX_MOD_BYTES {
        return Err(vec!["mod file is larger than 1 MiB".to_string()]);
    }
    let text = match std::str::from_utf8(bytes) {
        Ok(t) => t,
        Err(_) => return Err(vec!["mod file is not valid UTF-8 text".to_string()]),
    };
    let root = match json::parse(text) {
        Ok(v) => v,
        Err(e) => return Err(vec![format!("not valid JSON: {}", e)]),
    };
    let top = match &root {
        Value::Obj(o) => o,
        other => return Err(vec![format!("$: expected an object, found {}", other.type_name())]),
    };
    let mut cx = Cx { errs: Vec::new() };
    let mut c = base.clone();

    for (k, _) in top {
        if k != "comment" && !TOP_KEYS.contains(&k.as_str()) {
            cx.err(&format!("$.{}", k), "unknown key".to_string());
        }
    }
    if let Some(v) = find_key(top, "format") {
        if let Some(n) = get_num(&mut cx, "$.format", v, 0.0, 1000000.0) {
            if n != 1.0 {
                cx.err("$.format", "unsupported mod format (this engine reads format 1)".to_string());
            }
        }
    }
    let meta = |cx: &mut Cx, key: &str, max: usize| -> String {
        match find_key(top, key) {
            Some(v) => get_str(cx, &format!("$.{}", key), v, max, true).unwrap_or_default(),
            None => String::new(),
        }
    };
    let info = ModInfo { name: meta(&mut cx, "name", 60), version: meta(&mut cx, "version", 20), author: meta(&mut cx, "author", 60), description: meta(&mut cx, "description", 300) };

    // ---- replace
    let (mut rep_heroes, mut rep_items, mut rep_neutrals) = (false, false, false);
    if let Some(v) = find_key(top, "replace") {
        if let Some(arr) = get_arr(&mut cx, "$.replace", v) {
            for (i, x) in arr.iter().enumerate() {
                let p = format!("$.replace[{}]", i);
                if let Some(s) = get_str(&mut cx, &p, x, 40, true) {
                    match s.as_str() {
                        "heroes" => rep_heroes = true,
                        "items" => rep_items = true,
                        "neutrals" => rep_neutrals = true,
                        _ => cx.err(&p, format!("cannot replace \"{}\" (expected one of: heroes, items, neutrals)", s)),
                    }
                }
            }
        }
    }
    if rep_items && !rep_heroes {
        cx.err("$.replace", "replacing \"items\" also requires replacing \"heroes\" (their bot builds refer to the old items)".to_string());
    }
    if rep_neutrals && find_key(top, "camps").is_none() {
        cx.err("$.replace", "replacing \"neutrals\" also requires a \"camps\" list".to_string());
    }
    if rep_heroes {
        c.heroes.clear();
        c.abilities.clear();
    }
    if rep_items {
        c.items.truncate(1);
    }
    if rep_neutrals {
        c.neutrals.clear();
        c.camp_comp.clear();
    }

    // ---- rules, xp table
    if let Some(v) = find_key(top, "rules") {
        if let Some(o) = get_obj(&mut cx, "$.rules", v) {
            c.rules.patch(&mut cx, "$.rules", o);
        }
    }
    if let Some(v) = find_key(top, "xp_table") {
        if let Some(arr) = get_arr(&mut cx, "$.xp_table", v) {
            if !(2..=60).contains(&arr.len()) {
                cx.err("$.xp_table", "must have between 2 and 60 entries".to_string());
            } else {
                let mut t: Vec<f32> = Vec::new();
                let mut ok = true;
                for (i, x) in arr.iter().enumerate() {
                    match get_num(&mut cx, &format!("$.xp_table[{}]", i), x, 0.0, 10000000.0) {
                        Some(n) => t.push(n as f32),
                        None => ok = false,
                    }
                }
                if ok {
                    if t[0] != 0.0 {
                        cx.err("$.xp_table[0]", "level 1 must need 0 XP".to_string());
                    } else if t.windows(2).any(|w| w[1] < w[0]) {
                        cx.err("$.xp_table", "values must not decrease".to_string());
                    } else {
                        c.xp_table = t;
                    }
                }
            }
        }
    }

    // ---- items
    if let Some(v) = find_key(top, "items") {
        if let Some(arr) = get_arr(&mut cx, "$.items", v) {
            let mut names: Vec<String> = Vec::new();
            for (i, iv) in arr.iter().enumerate() {
                let p = format!("$.items[{}]", i);
                let Some(o) = get_obj(&mut cx, &p, iv) else { continue };
                let Some(name) = entry_name(&mut cx, &p, o) else { continue };
                if let Some(j) = names.iter().position(|n| *n == name) {
                    cx.err(&p, format!("duplicate name \"{}\" (also at $.items[{}])", name, j));
                    continue;
                }
                names.push(name.clone());
                match c.items.iter().position(|it| it.name == name && !it.name.is_empty()) {
                    Some(idx) => {
                        let mut it = c.items[idx];
                        patch_item(&mut cx, &p, o, &mut it, false);
                        c.items[idx] = it;
                    }
                    None => {
                        let mut it = c.items[0];
                        it.name = intern(&name);
                        patch_item(&mut cx, &p, o, &mut it, true);
                        c.items.push(it);
                    }
                }
            }
        }
    }

    // ---- neutrals, creeps, camps
    if let Some(v) = find_key(top, "neutrals") {
        if let Some(arr) = get_arr(&mut cx, "$.neutrals", v) {
            let mut names: Vec<String> = Vec::new();
            for (i, nv) in arr.iter().enumerate() {
                let p = format!("$.neutrals[{}]", i);
                let Some(o) = get_obj(&mut cx, &p, nv) else { continue };
                let Some(name) = entry_name(&mut cx, &p, o) else { continue };
                if let Some(j) = names.iter().position(|n| *n == name) {
                    cx.err(&p, format!("duplicate name \"{}\" (also at $.neutrals[{}])", name, j));
                    continue;
                }
                names.push(name.clone());
                match c.neutrals.iter().position(|n| n.name == name) {
                    Some(idx) => {
                        let mut n = c.neutrals[idx];
                        patch_creep(&mut cx, &p, o, &mut n, false, false);
                        c.neutrals[idx] = n;
                    }
                    None => {
                        let mut n = CreepDef { name: intern(&name), kind: Kind::Neutral, hp: 0.0, dmg_min: 0.0, dmg_max: 0.0, armor: 0.0, range: 70.0, bat: 1.0, ms: 100.0, proj_speed: 0.0, radius: 12.0, vision: 260.0, gold: 0.0, xp: 0.0, bldg_mult: 1.0 };
                        patch_creep(&mut cx, &p, o, &mut n, true, false);
                        c.neutrals.push(n);
                    }
                }
            }
        }
    }
    if let Some(v) = find_key(top, "creeps") {
        if let Some(o) = get_obj(&mut cx, "$.creeps", v) {
            for (k, cv) in o {
                if k == "comment" {
                    continue;
                }
                let p = format!("$.creeps.{}", k);
                let slot = match k.as_str() {
                    "melee" => &mut c.melee,
                    "ranged" => &mut c.ranged,
                    "siege" => &mut c.siege,
                    _ => {
                        cx.err(&p, "unknown key (expected melee, ranged or siege)".to_string());
                        continue;
                    }
                };
                if let Some(co) = get_obj(&mut cx, &p, cv) {
                    let mut tmp = *slot;
                    patch_creep(&mut cx, &p, co, &mut tmp, false, true);
                    *slot = tmp;
                }
            }
        }
    }
    if let Some(v) = find_key(top, "camps") {
        if let Some(arr) = get_arr(&mut cx, "$.camps", v) {
            if arr.len() != 3 {
                cx.err("$.camps", "expected exactly 3 camp tiers (arrays of neutral names)".to_string());
            } else {
                let mut comps: Vec<Vec<u8>> = Vec::new();
                let mut ok = true;
                for (t, tv) in arr.iter().enumerate() {
                    let tp = format!("$.camps[{}]", t);
                    let mut comp: Vec<u8> = Vec::new();
                    match get_arr(&mut cx, &tp, tv) {
                        Some(names) if !names.is_empty() && names.len() <= 8 => {
                            for (i, nv) in names.iter().enumerate() {
                                let np = format!("{}[{}]", tp, i);
                                if let Some(n) = get_str(&mut cx, &np, nv, 40, false) {
                                    match c.neutrals.iter().position(|x| x.name == n) {
                                        Some(idx) => comp.push(idx as u8),
                                        None => {
                                            cx.err(&np, format!("unknown neutral \"{}\"", n));
                                            ok = false;
                                        }
                                    }
                                } else {
                                    ok = false;
                                }
                            }
                        }
                        Some(_) => {
                            cx.err(&tp, "a camp needs between 1 and 8 neutrals".to_string());
                            ok = false;
                        }
                        None => ok = false,
                    }
                    comps.push(comp);
                }
                if ok {
                    c.camp_comp = comps;
                }
            }
        }
    }

    // ---- towers, ancient
    if let Some(v) = find_key(top, "towers") {
        if let Some(arr) = get_arr(&mut cx, "$.towers", v) {
            if arr.len() > 3 {
                cx.err("$.towers", "expected at most 3 tiers".to_string());
            } else {
                for (i, tv) in arr.iter().enumerate() {
                    if matches!(tv, Value::Null) {
                        continue;
                    }
                    let p = format!("$.towers[{}]", i);
                    if let Some(o) = get_obj(&mut cx, &p, tv) {
                        let mut t = c.towers[i];
                        patch_tower(&mut cx, &p, o, &mut t);
                        c.towers[i] = t;
                    }
                }
            }
        }
    }
    if let Some(v) = find_key(top, "ancient") {
        if let Some(o) = get_obj(&mut cx, "$.ancient", v) {
            let mut a = c.ancient;
            patch_ancient(&mut cx, "$.ancient", o, &mut a);
            c.ancient = a;
        }
    }

    // ---- heroes (after items so `build` can name them)
    if let Some(v) = find_key(top, "heroes") {
        if let Some(arr) = get_arr(&mut cx, "$.heroes", v) {
            let mut names: Vec<String> = Vec::new();
            for (i, hv) in arr.iter().enumerate() {
                let p = format!("$.heroes[{}]", i);
                let Some(o) = get_obj(&mut cx, &p, hv) else { continue };
                let Some(name) = entry_name(&mut cx, &p, o) else { continue };
                if let Some(j) = names.iter().position(|n| *n == name) {
                    cx.err(&p, format!("duplicate name \"{}\" (also at $.heroes[{}])", name, j));
                    continue;
                }
                names.push(name.clone());
                let items = c.items.clone();
                match c.heroes.iter().position(|h| h.name == name) {
                    Some(idx) => {
                        let mut h = c.heroes[idx];
                        let mut ab = [c.abilities[idx * 4], c.abilities[idx * 4 + 1], c.abilities[idx * 4 + 2], c.abilities[idx * 4 + 3]];
                        patch_hero(&mut cx, &p, o, &items, &mut h, &mut ab, false);
                        c.heroes[idx] = h;
                        for s in 0..4 {
                            c.abilities[idx * 4 + s] = ab[s];
                        }
                    }
                    None => {
                        let mut h = HeroDef {
                            name: intern(&name),
                            title: "",
                            primary: 0,
                            str_: 20.0,
                            agi: 20.0,
                            int: 20.0,
                            gain_str: 2.0,
                            gain_agi: 2.0,
                            gain_int: 2.0,
                            dmg_min: 30.0,
                            dmg_max: 36.0,
                            range: 100.0,
                            bat: 1.7,
                            ms: 300.0,
                            proj_speed: 0.0,
                            radius: 16.0,
                            build: &[],
                            skill_order: [0, 1, 2],
                            glyph: "",
                            color: "",
                        };
                        let mut ab = [AB_NONE; 4];
                        patch_hero(&mut cx, &p, o, &items, &mut h, &mut ab, true);
                        c.heroes.push(h);
                        c.abilities.extend_from_slice(&ab);
                    }
                }
            }
        }
    }

    // ---- whole-content checks (only meaningful when everything above was fine)
    if cx.errs.is_empty() {
        if c.heroes.is_empty() {
            cx.err("$.heroes", "the game needs at least one hero".to_string());
        }
        if c.heroes.len() > MAX_HEROES {
            cx.err("$.heroes", format!("too many heroes (at most {})", MAX_HEROES));
        }
        if c.items.len() > MAX_ITEMS {
            cx.err("$.items", format!("too many items (at most {})", MAX_ITEMS - 1));
        }
        if c.neutrals.is_empty() || c.neutrals.len() > MAX_NEUTRALS {
            cx.err("$.neutrals", format!("the game needs between 1 and {} neutral types", MAX_NEUTRALS));
        }
        if c.camp_comp.len() != 3 {
            cx.err("$.camps", "expected exactly 3 camp tiers (arrays of neutral names)".to_string());
        }
    }

    if !cx.errs.is_empty() {
        return Err(cx.errs);
    }
    c.mods.push(if info.name.is_empty() { ModInfo { name: "Unnamed mod".to_string(), ..info } } else { info });
    Ok(c)
}
