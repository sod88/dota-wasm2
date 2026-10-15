// The built-in game content: heroes, abilities, items, creeps, neutrals, towers and rules.
//
// GENERATED from the Rust engine (src/data.rs) by tools/gen_cpp_builtin.py, so both engines start from
// exactly the same numbers. Edit src/data.rs and regenerate, or change content with a mod instead.
#include "content.hpp"

namespace moba {

Content make_builtin_content() {
    Content c;

    // ---- rules
    c.rules.start_gold = 625.0f;
    c.rules.passive_gold = 1.6f;
    c.rules.wave_interval = 30.0f;
    c.rules.first_wave_at = 8.0f;
    c.rules.camp_respawn = 60.0f;
    c.rules.xp_radius = 700.0f;
    c.rules.shop_radius = 260.0f;
    c.rules.magic_resist = 0.25f;
    c.rules.blink_range = 340.0f;
    c.rules.salve_heal = 300.0f;
    c.rules.salve_time = 10.0f;
    c.rules.mana_potion_restore = 150.0f;
    c.rules.mana_potion_time = 8.0f;
    c.rules.hero_vision = 420.0f;
    c.rules.hp_base = 200.0f;
    c.rules.hp_per_str = 20.0f;
    c.rules.mana_base = 50.0f;
    c.rules.mana_per_int = 12.0f;
    c.rules.armor_base = 1.0f;
    c.rules.armor_per_agi = 0.15f;
    c.rules.hp_regen_base = 1.0f;
    c.rules.hp_regen_per_str = 0.12f;
    c.rules.mana_regen_base = 0.5f;
    c.rules.mana_regen_per_int = 0.05f;
    c.rules.respawn_base = 5.0f;
    c.rules.respawn_per_level = 2.2f;
    c.rules.respawn_max = 60.0f;
    c.rules.hero_bounty_base = 120.0f;
    c.rules.hero_bounty_per_level = 14.0f;
    c.rules.hero_xp_base = 60.0f;
    c.rules.hero_xp_per_level = 22.0f;
    c.rules.wave_melee = 3u;
    c.rules.wave_ranged = 1u;
    c.rules.wave_siege = 1u;
    c.rules.siege_every = 5u;

    // ---- XP needed to reach each level (its length is the level cap)
    c.xp_table = {0.0f, 240.0f, 640.0f, 1160.0f, 1760.0f, 2440.0f, 3200.0f, 4000.0f, 4900.0f, 5900.0f, 7000.0f, 8200.0f, 9500.0f, 10900.0f, 12400.0f, 14000.0f, 15700.0f, 17500.0f, 19400.0f, 21400.0f, 23600.0f, 25600.0f, 27600.0f, 29600.0f, 31600.0f};

    // ---- items (index 0 is the empty slot)
    c.items.push_back(ItemDef());
    { ItemDef it; it.name = "Iron Branch"; it.cost = 50.0f; it.str_ = 1.0f; it.agi = 1.0f; it.int_ = 1.0f; c.items.push_back(it); }
    { ItemDef it; it.name = "Belt of Might"; it.cost = 450.0f; it.str_ = 6.0f; c.items.push_back(it); }
    { ItemDef it; it.name = "Band of Agility"; it.cost = 450.0f; it.agi = 6.0f; c.items.push_back(it); }
    { ItemDef it; it.name = "Staff of Wisdom"; it.cost = 450.0f; it.int_ = 6.0f; c.items.push_back(it); }
    { ItemDef it; it.name = "Blades of Attack"; it.cost = 450.0f; it.dmg = 9.0f; c.items.push_back(it); }
    { ItemDef it; it.name = "Chainmail"; it.cost = 550.0f; it.armor = 5.0f; c.items.push_back(it); }
    { ItemDef it; it.name = "Boots of Speed"; it.cost = 500.0f; it.ms = 22.0f; c.items.push_back(it); }
    { ItemDef it; it.name = "Ring of Regeneration"; it.cost = 350.0f; it.hpreg = 3.0f; c.items.push_back(it); }
    { ItemDef it; it.name = "Vitality Booster"; it.cost = 1000.0f; it.hp = 250.0f; c.items.push_back(it); }
    { ItemDef it; it.name = "Energy Booster"; it.cost = 900.0f; it.mana = 250.0f; c.items.push_back(it); }
    { ItemDef it; it.name = "Broadsword"; it.cost = 1200.0f; it.dmg = 22.0f; c.items.push_back(it); }
    { ItemDef it; it.name = "Gloves of Haste"; it.cost = 600.0f; it.aspd = 20.0f; c.items.push_back(it); }
    { ItemDef it; it.name = "Treads of Swiftness"; it.cost = 1400.0f; it.agi = 5.0f; it.aspd = 15.0f; it.ms = 28.0f; c.items.push_back(it); }
    { ItemDef it; it.name = "Aegis Plate"; it.cost = 1800.0f; it.armor = 4.0f; it.hp = 300.0f; it.hpreg = 4.0f; c.items.push_back(it); }
    { ItemDef it; it.name = "Arcane Scepter"; it.cost = 2000.0f; it.int_ = 18.0f; it.mpreg = 3.0f; c.items.push_back(it); }
    { ItemDef it; it.name = "Healing Salve"; it.cost = 100.0f; it.cd = 1.0f; it.active = ACT_SALVE; it.stack = true; c.items.push_back(it); }
    { ItemDef it; it.name = "Mana Potion"; it.cost = 70.0f; it.cd = 1.0f; it.active = ACT_MANA; it.stack = true; c.items.push_back(it); }
    { ItemDef it; it.name = "Blink Dagger"; it.cost = 2250.0f; it.cd = 12.0f; it.active = ACT_BLINK; c.items.push_back(it); }
    { ItemDef it; it.name = "Titan Heart"; it.cost = 3300.0f; it.str_ = 25.0f; it.hp = 350.0f; it.hpreg = 8.0f; c.items.push_back(it); }
    { ItemDef it; it.name = "Executioner's Axe"; it.cost = 3200.0f; it.dmg = 55.0f; c.items.push_back(it); }

    // ---- lane creeps
    c.melee.name = "Footman";
    c.melee.kind = Kind::Melee;
    c.melee.hp = 550.0f; c.melee.dmg_min = 18.0f; c.melee.dmg_max = 22.0f; c.melee.armor = 2.0f; c.melee.range = 70.0f; c.melee.bat = 1.0f; c.melee.ms = 110.0f; c.melee.proj_speed = 0.0f; c.melee.radius = 11.0f; c.melee.vision = 300.0f; c.melee.gold = 38.0f; c.melee.xp = 55.0f; c.melee.bldg_mult = 1.0f;
    c.ranged.name = "Archer";
    c.ranged.kind = Kind::Ranged;
    c.ranged.hp = 320.0f; c.ranged.dmg_min = 20.0f; c.ranged.dmg_max = 26.0f; c.ranged.armor = 0.0f; c.ranged.range = 250.0f; c.ranged.bat = 1.0f; c.ranged.ms = 110.0f; c.ranged.proj_speed = 600.0f; c.ranged.radius = 10.0f; c.ranged.vision = 300.0f; c.ranged.gold = 44.0f; c.ranged.xp = 40.0f; c.ranged.bldg_mult = 1.0f;
    c.siege.name = "Catapult";
    c.siege.kind = Kind::Siege;
    c.siege.hp = 800.0f; c.siege.dmg_min = 35.0f; c.siege.dmg_max = 45.0f; c.siege.armor = 0.0f; c.siege.range = 300.0f; c.siege.bat = 2.7f; c.siege.ms = 100.0f; c.siege.proj_speed = 500.0f; c.siege.radius = 14.0f; c.siege.vision = 300.0f; c.siege.gold = 66.0f; c.siege.xp = 88.0f; c.siege.bldg_mult = 3.0f;

    // ---- jungle neutrals and camp compositions (by tier: small, medium, large)
    {
    CreepDef n;
    n.name = "Scrapper";
    n.kind = Kind::Neutral;
    n.hp = 350.0f; n.dmg_min = 14.0f; n.dmg_max = 18.0f; n.armor = 1.0f; n.range = 70.0f; n.bat = 1.2f; n.ms = 100.0f; n.proj_speed = 0.0f; n.radius = 11.0f; n.vision = 260.0f; n.gold = 22.0f; n.xp = 30.0f; n.bldg_mult = 1.0f;
    c.neutrals.push_back(n);
    }
    {
    CreepDef n;
    n.name = "Brute";
    n.kind = Kind::Neutral;
    n.hp = 600.0f; n.dmg_min = 24.0f; n.dmg_max = 30.0f; n.armor = 2.0f; n.range = 80.0f; n.bat = 1.4f; n.ms = 100.0f; n.proj_speed = 0.0f; n.radius = 14.0f; n.vision = 260.0f; n.gold = 30.0f; n.xp = 45.0f; n.bldg_mult = 1.0f;
    c.neutrals.push_back(n);
    }
    {
    CreepDef n;
    n.name = "Spitter";
    n.kind = Kind::Neutral;
    n.hp = 400.0f; n.dmg_min = 20.0f; n.dmg_max = 26.0f; n.armor = 0.0f; n.range = 230.0f; n.bat = 1.3f; n.ms = 100.0f; n.proj_speed = 550.0f; n.radius = 12.0f; n.vision = 260.0f; n.gold = 28.0f; n.xp = 42.0f; n.bldg_mult = 1.0f;
    c.neutrals.push_back(n);
    }
    {
    CreepDef n;
    n.name = "Ogre";
    n.kind = Kind::Neutral;
    n.hp = 1100.0f; n.dmg_min = 40.0f; n.dmg_max = 50.0f; n.armor = 3.0f; n.range = 90.0f; n.bat = 1.6f; n.ms = 95.0f; n.proj_speed = 0.0f; n.radius = 19.0f; n.vision = 260.0f; n.gold = 55.0f; n.xp = 90.0f; n.bldg_mult = 1.0f;
    c.neutrals.push_back(n);
    }
    c.camp_comp = {{0, 0}, {1, 1, 2}, {3, 1, 1}};

    // ---- towers (tiers 1-3) and the Ancient
    c.towers[0] = TowerDef{1800.0f, 105.0f, 115.0f, 8.0f, 340.0f, 1.0f, 150.0f, 0.34f};
    c.towers[1] = TowerDef{2100.0f, 120.0f, 132.0f, 10.0f, 340.0f, 1.0f, 200.0f, 0.22f};
    c.towers[2] = TowerDef{2500.0f, 135.0f, 150.0f, 12.0f, 340.0f, 1.0f, 250.0f, 0.12f};
    c.ancient = AncientDef{4500.0f, 10.0f, 56.0f, 450.0f};

    // ---- heroes and their four abilities each (index = hero * 4 + slot, slot 3 is the ultimate)
    {
        HeroDef h;
        h.name = "Warden";
        h.title = "Strength - melee bruiser";
        h.primary = 0;
        h.str_ = 24.0f; h.agi = 12.0f; h.int_ = 14.0f;
        h.gain_str = 3.2f; h.gain_agi = 1.4f; h.gain_int = 1.2f;
        h.dmg_min = 28.0f; h.dmg_max = 34.0f; h.range = 150.0f; h.bat = 1.7f; h.ms = 170.0f; h.proj_speed = 0.0f; h.radius = 17.0f;
        h.build = {2, 7, 6, 9, 18, 14, 19};  // Belt of Might, Boots of Speed, Chainmail, Vitality Booster, Blink Dagger, Aegis Plate, Titan Heart
        h.skill_order = {{0, 1, 2}};
        h.glyph = "shield";
        c.heroes.push_back(h);
    }
    {
        AbilityDef a;
        a.name = "Hammer Strike";
        a.desc = "Hurls a hammer at an enemy: magic damage and a stun.";
        a.cast = Cast::Target; a.dtype = DmgType::Magical; a.fx = FX_STUN;
        a.max_level = 4; a.req_level = {{1, 3, 5, 7}};
        a.range = {{280.0f, 280.0f, 280.0f, 280.0f}};
        a.radius = {{0.0f, 0.0f, 0.0f, 0.0f}};
        a.mana = {{60.0f, 70.0f, 80.0f, 90.0f}};
        a.cd = {{11.0f, 10.0f, 9.0f, 8.0f}};
        a.dmg = {{80.0f, 140.0f, 200.0f, 260.0f}};
        a.dur = {{1.2f, 1.4f, 1.6f, 1.8f}};
        a.pct = {{0.0f, 0.0f, 0.0f, 0.0f}};
        a.pct2 = {{0.0f, 0.0f, 0.0f, 0.0f}};
        a.speed = 600.0f; a.delay = 0.0f; a.aoe_on_land = false;
        c.abilities.push_back(a);
    }
    {
        AbilityDef a;
        a.name = "Shockwave";
        a.desc = "Slams the ground around you: magic damage and a slow.";
        a.cast = Cast::SelfAoE; a.dtype = DmgType::Magical; a.fx = FX_SLOW;
        a.max_level = 4; a.req_level = {{1, 3, 5, 7}};
        a.range = {{0.0f, 0.0f, 0.0f, 0.0f}};
        a.radius = {{170.0f, 180.0f, 190.0f, 200.0f}};
        a.mana = {{70.0f, 80.0f, 90.0f, 100.0f}};
        a.cd = {{14.0f, 13.0f, 12.0f, 11.0f}};
        a.dmg = {{60.0f, 100.0f, 140.0f, 180.0f}};
        a.dur = {{2.5f, 2.5f, 2.5f, 2.5f}};
        a.pct = {{35.0f, 40.0f, 45.0f, 50.0f}};
        a.pct2 = {{0.0f, 0.0f, 0.0f, 0.0f}};
        a.speed = 0.0f; a.delay = 0.0f; a.aoe_on_land = false;
        c.abilities.push_back(a);
    }
    {
        AbilityDef a;
        a.name = "Battle Cry";
        a.desc = "Roars: bonus attack speed and movement speed for a few seconds.";
        a.cast = Cast::SelfBuff; a.dtype = DmgType::Magical; a.fx = FX_HASTE | FX_ASPD;
        a.max_level = 4; a.req_level = {{1, 3, 5, 7}};
        a.range = {{0.0f, 0.0f, 0.0f, 0.0f}};
        a.radius = {{0.0f, 0.0f, 0.0f, 0.0f}};
        a.mana = {{50.0f, 55.0f, 60.0f, 65.0f}};
        a.cd = {{20.0f, 19.0f, 18.0f, 17.0f}};
        a.dmg = {{0.0f, 0.0f, 0.0f, 0.0f}};
        a.dur = {{6.0f, 6.0f, 6.0f, 6.0f}};
        a.pct = {{40.0f, 60.0f, 80.0f, 100.0f}};
        a.pct2 = {{10.0f, 12.0f, 14.0f, 16.0f}};
        a.speed = 0.0f; a.delay = 0.0f; a.aoe_on_land = false;
        c.abilities.push_back(a);
    }
    {
        AbilityDef a;
        a.name = "Titan Slam";
        a.desc = "Leaps forward and crashes down: magic damage and a stun in an area.";
        a.cast = Cast::Blink; a.dtype = DmgType::Magical; a.fx = FX_STUN;
        a.max_level = 3; a.req_level = {{6, 12, 18, 99}};
        a.range = {{380.0f, 420.0f, 460.0f, 460.0f}};
        a.radius = {{200.0f, 220.0f, 240.0f, 240.0f}};
        a.mana = {{150.0f, 200.0f, 250.0f, 250.0f}};
        a.cd = {{60.0f, 50.0f, 40.0f, 40.0f}};
        a.dmg = {{180.0f, 300.0f, 420.0f, 420.0f}};
        a.dur = {{1.0f, 1.5f, 2.0f, 2.0f}};
        a.pct = {{0.0f, 0.0f, 0.0f, 0.0f}};
        a.pct2 = {{0.0f, 0.0f, 0.0f, 0.0f}};
        a.speed = 0.0f; a.delay = 0.0f; a.aoe_on_land = true;
        c.abilities.push_back(a);
    }
    {
        HeroDef h;
        h.name = "Ranger";
        h.title = "Agility - ranged carry";
        h.primary = 1;
        h.str_ = 17.0f; h.agi = 26.0f; h.int_ = 15.0f;
        h.gain_str = 1.8f; h.gain_agi = 3.0f; h.gain_int = 1.4f;
        h.dmg_min = 30.0f; h.dmg_max = 36.0f; h.range = 300.0f; h.bat = 1.7f; h.ms = 165.0f; h.proj_speed = 700.0f; h.radius = 15.0f;
        h.build = {12, 7, 3, 5, 13, 11, 20};  // Gloves of Haste, Boots of Speed, Band of Agility, Blades of Attack, Treads of Swiftness, Broadsword, Executioner's Axe
        h.skill_order = {{0, 2, 1}};
        h.glyph = "bow";
        c.heroes.push_back(h);
    }
    {
        AbilityDef a;
        a.name = "Piercing Shot";
        a.desc = "Fires a bolt that pierces every enemy in a line.";
        a.cast = Cast::Line; a.dtype = DmgType::Magical; a.fx = 0;
        a.max_level = 4; a.req_level = {{1, 3, 5, 7}};
        a.range = {{560.0f, 560.0f, 560.0f, 560.0f}};
        a.radius = {{28.0f, 28.0f, 28.0f, 28.0f}};
        a.mana = {{50.0f, 55.0f, 60.0f, 65.0f}};
        a.cd = {{8.0f, 7.5f, 7.0f, 6.5f}};
        a.dmg = {{70.0f, 120.0f, 170.0f, 220.0f}};
        a.dur = {{0.0f, 0.0f, 0.0f, 0.0f}};
        a.pct = {{0.0f, 0.0f, 0.0f, 0.0f}};
        a.pct2 = {{0.0f, 0.0f, 0.0f, 0.0f}};
        a.speed = 520.0f; a.delay = 0.0f; a.aoe_on_land = false;
        c.abilities.push_back(a);
    }
    {
        AbilityDef a;
        a.name = "Entangling Net";
        a.desc = "Roots an enemy in place and deals a little magic damage.";
        a.cast = Cast::Target; a.dtype = DmgType::Magical; a.fx = FX_ROOT;
        a.max_level = 4; a.req_level = {{1, 3, 5, 7}};
        a.range = {{340.0f, 340.0f, 340.0f, 340.0f}};
        a.radius = {{0.0f, 0.0f, 0.0f, 0.0f}};
        a.mana = {{70.0f, 80.0f, 90.0f, 100.0f}};
        a.cd = {{15.0f, 14.0f, 13.0f, 12.0f}};
        a.dmg = {{40.0f, 60.0f, 80.0f, 100.0f}};
        a.dur = {{1.4f, 1.8f, 2.2f, 2.6f}};
        a.pct = {{0.0f, 0.0f, 0.0f, 0.0f}};
        a.pct2 = {{0.0f, 0.0f, 0.0f, 0.0f}};
        a.speed = 700.0f; a.delay = 0.0f; a.aoe_on_land = false;
        c.abilities.push_back(a);
    }
    {
        AbilityDef a;
        a.name = "Sprint";
        a.desc = "Greatly increases your movement speed for a short time.";
        a.cast = Cast::SelfBuff; a.dtype = DmgType::Magical; a.fx = FX_HASTE;
        a.max_level = 4; a.req_level = {{1, 3, 5, 7}};
        a.range = {{0.0f, 0.0f, 0.0f, 0.0f}};
        a.radius = {{0.0f, 0.0f, 0.0f, 0.0f}};
        a.mana = {{40.0f, 40.0f, 40.0f, 40.0f}};
        a.cd = {{22.0f, 20.0f, 18.0f, 16.0f}};
        a.dmg = {{0.0f, 0.0f, 0.0f, 0.0f}};
        a.dur = {{3.0f, 4.0f, 5.0f, 6.0f}};
        a.pct = {{0.0f, 0.0f, 0.0f, 0.0f}};
        a.pct2 = {{30.0f, 40.0f, 50.0f, 60.0f}};
        a.speed = 0.0f; a.delay = 0.0f; a.aoe_on_land = false;
        c.abilities.push_back(a);
    }
    {
        AbilityDef a;
        a.name = "Arrow Storm";
        a.desc = "Calls down a volley on an area after a short delay.";
        a.cast = Cast::PointAoE; a.dtype = DmgType::Magical; a.fx = 0;
        a.max_level = 3; a.req_level = {{6, 12, 18, 99}};
        a.range = {{480.0f, 480.0f, 480.0f, 480.0f}};
        a.radius = {{210.0f, 210.0f, 210.0f, 210.0f}};
        a.mana = {{140.0f, 190.0f, 240.0f, 240.0f}};
        a.cd = {{45.0f, 40.0f, 35.0f, 35.0f}};
        a.dmg = {{260.0f, 400.0f, 540.0f, 540.0f}};
        a.dur = {{0.0f, 0.0f, 0.0f, 0.0f}};
        a.pct = {{0.0f, 0.0f, 0.0f, 0.0f}};
        a.pct2 = {{0.0f, 0.0f, 0.0f, 0.0f}};
        a.speed = 0.0f; a.delay = 0.9f; a.aoe_on_land = false;
        c.abilities.push_back(a);
    }
    {
        HeroDef h;
        h.name = "Archmage";
        h.title = "Intelligence - ranged caster";
        h.primary = 2;
        h.str_ = 16.0f; h.agi = 14.0f; h.int_ = 25.0f;
        h.gain_str = 1.8f; h.gain_agi = 1.5f; h.gain_int = 3.2f;
        h.dmg_min = 26.0f; h.dmg_max = 32.0f; h.range = 300.0f; h.bat = 1.7f; h.ms = 160.0f; h.proj_speed = 650.0f; h.radius = 15.0f;
        h.build = {4, 17, 7, 10, 15, 18, 9};  // Staff of Wisdom, Mana Potion, Boots of Speed, Energy Booster, Arcane Scepter, Blink Dagger, Vitality Booster
        h.skill_order = {{0, 1, 2}};
        h.glyph = "star";
        c.heroes.push_back(h);
    }
    {
        AbilityDef a;
        a.name = "Searing Bolt";
        a.desc = "Launches a bolt of fire at an enemy.";
        a.cast = Cast::Target; a.dtype = DmgType::Magical; a.fx = 0;
        a.max_level = 4; a.req_level = {{1, 3, 5, 7}};
        a.range = {{400.0f, 400.0f, 400.0f, 400.0f}};
        a.radius = {{0.0f, 0.0f, 0.0f, 0.0f}};
        a.mana = {{70.0f, 80.0f, 90.0f, 100.0f}};
        a.cd = {{7.0f, 6.5f, 6.0f, 5.5f}};
        a.dmg = {{90.0f, 150.0f, 210.0f, 270.0f}};
        a.dur = {{0.0f, 0.0f, 0.0f, 0.0f}};
        a.pct = {{0.0f, 0.0f, 0.0f, 0.0f}};
        a.pct2 = {{0.0f, 0.0f, 0.0f, 0.0f}};
        a.speed = 650.0f; a.delay = 0.0f; a.aoe_on_land = false;
        c.abilities.push_back(a);
    }
    {
        AbilityDef a;
        a.name = "Frost Nova";
        a.desc = "Bursts at a point: magic damage and a heavy slow.";
        a.cast = Cast::PointAoE; a.dtype = DmgType::Magical; a.fx = FX_SLOW;
        a.max_level = 4; a.req_level = {{1, 3, 5, 7}};
        a.range = {{380.0f, 380.0f, 380.0f, 380.0f}};
        a.radius = {{170.0f, 170.0f, 170.0f, 170.0f}};
        a.mana = {{90.0f, 100.0f, 110.0f, 120.0f}};
        a.cd = {{12.0f, 11.0f, 10.0f, 9.0f}};
        a.dmg = {{60.0f, 100.0f, 140.0f, 180.0f}};
        a.dur = {{3.0f, 3.0f, 3.0f, 3.0f}};
        a.pct = {{40.0f, 45.0f, 50.0f, 55.0f}};
        a.pct2 = {{0.0f, 0.0f, 0.0f, 0.0f}};
        a.speed = 0.0f; a.delay = 0.0f; a.aoe_on_land = false;
        c.abilities.push_back(a);
    }
    {
        AbilityDef a;
        a.name = "Blink";
        a.desc = "Teleports a short distance in the chosen direction.";
        a.cast = Cast::Blink; a.dtype = DmgType::Magical; a.fx = 0;
        a.max_level = 4; a.req_level = {{1, 3, 5, 7}};
        a.range = {{320.0f, 400.0f, 480.0f, 560.0f}};
        a.radius = {{0.0f, 0.0f, 0.0f, 0.0f}};
        a.mana = {{40.0f, 40.0f, 40.0f, 40.0f}};
        a.cd = {{14.0f, 12.0f, 10.0f, 8.0f}};
        a.dmg = {{0.0f, 0.0f, 0.0f, 0.0f}};
        a.dur = {{0.0f, 0.0f, 0.0f, 0.0f}};
        a.pct = {{0.0f, 0.0f, 0.0f, 0.0f}};
        a.pct2 = {{0.0f, 0.0f, 0.0f, 0.0f}};
        a.speed = 0.0f; a.delay = 0.0f; a.aoe_on_land = false;
        c.abilities.push_back(a);
    }
    {
        AbilityDef a;
        a.name = "Meteor";
        a.desc = "Summons a meteor that lands after a delay: huge damage and a short stun.";
        a.cast = Cast::PointAoE; a.dtype = DmgType::Magical; a.fx = FX_STUN;
        a.max_level = 3; a.req_level = {{6, 12, 18, 99}};
        a.range = {{620.0f, 620.0f, 620.0f, 620.0f}};
        a.radius = {{230.0f, 230.0f, 230.0f, 230.0f}};
        a.mana = {{180.0f, 240.0f, 300.0f, 300.0f}};
        a.cd = {{70.0f, 60.0f, 50.0f, 50.0f}};
        a.dmg = {{320.0f, 520.0f, 720.0f, 720.0f}};
        a.dur = {{0.5f, 0.8f, 1.0f, 1.0f}};
        a.pct = {{0.0f, 0.0f, 0.0f, 0.0f}};
        a.pct2 = {{0.0f, 0.0f, 0.0f, 0.0f}};
        a.speed = 0.0f; a.delay = 1.3f; a.aoe_on_land = false;
        c.abilities.push_back(a);
    }
    return c;
}

}  // namespace moba
