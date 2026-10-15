//! Static game data: heroes, abilities, items, creeps, neutrals and buildings.
//!
//! Everything balance-related lives in this file as plain tables, so adding a hero or
//! an item is a data change, not an engine change. The same tables are exported to
//! JavaScript (see `ffi.rs`) so the UI never duplicates numbers.

pub const TICK_HZ: f32 = 30.0;
pub const DT: f32 = 1.0 / TICK_HZ;

pub const TEAM_RADIANT: u8 = 0;
pub const TEAM_DIRE: u8 = 1;
pub const TEAM_NEUTRAL: u8 = 2;

pub const MAX_LEVEL: usize = 25;
pub const INV_SLOTS: usize = 6;
pub const START_GOLD: f32 = 625.0;
pub const PASSIVE_GOLD_PER_SEC: f32 = 1.6;
pub const WAVE_INTERVAL: f32 = 30.0;
pub const FIRST_WAVE_AT: f32 = 8.0;
pub const CAMP_RESPAWN: f32 = 60.0;
pub const XP_RADIUS: f32 = 700.0;
pub const SHOP_RADIUS: f32 = 260.0;
pub const MAGIC_RESIST: f32 = 0.25;

/// Cumulative XP needed to *reach* level `i+1` (index 0 = level 1).
pub const XP_TABLE: [f32; MAX_LEVEL] = [
    0.0, 240.0, 640.0, 1160.0, 1760.0, 2440.0, 3200.0, 4000.0, 4900.0, 5900.0, 7000.0, 8200.0,
    9500.0, 10900.0, 12400.0, 14000.0, 15700.0, 17500.0, 19400.0, 21400.0, 23600.0, 25600.0,
    27600.0, 29600.0, 31600.0,
];

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
#[repr(u8)]
pub enum Kind {
    Hero = 0,
    Melee = 1,
    Ranged = 2,
    Siege = 3,
    Neutral = 4,
    Tower = 5,
    Ancient = 6,
}

impl Kind {
    pub fn is_building(self) -> bool {
        matches!(self, Kind::Tower | Kind::Ancient)
    }
    pub fn is_lane_creep(self) -> bool {
        matches!(self, Kind::Melee | Kind::Ranged | Kind::Siege)
    }
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
#[repr(u8)]
pub enum DmgType {
    Physical = 0,
    Magical = 1,
    Pure = 2,
}

/// How an ability is aimed / delivered.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
#[repr(u8)]
pub enum Cast {
    None = 0,
    /// Enemy unit target (instant, or a homing projectile if `speed > 0`).
    Target = 1,
    /// Linear projectile fired towards a point; pierces everything in its path.
    Line = 2,
    /// Area at a point (optionally with a telegraphed delay).
    PointAoE = 3,
    /// Area centred on the caster.
    SelfAoE = 4,
    /// Buff on the caster.
    SelfBuff = 5,
    /// Teleport towards a point; optionally detonates an AoE on arrival.
    Blink = 6,
}

pub const FX_STUN: u8 = 1;
pub const FX_SLOW: u8 = 2;
pub const FX_ROOT: u8 = 4;
pub const FX_HASTE: u8 = 8; // caster move-speed buff (pct2)
pub const FX_ASPD: u8 = 16; // caster attack-speed buff (pct)

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct AbilityDef {
    pub name: &'static str,
    pub desc: &'static str,
    pub cast: Cast,
    pub dtype: DmgType,
    pub fx: u8,
    pub max_level: u8,
    /// Hero level required to learn level 1/2/3/4 of this ability.
    pub req_level: [u8; 4],
    pub range: [f32; 4],
    pub radius: [f32; 4],
    pub mana: [f32; 4],
    pub cd: [f32; 4],
    pub dmg: [f32; 4],
    /// Duration (seconds) of every status this ability applies (stun / slow / root / buff).
    pub dur: [f32; 4],
    /// Slow % (FX_SLOW) or attack-speed % (FX_ASPD).
    pub pct: [f32; 4],
    /// Move-speed % buff (FX_HASTE).
    pub pct2: [f32; 4],
    /// Projectile speed (0 = instant).
    pub speed: f32,
    /// Telegraph delay for PointAoE.
    pub delay: f32,
    /// Blink detonates an AoE at the landing spot.
    pub aoe_on_land: bool,
    /// Optional front-end hints (icon glyph name, `#rrggbb` colour); empty = derive from the ability.
    pub icon: &'static str,
    pub color: &'static str,
}

/// A blank ability (also the template for abilities added by mods).
pub const AB_NONE: AbilityDef = AbilityDef {
    name: "",
    desc: "",
    cast: Cast::None,
    dtype: DmgType::Magical,
    fx: 0,
    max_level: 4,
    req_level: [1, 3, 5, 7],
    range: [0.0; 4],
    radius: [0.0; 4],
    mana: [0.0; 4],
    cd: [0.0; 4],
    dmg: [0.0; 4],
    dur: [0.0; 4],
    pct: [0.0; 4],
    pct2: [0.0; 4],
    speed: 0.0,
    delay: 0.0,
    aoe_on_land: false,
    icon: "",
    color: "",
};

const ULT_REQ: [u8; 4] = [6, 12, 18, 99];

/// 3 heroes x 4 abilities. Index = hero * 4 + slot (slot 3 is the ultimate).
pub const ABILITIES: [AbilityDef; 12] = [
    // ---------------------------------------------------------------- Warden
    AbilityDef {
        name: "Hammer Strike",
        desc: "Hurls a hammer at an enemy: magic damage and a stun.",
        cast: Cast::Target,
        fx: FX_STUN,
        range: [280.0; 4],
        mana: [60.0, 70.0, 80.0, 90.0],
        cd: [11.0, 10.0, 9.0, 8.0],
        dmg: [80.0, 140.0, 200.0, 260.0],
        dur: [1.2, 1.4, 1.6, 1.8],
        speed: 600.0,
        ..AB_NONE
    },
    AbilityDef {
        name: "Shockwave",
        desc: "Slams the ground around you: magic damage and a slow.",
        cast: Cast::SelfAoE,
        fx: FX_SLOW,
        radius: [170.0, 180.0, 190.0, 200.0],
        mana: [70.0, 80.0, 90.0, 100.0],
        cd: [14.0, 13.0, 12.0, 11.0],
        dmg: [60.0, 100.0, 140.0, 180.0],
        dur: [2.5; 4],
        pct: [35.0, 40.0, 45.0, 50.0],
        req_level: [1, 3, 5, 7],
        ..AB_NONE
    },
    AbilityDef {
        name: "Battle Cry",
        desc: "Roars: bonus attack speed and movement speed for a few seconds.",
        cast: Cast::SelfBuff,
        fx: FX_ASPD | FX_HASTE,
        mana: [50.0, 55.0, 60.0, 65.0],
        cd: [20.0, 19.0, 18.0, 17.0],
        dur: [6.0; 4],
        pct: [40.0, 60.0, 80.0, 100.0],
        pct2: [10.0, 12.0, 14.0, 16.0],
        ..AB_NONE
    },
    AbilityDef {
        name: "Titan Slam",
        desc: "Leaps forward and crashes down: magic damage and a stun in an area.",
        cast: Cast::Blink,
        fx: FX_STUN,
        max_level: 3,
        req_level: ULT_REQ,
        range: [380.0, 420.0, 460.0, 460.0],
        radius: [200.0, 220.0, 240.0, 240.0],
        mana: [150.0, 200.0, 250.0, 250.0],
        cd: [60.0, 50.0, 40.0, 40.0],
        dmg: [180.0, 300.0, 420.0, 420.0],
        dur: [1.0, 1.5, 2.0, 2.0],
        aoe_on_land: true,
        ..AB_NONE
    },
    // ---------------------------------------------------------------- Ranger
    AbilityDef {
        name: "Piercing Shot",
        desc: "Fires a bolt that pierces every enemy in a line.",
        cast: Cast::Line,
        range: [560.0; 4],
        radius: [28.0; 4],
        mana: [50.0, 55.0, 60.0, 65.0],
        cd: [8.0, 7.5, 7.0, 6.5],
        dmg: [70.0, 120.0, 170.0, 220.0],
        speed: 520.0,
        ..AB_NONE
    },
    AbilityDef {
        name: "Entangling Net",
        desc: "Roots an enemy in place and deals a little magic damage.",
        cast: Cast::Target,
        fx: FX_ROOT,
        range: [340.0; 4],
        mana: [70.0, 80.0, 90.0, 100.0],
        cd: [15.0, 14.0, 13.0, 12.0],
        dmg: [40.0, 60.0, 80.0, 100.0],
        dur: [1.4, 1.8, 2.2, 2.6],
        speed: 700.0,
        ..AB_NONE
    },
    AbilityDef {
        name: "Sprint",
        desc: "Greatly increases your movement speed for a short time.",
        cast: Cast::SelfBuff,
        fx: FX_HASTE,
        mana: [40.0; 4],
        cd: [22.0, 20.0, 18.0, 16.0],
        dur: [3.0, 4.0, 5.0, 6.0],
        pct2: [30.0, 40.0, 50.0, 60.0],
        ..AB_NONE
    },
    AbilityDef {
        name: "Arrow Storm",
        desc: "Calls down a volley on an area after a short delay.",
        cast: Cast::PointAoE,
        max_level: 3,
        req_level: ULT_REQ,
        range: [480.0; 4],
        radius: [210.0; 4],
        mana: [140.0, 190.0, 240.0, 240.0],
        cd: [45.0, 40.0, 35.0, 35.0],
        dmg: [260.0, 400.0, 540.0, 540.0],
        delay: 0.9,
        ..AB_NONE
    },
    // ------------------------------------------------------------- Archmage
    AbilityDef {
        name: "Searing Bolt",
        desc: "Launches a bolt of fire at an enemy.",
        cast: Cast::Target,
        range: [400.0; 4],
        mana: [70.0, 80.0, 90.0, 100.0],
        cd: [7.0, 6.5, 6.0, 5.5],
        dmg: [90.0, 150.0, 210.0, 270.0],
        speed: 650.0,
        ..AB_NONE
    },
    AbilityDef {
        name: "Frost Nova",
        desc: "Bursts at a point: magic damage and a heavy slow.",
        cast: Cast::PointAoE,
        fx: FX_SLOW,
        range: [380.0; 4],
        radius: [170.0; 4],
        mana: [90.0, 100.0, 110.0, 120.0],
        cd: [12.0, 11.0, 10.0, 9.0],
        dmg: [60.0, 100.0, 140.0, 180.0],
        dur: [3.0; 4],
        pct: [40.0, 45.0, 50.0, 55.0],
        ..AB_NONE
    },
    AbilityDef {
        name: "Blink",
        desc: "Teleports a short distance in the chosen direction.",
        cast: Cast::Blink,
        range: [320.0, 400.0, 480.0, 560.0],
        mana: [40.0; 4],
        cd: [14.0, 12.0, 10.0, 8.0],
        ..AB_NONE
    },
    AbilityDef {
        name: "Meteor",
        desc: "Summons a meteor that lands after a delay: huge damage and a short stun.",
        cast: Cast::PointAoE,
        fx: FX_STUN,
        max_level: 3,
        req_level: ULT_REQ,
        range: [620.0; 4],
        radius: [230.0; 4],
        mana: [180.0, 240.0, 300.0, 300.0],
        cd: [70.0, 60.0, 50.0, 50.0],
        dmg: [320.0, 520.0, 720.0, 720.0],
        dur: [0.5, 0.8, 1.0, 1.0],
        delay: 1.3,
        ..AB_NONE
    },
];

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct HeroDef {
    pub name: &'static str,
    pub title: &'static str,
    /// 0 = strength, 1 = agility, 2 = intelligence (adds 1 damage per point).
    pub primary: u8,
    pub str_: f32,
    pub agi: f32,
    pub int: f32,
    pub gain_str: f32,
    pub gain_agi: f32,
    pub gain_int: f32,
    pub dmg_min: f32,
    pub dmg_max: f32,
    pub range: f32,
    pub bat: f32,
    pub ms: f32,
    pub proj_speed: f32,
    pub radius: f32,
    /// Item ids a bot buys, in order.
    pub build: &'static [u8],
    /// Bot skill priority among the three basic abilities (ult is always taken first).
    pub skill_order: [u8; 3],
    /// Optional front-end hints: emblem glyph name and `#rrggbb` accent colour.
    pub glyph: &'static str,
    pub color: &'static str,
}

pub const HEROES: [HeroDef; 3] = [
    HeroDef {
        name: "Warden",
        title: "Strength - melee bruiser",
        primary: 0,
        str_: 24.0,
        agi: 12.0,
        int: 14.0,
        gain_str: 3.2,
        gain_agi: 1.4,
        gain_int: 1.2,
        dmg_min: 28.0,
        dmg_max: 34.0,
        range: 150.0,
        bat: 1.7,
        ms: 170.0,
        proj_speed: 0.0,
        radius: 17.0,
        build: &[2, 7, 6, 9, 18, 14, 19],
        skill_order: [0, 1, 2],
        glyph: "shield",
        color: "",
    },
    HeroDef {
        name: "Ranger",
        title: "Agility - ranged carry",
        primary: 1,
        str_: 17.0,
        agi: 26.0,
        int: 15.0,
        gain_str: 1.8,
        gain_agi: 3.0,
        gain_int: 1.4,
        dmg_min: 30.0,
        dmg_max: 36.0,
        range: 300.0,
        bat: 1.7,
        ms: 165.0,
        proj_speed: 700.0,
        radius: 15.0,
        build: &[12, 7, 3, 5, 13, 11, 20],
        skill_order: [0, 2, 1],
        glyph: "bow",
        color: "",
    },
    HeroDef {
        name: "Archmage",
        title: "Intelligence - ranged caster",
        primary: 2,
        str_: 16.0,
        agi: 14.0,
        int: 25.0,
        gain_str: 1.8,
        gain_agi: 1.5,
        gain_int: 3.2,
        dmg_min: 26.0,
        dmg_max: 32.0,
        range: 300.0,
        bat: 1.7,
        ms: 160.0,
        proj_speed: 650.0,
        radius: 15.0,
        build: &[4, 17, 7, 10, 15, 18, 9],
        skill_order: [0, 1, 2],
        glyph: "star",
        color: "",
    },
];

// ---------------------------------------------------------------------------- items

pub const ACT_NONE: u8 = 0;
pub const ACT_SALVE: u8 = 1;
pub const ACT_MANA: u8 = 2;
pub const ACT_BLINK: u8 = 3;

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct ItemDef {
    pub name: &'static str,
    pub cost: f32,
    pub str_: f32,
    pub agi: f32,
    pub int: f32,
    pub dmg: f32,
    pub armor: f32,
    pub hp: f32,
    pub mana: f32,
    pub aspd: f32,
    pub ms: f32,
    pub hpreg: f32,
    pub mpreg: f32,
    pub active: u8,
    pub cd: f32,
    /// Consumables stack in one slot (up to 5 charges).
    pub stack: bool,
    /// Optional front-end hint: `#rrggbb` icon colour (empty = derive from the strongest stat).
    pub color: &'static str,
}

pub const IT: ItemDef = ItemDef {
    name: "",
    cost: 0.0,
    str_: 0.0,
    agi: 0.0,
    int: 0.0,
    dmg: 0.0,
    armor: 0.0,
    hp: 0.0,
    mana: 0.0,
    aspd: 0.0,
    ms: 0.0,
    hpreg: 0.0,
    mpreg: 0.0,
    active: ACT_NONE,
    cd: 0.0,
    stack: false,
    color: "",
};

pub const ITEM_COUNT: usize = 21;

/// Index 0 is "no item".
pub const ITEMS: [ItemDef; ITEM_COUNT] = [
    IT,
    ItemDef { name: "Iron Branch", cost: 50.0, str_: 1.0, agi: 1.0, int: 1.0, ..IT },
    ItemDef { name: "Belt of Might", cost: 450.0, str_: 6.0, ..IT },
    ItemDef { name: "Band of Agility", cost: 450.0, agi: 6.0, ..IT },
    ItemDef { name: "Staff of Wisdom", cost: 450.0, int: 6.0, ..IT },
    ItemDef { name: "Blades of Attack", cost: 450.0, dmg: 9.0, ..IT },
    ItemDef { name: "Chainmail", cost: 550.0, armor: 5.0, ..IT },
    ItemDef { name: "Boots of Speed", cost: 500.0, ms: 22.0, ..IT },
    ItemDef { name: "Ring of Regeneration", cost: 350.0, hpreg: 3.0, ..IT },
    ItemDef { name: "Vitality Booster", cost: 1000.0, hp: 250.0, ..IT },
    ItemDef { name: "Energy Booster", cost: 900.0, mana: 250.0, ..IT },
    ItemDef { name: "Broadsword", cost: 1200.0, dmg: 22.0, ..IT },
    ItemDef { name: "Gloves of Haste", cost: 600.0, aspd: 20.0, ..IT },
    ItemDef { name: "Treads of Swiftness", cost: 1400.0, ms: 28.0, aspd: 15.0, agi: 5.0, ..IT },
    ItemDef { name: "Aegis Plate", cost: 1800.0, hp: 300.0, armor: 4.0, hpreg: 4.0, ..IT },
    ItemDef { name: "Arcane Scepter", cost: 2000.0, int: 18.0, mpreg: 3.0, ..IT },
    ItemDef { name: "Healing Salve", cost: 100.0, active: ACT_SALVE, cd: 1.0, stack: true, ..IT },
    ItemDef { name: "Mana Potion", cost: 70.0, active: ACT_MANA, cd: 1.0, stack: true, ..IT },
    ItemDef { name: "Blink Dagger", cost: 2250.0, active: ACT_BLINK, cd: 12.0, ..IT },
    ItemDef { name: "Titan Heart", cost: 3300.0, str_: 25.0, hp: 350.0, hpreg: 8.0, ..IT },
    ItemDef { name: "Executioner's Axe", cost: 3200.0, dmg: 55.0, ..IT },
];

pub const BLINK_DAGGER_RANGE: f32 = 340.0;
pub const SALVE_HEAL: f32 = 300.0;
pub const SALVE_TIME: f32 = 10.0;
pub const MANA_POTION_RESTORE: f32 = 150.0;
pub const MANA_POTION_TIME: f32 = 8.0;

// ------------------------------------------------------------------- creeps & neutrals

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct CreepDef {
    pub name: &'static str,
    pub kind: Kind,
    pub hp: f32,
    pub dmg_min: f32,
    pub dmg_max: f32,
    pub armor: f32,
    pub range: f32,
    pub bat: f32,
    pub ms: f32,
    pub proj_speed: f32,
    pub radius: f32,
    pub vision: f32,
    pub gold: f32,
    pub xp: f32,
    /// Damage multiplier against buildings.
    pub bldg_mult: f32,
}

pub const MELEE_CREEP: CreepDef = CreepDef {
    name: "Footman",
    kind: Kind::Melee,
    hp: 550.0,
    dmg_min: 18.0,
    dmg_max: 22.0,
    armor: 2.0,
    range: 70.0,
    bat: 1.0,
    ms: 110.0,
    proj_speed: 0.0,
    radius: 11.0,
    vision: 300.0,
    gold: 38.0,
    xp: 55.0,
    bldg_mult: 1.0,
};
pub const RANGED_CREEP: CreepDef = CreepDef {
    name: "Archer",
    kind: Kind::Ranged,
    hp: 320.0,
    dmg_min: 20.0,
    dmg_max: 26.0,
    armor: 0.0,
    range: 250.0,
    bat: 1.0,
    ms: 110.0,
    proj_speed: 600.0,
    radius: 10.0,
    vision: 300.0,
    gold: 44.0,
    xp: 40.0,
    bldg_mult: 1.0,
};
pub const SIEGE_CREEP: CreepDef = CreepDef {
    name: "Catapult",
    kind: Kind::Siege,
    hp: 800.0,
    dmg_min: 35.0,
    dmg_max: 45.0,
    armor: 0.0,
    range: 300.0,
    bat: 2.7,
    ms: 100.0,
    proj_speed: 500.0,
    radius: 14.0,
    vision: 300.0,
    gold: 66.0,
    xp: 88.0,
    bldg_mult: 3.0,
};

pub const NEUTRALS: [CreepDef; 4] = [
    CreepDef {
        name: "Scrapper",
        kind: Kind::Neutral,
        hp: 350.0,
        dmg_min: 14.0,
        dmg_max: 18.0,
        armor: 1.0,
        range: 70.0,
        bat: 1.2,
        ms: 100.0,
        proj_speed: 0.0,
        radius: 11.0,
        vision: 260.0,
        gold: 22.0,
        xp: 30.0,
        bldg_mult: 1.0,
    },
    CreepDef {
        name: "Brute",
        kind: Kind::Neutral,
        hp: 600.0,
        dmg_min: 24.0,
        dmg_max: 30.0,
        armor: 2.0,
        range: 80.0,
        bat: 1.4,
        ms: 100.0,
        proj_speed: 0.0,
        radius: 14.0,
        vision: 260.0,
        gold: 30.0,
        xp: 45.0,
        bldg_mult: 1.0,
    },
    CreepDef {
        name: "Spitter",
        kind: Kind::Neutral,
        hp: 400.0,
        dmg_min: 20.0,
        dmg_max: 26.0,
        armor: 0.0,
        range: 230.0,
        bat: 1.3,
        ms: 100.0,
        proj_speed: 550.0,
        radius: 12.0,
        vision: 260.0,
        gold: 28.0,
        xp: 42.0,
        bldg_mult: 1.0,
    },
    CreepDef {
        name: "Ogre",
        kind: Kind::Neutral,
        hp: 1100.0,
        dmg_min: 40.0,
        dmg_max: 50.0,
        armor: 3.0,
        range: 90.0,
        bat: 1.6,
        ms: 95.0,
        proj_speed: 0.0,
        radius: 19.0,
        vision: 260.0,
        gold: 55.0,
        xp: 90.0,
        bldg_mult: 1.0,
    },
];

/// Camp composition per tier (indices into `NEUTRALS`).
pub const CAMP_COMP: [&[u8]; 3] = [&[0, 0], &[1, 1, 2], &[3, 1, 1]];

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct TowerDef {
    pub hp: f32,
    pub dmg_min: f32,
    pub dmg_max: f32,
    pub armor: f32,
    pub range: f32,
    pub bat: f32,
    pub gold: f32,
    /// Position along the lane (0 = own base, 1 = enemy base) from the owner's side.
    pub lane_frac: f32,
}

pub const TOWERS: [TowerDef; 3] = [
    TowerDef { hp: 1800.0, dmg_min: 105.0, dmg_max: 115.0, armor: 8.0, range: 340.0, bat: 1.0, gold: 150.0, lane_frac: 0.34 },
    TowerDef { hp: 2100.0, dmg_min: 120.0, dmg_max: 132.0, armor: 10.0, range: 340.0, bat: 1.0, gold: 200.0, lane_frac: 0.22 },
    TowerDef { hp: 2500.0, dmg_min: 135.0, dmg_max: 150.0, armor: 12.0, range: 340.0, bat: 1.0, gold: 250.0, lane_frac: 0.12 },
];
pub const TOWER_RADIUS: f32 = 34.0;
pub const TOWER_VISION: f32 = 480.0;
pub const TOWER_PROJ_SPEED: f32 = 700.0;

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct AncientDef {
    pub hp: f32,
    pub armor: f32,
    pub radius: f32,
    pub vision: f32,
}

pub const ANCIENT_HP: f32 = 4500.0;
pub const ANCIENT_ARMOR: f32 = 10.0;
pub const ANCIENT_RADIUS: f32 = 56.0;
pub const ANCIENT_VISION: f32 = 450.0;

pub fn armor_multiplier(armor: f32) -> f32 {
    1.0 - (0.06 * armor) / (1.0 + 0.06 * armor.abs())
}
