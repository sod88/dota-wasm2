//! Entity model: one flat `Unit` struct covers heroes, creeps, neutrals and buildings.
//! Hero-only state lives in `HeroData` (linked through `Unit::hero`).

use crate::content::Content;
use crate::data::*;
use crate::math::Vec2;

#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Order {
    Idle,
    Move(Vec2),
    AttackMove(Vec2),
    Attack(u32),
    /// `slot` 0..=3 are hero abilities, 10..=15 are inventory items.
    Cast {
        slot: u8,
        target: u32,
        point: Vec2,
    },
    /// Attack only what is already in range; never move.
    Hold,
    /// Stand still and do nothing (used by bots waiting for a last hit).
    Stay,
}

impl Order {
    /// Compact id for the snapshot (UI/debug).
    pub fn code(&self) -> f32 {
        match self {
            Order::Idle => 0.0,
            Order::Move(_) => 1.0,
            Order::AttackMove(_) => 2.0,
            Order::Attack(_) => 3.0,
            Order::Cast { .. } => 4.0,
            Order::Hold => 5.0,
            Order::Stay => 6.0,
        }
    }
}

#[derive(Clone, Debug)]
pub struct Unit {
    pub id: u32,
    pub kind: Kind,
    /// Hero def index / neutral def index / tower tier.
    pub def: u8,
    pub team: u8,
    pub alive: bool,
    pub pos: Vec2,
    pub facing: f32,
    pub radius: f32,

    pub hp: f32,
    pub mana: f32,
    pub max_hp: f32,
    pub max_mana: f32,
    pub armor: f32,
    pub dmg_min: f32,
    pub dmg_max: f32,
    pub range: f32,
    pub bat: f32,
    /// Attack-speed bonus in percent (agility + items), before temporary buffs.
    pub aspd: f32,
    pub move_speed: f32,
    pub hp_regen: f32,
    pub mana_regen: f32,
    pub vision: f32,
    pub proj_speed: f32,
    pub bldg_mult: f32,
    pub bounty_gold: f32,
    pub bounty_xp: f32,

    // --- orders / movement
    pub order: Order,
    pub target: u32,
    pub path: Vec<Vec2>,
    pub path_i: usize,
    pub path_goal: Vec2,
    pub repath_t: f32,

    // --- attack state machine
    pub swing_t: f32,
    pub swing_len: f32,
    pub swing_target: u32,
    pub attack_cd: f32,

    // --- status effects
    pub stun_t: f32,
    pub root_t: f32,
    pub slow_t: f32,
    pub slow_pct: f32,
    pub haste_t: f32,
    pub haste_pct: f32,
    pub aspd_t: f32,
    pub aspd_pct: f32,
    pub salve_t: f32,
    pub salve_rate: f32,
    pub mana_pot_t: f32,
    pub mana_pot_rate: f32,

    /// Seconds since this unit last took damage.
    pub since_dmg: f32,

    // --- lane creeps
    pub lane: u8,
    pub wp: usize,
    // --- neutrals
    pub home: Vec2,
    pub camp: i32,
    pub returning: bool,
    // --- towers: forced target after an enemy hero hits one of our heroes
    pub force_target: u32,
    pub force_t: f32,

    pub hero: i32,
}

impl Unit {
    pub fn blank(id: u32, kind: Kind, team: u8, pos: Vec2) -> Unit {
        Unit {
            id,
            kind,
            def: 0,
            team,
            alive: true,
            pos,
            facing: 0.0,
            radius: 12.0,
            hp: 1.0,
            mana: 0.0,
            max_hp: 1.0,
            max_mana: 0.0,
            armor: 0.0,
            dmg_min: 0.0,
            dmg_max: 0.0,
            range: 70.0,
            bat: 1.0,
            aspd: 0.0,
            move_speed: 100.0,
            hp_regen: 0.0,
            mana_regen: 0.0,
            vision: 300.0,
            proj_speed: 0.0,
            bldg_mult: 1.0,
            bounty_gold: 0.0,
            bounty_xp: 0.0,
            order: Order::Idle,
            target: 0,
            path: Vec::new(),
            path_i: 0,
            path_goal: Vec2::ZERO,
            repath_t: 0.0,
            swing_t: 0.0,
            swing_len: 0.0,
            swing_target: 0,
            attack_cd: 0.0,
            stun_t: 0.0,
            root_t: 0.0,
            slow_t: 0.0,
            slow_pct: 0.0,
            haste_t: 0.0,
            haste_pct: 0.0,
            aspd_t: 0.0,
            aspd_pct: 0.0,
            salve_t: 0.0,
            salve_rate: 0.0,
            mana_pot_t: 0.0,
            mana_pot_rate: 0.0,
            since_dmg: 99.0,
            lane: 1,
            wp: 0,
            home: pos,
            camp: -1,
            returning: false,
            force_target: 0,
            force_t: 0.0,
            hero: -1,
        }
    }

    #[inline]
    pub fn is_hero(&self) -> bool {
        self.kind == Kind::Hero
    }
    #[inline]
    pub fn is_building(&self) -> bool {
        self.kind.is_building()
    }
    #[inline]
    pub fn stunned(&self) -> bool {
        self.stun_t > 0.0
    }
    #[inline]
    pub fn rooted(&self) -> bool {
        self.root_t > 0.0
    }

    /// Effective movement speed including slows and haste.
    pub fn speed(&self) -> f32 {
        if self.stunned() || self.rooted() {
            return 0.0;
        }
        let mut ms = self.move_speed;
        if self.haste_t > 0.0 {
            ms *= 1.0 + self.haste_pct / 100.0;
        }
        if self.slow_t > 0.0 {
            ms *= (1.0 - self.slow_pct / 100.0).max(0.2);
        }
        ms
    }

    /// Seconds between attacks (attack-speed adjusted).
    pub fn attack_time(&self) -> f32 {
        let mut a = self.aspd;
        if self.aspd_t > 0.0 {
            a += self.aspd_pct;
        }
        let a = a.clamp(-50.0, 400.0);
        self.bat / (1.0 + a / 100.0)
    }

    pub fn avg_damage(&self) -> f32 {
        (self.dmg_min + self.dmg_max) * 0.5
    }

    pub fn clear_path(&mut self) {
        self.path.clear();
        self.path_i = 0;
    }
}

#[derive(Clone, Copy, Debug, Default)]
pub struct ItemSlot {
    pub id: u8,
    pub charges: u8,
    pub cd: f32,
}

#[derive(Clone, Debug)]
pub struct HeroData {
    pub unit: u32,
    pub def: u8,
    pub team: u8,
    pub lane: u8,
    pub bot: bool,
    pub level: u8,
    pub xp: f32,
    pub skill_pts: u8,
    pub ab_level: [u8; 4],
    pub ab_cd: [f32; 4],
    pub base_str: f32,
    pub base_agi: f32,
    pub base_int: f32,
    pub gold: f32,
    pub networth: f32,
    pub items: [ItemSlot; INV_SLOTS],
    pub kills: u32,
    pub deaths: u32,
    pub assists: u32,
    pub last_hits: u32,
    pub denies: u32,
    pub respawn_t: f32,
    /// Enemy heroes that recently damaged us: (unit id, seconds ago) — for assists.
    pub recent: Vec<(u32, f32)>,
    // --- bot brain
    pub think_t: f32,
    pub ai_state: u8,
    pub build_i: usize,
    pub shop_cd: f32,
    pub lane_front: Vec2,
}

impl HeroData {
    pub fn new(unit: u32, def: u8, team: u8, lane: u8, bot: bool, c: &Content) -> HeroData {
        let d = &c.heroes[def as usize];
        HeroData {
            unit,
            def,
            team,
            lane,
            bot,
            level: 1,
            xp: 0.0,
            skill_pts: 1,
            ab_level: [0; 4],
            ab_cd: [0.0; 4],
            base_str: d.str_,
            base_agi: d.agi,
            base_int: d.int,
            gold: c.rules.start_gold,
            networth: c.rules.start_gold,
            items: [ItemSlot::default(); INV_SLOTS],
            kills: 0,
            deaths: 0,
            assists: 0,
            last_hits: 0,
            denies: 0,
            respawn_t: 0.0,
            recent: Vec::new(),
            think_t: 0.0,
            ai_state: 0,
            build_i: 0,
            shop_cd: 0.0,
            lane_front: Vec2::ZERO,
        }
    }

    /// Sum of passive bonuses from all carried items.
    pub fn item_bonus(&self, c: &Content) -> ItemDef {
        let mut b = c.items[0];
        for s in &self.items {
            if s.id == 0 {
                continue;
            }
            let it = &c.items[s.id as usize];
            b.str_ += it.str_;
            b.agi += it.agi;
            b.int += it.int;
            b.dmg += it.dmg;
            b.armor += it.armor;
            b.hp += it.hp;
            b.mana += it.mana;
            b.aspd += it.aspd;
            b.ms += it.ms;
            b.hpreg += it.hpreg;
            b.mpreg += it.mpreg;
        }
        b
    }

    pub fn free_slot(&self) -> Option<usize> {
        self.items.iter().position(|s| s.id == 0)
    }
}

#[derive(Clone, Debug)]
pub struct Proj {
    pub id: u32,
    pub src: u32,
    pub team: u8,
    pub pos: Vec2,
    pub dir: Vec2,
    pub speed: f32,
    /// Target unit id for homing projectiles; 0 for linear ones.
    pub homing: u32,
    pub dmg: f32,
    pub dtype: DmgType,
    pub fx: u8,
    pub dur: f32,
    pub pct: f32,
    /// hero*4+slot, or 255 for a basic attack.
    pub ability: u8,
    pub radius: f32,
    pub remaining: f32,
    pub hits: Vec<u32>,
}

#[derive(Clone, Debug)]
pub struct Zone {
    pub id: u32,
    pub pos: Vec2,
    pub radius: f32,
    pub t_left: f32,
    pub t_total: f32,
    pub team: u8,
    pub src: u32,
    pub dmg: f32,
    pub dtype: DmgType,
    pub fx: u8,
    pub dur: f32,
    pub pct: f32,
    pub ability: u8,
}

#[derive(Clone, Debug)]
pub struct Camp {
    pub pos: Vec2,
    pub tier: u8,
    pub members: Vec<u32>,
    pub respawn_t: f32,
    pub waiting: bool,
}

#[derive(Clone, Copy, Debug)]
pub enum Cmd {
    Move { unit: u32, to: Vec2 },
    AttackMove { unit: u32, to: Vec2 },
    Attack { unit: u32, target: u32 },
    Stop { unit: u32 },
    Hold { unit: u32 },
    Cast { unit: u32, slot: u8, target: u32, point: Vec2 },
    LevelUp { unit: u32, slot: u8 },
    Buy { unit: u32, item: u8 },
    Sell { unit: u32, slot: u8 },
}
