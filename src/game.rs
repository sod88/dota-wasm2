//! The simulation core: world state, fixed-timestep pipeline, spawning, vision, hero stats.
//!
//! One `Game::step()` advances the world by `1 / TICK_HZ` seconds. Given the same
//! `Config` and the same stream of commands, the simulation is fully deterministic
//! (seeded PRNG, no hash maps, no wall-clock), so it is replay- and lockstep-friendly.

use crate::content::{self, Content};
use crate::data::*;
use crate::map::*;
use crate::math::*;
use crate::path::PathFinder;
use crate::unit::*;

// ---- event type codes (see `Game::events`: [type, a, b, x, y, v, w, t]) ----
pub const EV_DAMAGE: f32 = 1.0; // a=victim b=attacker v=amount w=dmg type
pub const EV_KILL: f32 = 2.0; // a=victim b=killer v=gold w=victim kind t=victim team
pub const EV_HEAL: f32 = 3.0; // a=unit v=amount
pub const EV_LEVEL: f32 = 4.0; // a=hero unit v=new level
pub const EV_BUILDING: f32 = 5.0; // a=building id b=kind t=team
pub const EV_CAST: f32 = 6.0; // a=caster b=slot (0-3 ability, 10+ item) x,y=point v=radius w=ability id (hero*4+slot) or 100+item
pub const EV_GOLD: f32 = 7.0; // a=hero unit v=amount
pub const EV_GAMEOVER: f32 = 8.0; // a=winning team
pub const EV_WAVE: f32 = 9.0; // a=wave number
pub const EV_RESPAWN: f32 = 10.0; // a=hero unit
pub const EV_ATTACK: f32 = 11.0; // a=attacker b=target (swing started / projectile launched)
pub const EV_ITEM: f32 = 12.0; // a=hero b=item v=1 bought, 2 sold
pub const EV_LEARN: f32 = 13.0; // a=hero b=slot v=new ability level
pub const EV_BLINK: f32 = 14.0; // a=unit x,y=from v,w=to
pub const EV_ASSIST: f32 = 15.0;

pub const LANE_ORDER: [u8; 5] = [1, 0, 2, 0, 2]; // mid, top, bot, top, bot

#[derive(Clone, Copy, Debug)]
pub struct Config {
    pub seed: u32,
    pub player_hero: u8,
    pub heroes_per_team: u8,
    pub fog: bool,
    /// If true the "player" hero is also driven by the bot AI (watch mode / tests).
    pub player_is_bot: bool,
}

impl Default for Config {
    fn default() -> Self {
        Config { seed: 1, player_hero: 0, heroes_per_team: 1, fog: true, player_is_bot: false }
    }
}

pub struct Game {
    pub cfg: Config,
    /// The rule set and content tables this match runs on (built-in or modded).
    pub content: &'static Content,
    pub time: f32,
    pub tick: u32,
    pub map: Map,
    pub finder: PathFinder,
    pub rng: Rng,
    pub units: Vec<Unit>,
    pub heroes: Vec<HeroData>,
    pub projs: Vec<Proj>,
    pub zones: Vec<Zone>,
    pub camps: Vec<Camp>,
    pub next_id: u32,
    pub events: Vec<[f32; 8]>,
    pub cmds: Vec<Cmd>,
    pub wave_t: f32,
    pub wave_no: u32,
    /// -1 while the game is running, otherwise the winning team.
    pub winner: i32,
    /// Unit id of the human-controlled hero.
    pub player: u32,
    pub player_team: u8,
    pub fog: bool,
    pub vis: [Vec<bool>; 2],
    pub lane_paths: [[Vec<Vec2>; 3]; 2],
    pub ancient_ids: [u32; 2],
    /// tw_alive[team][lane][tier]
    pub tw_alive: [[[bool; 3]; 3]; 2],
    gold_t: f32,
}

pub fn base_of(team: u8) -> Vec2 {
    if team == TEAM_RADIANT {
        BASE_RADIANT
    } else {
        BASE_DIRE
    }
}

impl Game {
    /// A match on the built-in content.
    pub fn new(cfg: Config) -> Game {
        Game::with_content(cfg, content::builtin())
    }

    /// A match on custom (modded) content.
    pub fn with_content(cfg: Config, content: &'static Content) -> Game {
        let map = Map::generate();
        let mut g = Game {
            cfg,
            content,
            time: 0.0,
            tick: 0,
            finder: PathFinder::new(),
            rng: Rng::new(cfg.seed as u64),
            units: Vec::new(),
            heroes: Vec::new(),
            projs: Vec::new(),
            zones: Vec::new(),
            camps: Vec::new(),
            next_id: 1,
            events: Vec::new(),
            cmds: Vec::new(),
            wave_t: content.rules.first_wave_at,
            wave_no: 0,
            winner: -1,
            player: 0,
            player_team: TEAM_RADIANT,
            fog: cfg.fog,
            vis: [Vec::new(), Vec::new()],
            lane_paths: Default::default(),
            ancient_ids: [0; 2],
            tw_alive: [[[true; 3]; 3]; 2],
            gold_t: 0.0,
            map,
        };
        g.build_lane_paths();
        g.build_structures();
        g.build_camps();
        g.build_heroes();
        g.recompute_tower_flags();
        g
    }

    // ------------------------------------------------------------------ lookup helpers

    pub fn alloc_id(&mut self) -> u32 {
        let id = self.next_id;
        self.next_id += 1;
        id
    }

    /// Index of a unit by id (units stay sorted by id, so this is a binary search).
    #[inline]
    pub fn idx(&self, id: u32) -> Option<usize> {
        if id == 0 {
            return None;
        }
        self.units.binary_search_by_key(&id, |u| u.id).ok()
    }

    /// Index of a unit that exists and is alive.
    #[inline]
    pub fn live_idx(&self, id: u32) -> Option<usize> {
        self.idx(id).filter(|&i| self.units[i].alive)
    }

    #[inline]
    pub fn emit(&mut self, ev: [f32; 8]) {
        self.events.push(ev);
    }

    pub fn player_idx(&self) -> Option<usize> {
        self.idx(self.player)
    }

    #[inline]
    pub fn visible_to(&self, team: u8, ui: usize) -> bool {
        // Neutrals see everything; with fog disabled (sandbox mode) the player's team does too.
        if team > 1 || (!self.fog && team == self.player_team) {
            return true;
        }
        self.vis[team as usize].get(ui).copied().unwrap_or(false)
    }

    // ------------------------------------------------------------------ world setup

    fn build_lane_paths(&mut self) {
        for lane in Lane::ALL {
            let len = self.map.lane_length(lane);
            let n = (len / 140.0).ceil() as usize;
            let mut pts: Vec<Vec2> = (1..=n).map(|k| self.map.lane_point(lane, k as f32 / n as f32)).collect();
            self.lane_paths[0][lane.idx()] = pts.clone();
            // Dire walks the same lane backwards, starting from Radiant's side.
            pts.reverse();
            let mut rev: Vec<Vec2> = pts.into_iter().skip(1).collect();
            rev.push(BASE_RADIANT);
            self.lane_paths[1][lane.idx()] = rev;
        }
    }

    fn build_structures(&mut self) {
        for team in 0..2u8 {
            // Ancient
            let id = self.alloc_id();
            let mut a = Unit::blank(id, Kind::Ancient, team, base_of(team));
            a.radius = self.content.ancient.radius;
            a.max_hp = self.content.ancient.hp;
            a.hp = self.content.ancient.hp;
            a.armor = self.content.ancient.armor;
            a.vision = self.content.ancient.vision;
            a.range = 0.0;
            self.ancient_ids[team as usize] = id;
            self.units.push(a);
            // Towers: 3 tiers in each of the 3 lanes.
            for lane in Lane::ALL {
                for tier in 0..3u8 {
                    let td = &self.content.towers[tier as usize];
                    let frac = if team == 0 { td.lane_frac } else { 1.0 - td.lane_frac };
                    let pos = self.map.lane_point(lane, frac);
                    let id = self.alloc_id();
                    let mut t = Unit::blank(id, Kind::Tower, team, pos);
                    t.def = tier;
                    t.lane = lane.idx() as u8;
                    t.radius = TOWER_RADIUS;
                    t.max_hp = td.hp;
                    t.hp = td.hp;
                    t.armor = td.armor;
                    t.dmg_min = td.dmg_min;
                    t.dmg_max = td.dmg_max;
                    t.range = td.range;
                    t.bat = td.bat;
                    t.proj_speed = TOWER_PROJ_SPEED;
                    t.vision = TOWER_VISION;
                    t.bounty_gold = td.gold;
                    t.order = Order::Hold;
                    self.units.push(t);
                }
            }
        }
    }

    fn build_camps(&mut self) {
        for c in self.map.camps.clone() {
            self.camps.push(Camp { pos: c.pos, tier: c.tier, members: Vec::new(), respawn_t: 0.5, waiting: true });
        }
    }

    fn build_heroes(&mut self) {
        let per = self.cfg.heroes_per_team.clamp(1, 5) as usize;
        for team in 0..2u8 {
            for k in 0..per {
                let def = if team == 0 {
                    ((self.cfg.player_hero as usize + k) % self.content.heroes.len()) as u8
                } else {
                    ((self.cfg.player_hero as usize + 1 + k) % self.content.heroes.len()) as u8
                };
                let lane = LANE_ORDER[k];
                let is_player = team == 0 && k == 0;
                let bot = !is_player || self.cfg.player_is_bot;
                let uid = self.spawn_hero(def, team, lane, k, bot);
                if is_player {
                    self.player = uid;
                    self.player_team = team;
                }
            }
        }
    }

    fn hero_spawn_pos(&self, team: u8, k: usize) -> Vec2 {
        let base = base_of(team);
        let toward = (Vec2::new(WORLD * 0.5, WORLD * 0.5) - base).norm();
        let side = Vec2::new(-toward.y, toward.x);
        base + toward * 90.0 + side * ((k as f32 - 2.0) * 44.0)
    }

    pub fn spawn_hero(&mut self, def: u8, team: u8, lane: u8, k: usize, bot: bool) -> u32 {
        let id = self.alloc_id();
        let pos = self.hero_spawn_pos(team, k);
        let mut u = Unit::blank(id, Kind::Hero, team, pos);
        u.def = def;
        u.lane = lane;
        let hd = &self.content.heroes[def as usize];
        u.radius = hd.radius;
        u.range = hd.range;
        u.bat = hd.bat;
        u.proj_speed = hd.proj_speed;
        u.vision = self.content.rules.hero_vision;
        u.hero = self.heroes.len() as i32;
        u.facing = if team == 0 { -std::f32::consts::FRAC_PI_4 } else { std::f32::consts::PI * 0.75 };
        self.units.push(u);
        self.heroes.push(HeroData::new(id, def, team, lane, bot, self.content));
        let ui = self.units.len() - 1;
        self.recalc_hero(ui, false);
        let max_hp = self.units[ui].max_hp;
        let max_mana = self.units[ui].max_mana;
        self.units[ui].hp = max_hp;
        self.units[ui].mana = max_mana;
        id
    }

    // ------------------------------------------------------------------ hero stats

    /// Recompute a hero's derived stats from level, attributes and items.
    /// `keep_ratio == false` is used on spawn; otherwise the gained max HP/mana is added to the pools.
    pub fn recalc_hero(&mut self, ui: usize, _grow: bool) {
        let hi = self.units[ui].hero;
        if hi < 0 {
            return;
        }
        let hi = hi as usize;
        let (def, level) = (self.heroes[hi].def, self.heroes[hi].level as f32);
        let d = self.content.heroes[def as usize];
        let r = &self.content.rules;
        self.heroes[hi].base_str = d.str_ + d.gain_str * (level - 1.0);
        self.heroes[hi].base_agi = d.agi + d.gain_agi * (level - 1.0);
        self.heroes[hi].base_int = d.int + d.gain_int * (level - 1.0);
        let b = self.heroes[hi].item_bonus(self.content);
        let s = self.heroes[hi].base_str + b.str_;
        let a = self.heroes[hi].base_agi + b.agi;
        let n = self.heroes[hi].base_int + b.int;
        let primary = match d.primary {
            0 => s,
            1 => a,
            _ => n,
        };
        let u = &mut self.units[ui];
        let (old_hp, old_mana) = (u.max_hp, u.max_mana);
        u.max_hp = r.hp_base + r.hp_per_str * s + b.hp;
        u.max_mana = r.mana_base + r.mana_per_int * n + b.mana;
        u.armor = r.armor_base + r.armor_per_agi * a + b.armor;
        u.aspd = a + b.aspd;
        u.move_speed = d.ms + b.ms;
        u.dmg_min = d.dmg_min + primary + b.dmg;
        u.dmg_max = d.dmg_max + primary + b.dmg;
        u.hp_regen = r.hp_regen_base + r.hp_regen_per_str * s + b.hpreg;
        u.mana_regen = r.mana_regen_base + r.mana_regen_per_int * n + b.mpreg;
        if old_hp > 1.0 {
            u.hp = (u.hp + (u.max_hp - old_hp)).clamp(1.0, u.max_hp);
            u.mana = (u.mana + (u.max_mana - old_mana)).clamp(0.0, u.max_mana);
        }
        u.hp = u.hp.min(u.max_hp);
        u.mana = u.mana.min(u.max_mana);
    }

    pub fn add_xp(&mut self, ui: usize, xp: f32) {
        let hi = self.units[ui].hero;
        if hi < 0 || xp <= 0.0 {
            return;
        }
        let hi = hi as usize;
        let mut leveled = false;
        let xp_table = &self.content.xp_table;
        {
            let h = &mut self.heroes[hi];
            h.xp += xp;
            while (h.level as usize) < xp_table.len() && h.xp >= xp_table[h.level as usize] {
                h.level += 1;
                h.skill_pts += 1;
                leveled = true;
            }
        }
        if leveled {
            self.recalc_hero(ui, true);
            let (id, lvl, pos) = (self.units[ui].id, self.heroes[hi].level, self.units[ui].pos);
            self.emit([EV_LEVEL, id as f32, 0.0, pos.x, pos.y, lvl as f32, 0.0, 0.0]);
        }
    }

    pub fn add_gold(&mut self, ui: usize, amount: f32) {
        let hi = self.units[ui].hero;
        if hi < 0 || amount <= 0.0 {
            return;
        }
        let h = &mut self.heroes[hi as usize];
        h.gold += amount;
        h.networth += amount;
    }

    pub fn learn_ability(&mut self, ui: usize, slot: usize) -> bool {
        let hi = self.units[ui].hero;
        if hi < 0 || slot > 3 {
            return false;
        }
        let hi = hi as usize;
        let def = &self.content.abilities[self.heroes[hi].def as usize * 4 + slot];
        let h = &self.heroes[hi];
        let cur = h.ab_level[slot];
        if h.skill_pts == 0 || cur >= def.max_level || h.level < def.req_level[cur as usize] {
            return false;
        }
        let h = &mut self.heroes[hi];
        h.ab_level[slot] += 1;
        h.skill_pts -= 1;
        let (id, lv) = (h.unit, h.ab_level[slot]);
        self.emit([EV_LEARN, id as f32, slot as f32, 0.0, 0.0, lv as f32, 0.0, 0.0]);
        true
    }

    pub fn near_shop(&self, ui: usize) -> bool {
        let u = &self.units[ui];
        u.alive && u.pos.dist(base_of(u.team)) <= self.content.rules.shop_radius
    }

    pub fn buy_item(&mut self, ui: usize, item: u8) -> bool {
        let hi = self.units[ui].hero;
        if hi < 0 || item == 0 || item as usize >= self.content.items.len() || !self.near_shop(ui) {
            return false;
        }
        let hi = hi as usize;
        let it = self.content.items[item as usize];
        if self.heroes[hi].gold < it.cost {
            return false;
        }
        let h = &mut self.heroes[hi];
        // Stack consumables.
        if it.stack {
            if let Some(s) = h.items.iter_mut().find(|s| s.id == item && s.charges < 5) {
                s.charges += 1;
                h.gold -= it.cost;
                let (id, _) = (h.unit, 0);
                self.emit([EV_ITEM, id as f32, item as f32, 0.0, 0.0, 1.0, 0.0, 0.0]);
                return true;
            }
        }
        let slot = match h.free_slot() {
            Some(s) => s,
            None => return false,
        };
        h.items[slot] = ItemSlot { id: item, charges: if it.stack { 1 } else { 0 }, cd: 0.0 };
        h.gold -= it.cost;
        let id = h.unit;
        self.recalc_hero(ui, true);
        self.emit([EV_ITEM, id as f32, item as f32, 0.0, 0.0, 1.0, 0.0, 0.0]);
        true
    }

    pub fn sell_item(&mut self, ui: usize, slot: usize) -> bool {
        let hi = self.units[ui].hero;
        if hi < 0 || slot >= INV_SLOTS || !self.near_shop(ui) {
            return false;
        }
        let hi = hi as usize;
        let s = self.heroes[hi].items[slot];
        if s.id == 0 {
            return false;
        }
        let it = self.content.items[s.id as usize];
        let refund = it.cost * 0.5 * (s.charges.max(1) as f32);
        let h = &mut self.heroes[hi];
        h.items[slot] = ItemSlot::default();
        h.gold += refund;
        let id = h.unit;
        self.recalc_hero(ui, true);
        self.emit([EV_ITEM, id as f32, s.id as f32, 0.0, 0.0, 2.0, 0.0, 0.0]);
        true
    }

    // ------------------------------------------------------------------ spawning

    pub fn spawn_creep(&mut self, def: &CreepDef, team: u8, lane: u8, pos: Vec2) -> u32 {
        let id = self.alloc_id();
        let mut u = Unit::blank(id, def.kind, team, pos);
        let m = (self.time / 60.0).min(40.0);
        let hp_scale = 1.0 + 0.015 * m;
        let dmg_scale = 1.0 + 0.012 * m;
        u.max_hp = def.hp * hp_scale;
        u.hp = u.max_hp;
        u.armor = def.armor + 0.03 * m;
        u.dmg_min = def.dmg_min * dmg_scale;
        u.dmg_max = def.dmg_max * dmg_scale;
        u.range = def.range;
        u.bat = def.bat;
        u.move_speed = def.ms;
        u.proj_speed = def.proj_speed;
        u.radius = def.radius;
        u.vision = def.vision;
        u.bounty_gold = def.gold + 0.25 * m;
        u.bounty_xp = def.xp;
        u.bldg_mult = def.bldg_mult;
        u.lane = lane;
        u.wp = 0;
        let path = &self.lane_paths[team as usize][lane as usize];
        if let Some(&p) = path.first() {
            u.order = Order::AttackMove(p);
            u.facing = (p - pos).y.atan2((p - pos).x);
        }
        self.units.push(u);
        id
    }

    fn spawn_wave(&mut self) {
        self.wave_no += 1;
        let wave = self.wave_no;
        for team in 0..2u8 {
            for lane in Lane::ALL {
                let spawn_frac = if team == 0 { 0.10 } else { 0.90 };
                let p0 = self.map.lane_point(lane, spawn_frac);
                let p1 = self.map.lane_point(lane, if team == 0 { 0.13 } else { 0.87 });
                let dir = (p1 - p0).norm();
                let side = Vec2::new(-dir.y, dir.x);
                let ct = self.content;
                let mut comp: Vec<CreepDef> = Vec::new();
                comp.extend((0..ct.rules.wave_melee).map(|_| ct.melee));
                comp.extend((0..ct.rules.wave_ranged).map(|_| ct.ranged));
                if ct.rules.siege_every > 0 && wave % ct.rules.siege_every == 0 {
                    comp.extend((0..ct.rules.wave_siege).map(|_| ct.siege));
                }
                for (k, def) in comp.iter().enumerate() {
                    let jitter = (self.rng.f32() - 0.5) * 20.0;
                    let pos = p0 - dir * (k as f32 * 26.0) + side * jitter;
                    let pos = self.map.nearest_walkable(pos);
                    self.spawn_creep(def, team, lane.idx() as u8, pos);
                }
            }
        }
        self.emit([EV_WAVE, wave as f32, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0]);
    }

    fn spawn_camp(&mut self, ci: usize) {
        let (pos, tier) = (self.camps[ci].pos, self.camps[ci].tier as usize);
        let comp = &self.content.camp_comp[tier];
        let mut ids = Vec::new();
        for (k, &nd) in comp.iter().enumerate() {
            let def = self.content.neutrals[nd as usize];
            let ang = k as f32 * 2.1 + 0.5;
            let p = self.map.nearest_walkable(pos + Vec2::new(ang.cos(), ang.sin()) * 26.0);
            let id = self.alloc_id();
            let mut u = Unit::blank(id, Kind::Neutral, TEAM_NEUTRAL, p);
            let m = (self.time / 60.0).min(40.0);
            u.def = nd;
            u.max_hp = def.hp * (1.0 + 0.01 * m);
            u.hp = u.max_hp;
            u.armor = def.armor;
            u.dmg_min = def.dmg_min * (1.0 + 0.01 * m);
            u.dmg_max = def.dmg_max * (1.0 + 0.01 * m);
            u.range = def.range;
            u.bat = def.bat;
            u.move_speed = def.ms;
            u.proj_speed = def.proj_speed;
            u.radius = def.radius;
            u.vision = def.vision;
            u.bounty_gold = def.gold * (1.0 + 0.01 * m);
            u.bounty_xp = def.xp;
            u.home = p;
            u.camp = ci as i32;
            u.order = Order::Stay;
            self.units.push(u);
            ids.push(id);
        }
        self.camps[ci].members = ids;
        self.camps[ci].waiting = false;
    }

    fn spawn_logic(&mut self) {
        // Passive income (one pulse per second keeps gold integer-friendly for the UI).
        self.gold_t += DT;
        if self.gold_t >= 1.0 {
            self.gold_t -= 1.0;
            for hi in 0..self.heroes.len() {
                let id = self.heroes[hi].unit;
                if let Some(ui) = self.live_idx(id) {
                    self.add_gold(ui, self.content.rules.passive_gold);
                }
            }
        }
        // Lane waves.
        self.wave_t -= DT;
        if self.wave_t <= 0.0 {
            self.wave_t += self.content.rules.wave_interval;
            self.spawn_wave();
        }
        // Jungle camps.
        for ci in 0..self.camps.len() {
            let mut members = std::mem::take(&mut self.camps[ci].members);
            members.retain(|&id| self.live_idx(id).is_some());
            let empty = members.is_empty();
            self.camps[ci].members = members;
            if empty {
                if !self.camps[ci].waiting {
                    self.camps[ci].waiting = true;
                    self.camps[ci].respawn_t = self.content.rules.camp_respawn;
                }
                self.camps[ci].respawn_t -= DT;
                if self.camps[ci].respawn_t <= 0.0 {
                    self.spawn_camp(ci);
                }
            }
        }
        // Hero respawns.
        for hi in 0..self.heroes.len() {
            let id = self.heroes[hi].unit;
            let ui = match self.idx(id) {
                Some(i) => i,
                None => continue,
            };
            if self.units[ui].alive {
                continue;
            }
            self.heroes[hi].respawn_t -= DT;
            if self.heroes[hi].respawn_t <= 0.0 {
                let team = self.units[ui].team;
                let k = hi % (self.cfg.heroes_per_team.clamp(1, 5) as usize);
                let pos = self.hero_spawn_pos(team, k);
                let u = &mut self.units[ui];
                u.alive = true;
                u.pos = pos;
                u.hp = u.max_hp;
                u.mana = u.max_mana;
                u.order = Order::Idle;
                u.target = 0;
                u.swing_t = 0.0;
                u.stun_t = 0.0;
                u.root_t = 0.0;
                u.slow_t = 0.0;
                u.haste_t = 0.0;
                u.aspd_t = 0.0;
                u.salve_t = 0.0;
                u.clear_path();
                self.heroes[hi].ai_state = 0;
                self.emit([EV_RESPAWN, id as f32, 0.0, pos.x, pos.y, 0.0, 0.0, 0.0]);
            }
        }
    }

    // ------------------------------------------------------------------ vision & flags

    pub fn recompute_tower_flags(&mut self) {
        let mut f = [[[false; 3]; 3]; 2];
        for u in &self.units {
            if u.alive && u.kind == Kind::Tower {
                f[u.team as usize][u.lane as usize][u.def as usize] = true;
            }
        }
        self.tw_alive = f;
    }

    /// Buildings are untouchable until the structure protecting them has fallen:
    /// tier N needs tier N-1 of its lane dead; the Ancient needs any tier-3 tower dead.
    pub fn building_protected(&self, ui: usize) -> bool {
        let u = &self.units[ui];
        let t = u.team as usize;
        match u.kind {
            Kind::Tower => u.def > 0 && self.tw_alive[t][u.lane as usize][u.def as usize - 1],
            Kind::Ancient => (0..3).all(|l| self.tw_alive[t][l][2]),
            _ => false,
        }
    }

    pub fn compute_vision(&mut self) {
        let n = self.units.len();
        for team in 0..2usize {
            let v = &mut self.vis[team];
            v.clear();
            v.resize(n, false);
        }
        for a in 0..n {
            let ua = &self.units[a];
            if !ua.alive || ua.team > 1 {
                continue;
            }
            let t = ua.team as usize;
            let r2 = ua.vision * ua.vision;
            let pa = ua.pos;
            for b in 0..n {
                if self.vis[t][b] {
                    continue;
                }
                let ub = &self.units[b];
                if ub.alive && pa.dist_sq(ub.pos) <= r2 {
                    self.vis[t][b] = true;
                }
            }
        }
    }

    // ------------------------------------------------------------------ main loop

    pub fn step(&mut self) {
        if self.winner >= 0 {
            return;
        }
        self.apply_commands();
        self.time += DT;
        self.tick += 1;
        self.recompute_tower_flags();
        self.compute_vision();
        self.spawn_logic();
        let n = self.units.len();
        for i in 0..n {
            if self.units[i].alive {
                self.update_timers(i);
            }
        }
        for i in 0..n {
            if !self.units[i].alive {
                continue;
            }
            self.think(i);
            if self.units[i].alive {
                self.act(i);
            }
        }
        self.update_projectiles();
        self.update_zones();
        self.separate_units();
        self.cleanup();
    }

    /// Advance by `n` ticks.
    pub fn run(&mut self, n: u32) {
        for _ in 0..n {
            self.step();
        }
    }

    fn cleanup(&mut self) {
        self.units.retain(|u| u.alive || u.kind == Kind::Hero);
    }

    // ------------------------------------------------------------------ commands

    pub fn push_cmd(&mut self, c: Cmd) {
        self.cmds.push(c);
    }

    fn controllable(&self, id: u32) -> Option<usize> {
        self.live_idx(id).filter(|&i| self.units[i].is_hero())
    }

    fn clamp_world(&self, p: Vec2) -> Vec2 {
        Vec2::new(p.x.clamp(1.0, WORLD - 1.0), p.y.clamp(1.0, WORLD - 1.0))
    }

    fn apply_commands(&mut self) {
        let cmds = std::mem::take(&mut self.cmds);
        for c in cmds {
            match c {
                Cmd::Move { unit, to } => {
                    if let Some(i) = self.controllable(unit) {
                        let to = self.clamp_world(to);
                        self.set_order(i, Order::Move(to));
                    }
                }
                Cmd::AttackMove { unit, to } => {
                    if let Some(i) = self.controllable(unit) {
                        let to = self.clamp_world(to);
                        self.set_order(i, Order::AttackMove(to));
                    }
                }
                Cmd::Attack { unit, target } => {
                    if let Some(i) = self.controllable(unit) {
                        if let Some(t) = self.live_idx(target) {
                            if self.can_attack(i, t) {
                                self.set_order(i, Order::Attack(target));
                            }
                        }
                    }
                }
                Cmd::Stop { unit } => {
                    if let Some(i) = self.controllable(unit) {
                        self.set_order(i, Order::Idle);
                    }
                }
                Cmd::Hold { unit } => {
                    if let Some(i) = self.controllable(unit) {
                        self.set_order(i, Order::Hold);
                    }
                }
                Cmd::Cast { unit, slot, target, point } => {
                    if let Some(i) = self.controllable(unit) {
                        self.order_cast(i, slot, target, point);
                    }
                }
                Cmd::LevelUp { unit, slot } => {
                    if let Some(i) = self.controllable(unit) {
                        self.learn_ability(i, slot as usize);
                    }
                }
                Cmd::Buy { unit, item } => {
                    if let Some(i) = self.controllable(unit) {
                        self.buy_item(i, item);
                    }
                }
                Cmd::Sell { unit, slot } => {
                    if let Some(i) = self.controllable(unit) {
                        self.sell_item(i, slot as usize);
                    }
                }
            }
        }
    }

    pub fn set_order(&mut self, i: usize, o: Order) {
        let u = &mut self.units[i];
        u.order = o;
        u.target = 0;
        u.swing_t = 0.0; // issuing a new order cancels a wind-up (animation cancel)
        u.clear_path();
    }

    // ------------------------------------------------------------------ introspection

    /// FNV-1a hash of the simulation state, used to detect desyncs and to test determinism.
    pub fn state_hash(&self) -> u64 {
        let mut h: u64 = 0xcbf29ce484222325;
        let mut mix = |v: u64| {
            h ^= v;
            h = h.wrapping_mul(0x100000001b3);
        };
        mix(self.tick as u64);
        for u in &self.units {
            mix(u.id as u64);
            mix(u.pos.x.to_bits() as u64);
            mix(u.pos.y.to_bits() as u64);
            mix(u.hp.to_bits() as u64);
            mix(u.alive as u64);
        }
        for hd in &self.heroes {
            mix(hd.gold.to_bits() as u64);
            mix(hd.xp.to_bits() as u64);
            mix(hd.kills as u64);
        }
        h
    }

    pub fn count_kind(&self, team: u8, kind: Kind) -> usize {
        self.units.iter().filter(|u| u.alive && u.team == team && u.kind == kind).count()
    }
}
