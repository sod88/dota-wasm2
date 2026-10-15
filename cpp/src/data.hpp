// Game data types and constants (C++ port of src/data.rs).
// The built-in tables themselves live in data.cpp; everything moddable is gathered in `Content`.
#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace moba {

constexpr float TICK_HZ = 30.0f;
constexpr float DT = 1.0f / TICK_HZ;

constexpr uint8_t TEAM_RADIANT = 0;
constexpr uint8_t TEAM_DIRE = 1;
constexpr uint8_t TEAM_NEUTRAL = 2;

constexpr size_t INV_SLOTS = 6;

enum class Kind : uint8_t { Hero = 0, Melee = 1, Ranged = 2, Siege = 3, Neutral = 4, Tower = 5, Ancient = 6 };
inline bool is_building(Kind k) { return k == Kind::Tower || k == Kind::Ancient; }
inline bool is_lane_creep(Kind k) { return k == Kind::Melee || k == Kind::Ranged || k == Kind::Siege; }

enum class DmgType : uint8_t { Physical = 0, Magical = 1, Pure = 2 };

/// How an ability is aimed / delivered.
enum class Cast : uint8_t {
    None = 0,
    Target = 1,    // enemy unit (instant, or a homing projectile if speed > 0)
    Line = 2,      // linear projectile towards a point; pierces everything in its path
    PointAoE = 3,  // area at a point (optionally with a telegraphed delay)
    SelfAoE = 4,   // area centred on the caster
    SelfBuff = 5,  // buff on the caster
    Blink = 6,     // teleport towards a point; optionally detonates an AoE on arrival
};

constexpr uint8_t FX_STUN = 1;
constexpr uint8_t FX_SLOW = 2;
constexpr uint8_t FX_ROOT = 4;
constexpr uint8_t FX_HASTE = 8;  // caster move-speed buff (pct2)
constexpr uint8_t FX_ASPD = 16;  // caster attack-speed buff (pct)

struct AbilityDef {
    std::string name, desc;
    Cast cast = Cast::None;
    DmgType dtype = DmgType::Magical;
    uint8_t fx = 0;
    uint8_t max_level = 4;
    /// Hero level required to learn level 1/2/3/4 of this ability.
    std::array<uint8_t, 4> req_level{{1, 3, 5, 7}};
    std::array<float, 4> range{}, radius{}, mana{}, cd{}, dmg{};
    /// Duration (seconds) of every status this ability applies.
    std::array<float, 4> dur{};
    /// Slow % (FX_SLOW) or attack-speed % (FX_ASPD).
    std::array<float, 4> pct{};
    /// Move-speed % buff (FX_HASTE).
    std::array<float, 4> pct2{};
    float speed = 0.0f;   // projectile speed (0 = instant)
    float delay = 0.0f;   // telegraph delay for PointAoE
    bool aoe_on_land = false;
    /// Optional front-end hints (icon glyph name, `#rrggbb` colour).
    std::string icon, color;
    bool operator==(const AbilityDef& o) const;
};

struct HeroDef {
    std::string name, title;
    /// 0 = strength, 1 = agility, 2 = intelligence (adds 1 damage per point).
    uint8_t primary = 0;
    float str_ = 0, agi = 0, int_ = 0, gain_str = 0, gain_agi = 0, gain_int = 0;
    float dmg_min = 0, dmg_max = 0, range = 0, bat = 1, ms = 0, proj_speed = 0, radius = 16;
    /// Item ids a bot buys, in order.
    std::vector<uint8_t> build;
    /// Bot skill priority among the three basic abilities (ult is always taken first).
    std::array<uint8_t, 3> skill_order{{0, 1, 2}};
    std::string glyph, color;
    bool operator==(const HeroDef& o) const;
};

constexpr uint8_t ACT_NONE = 0;
constexpr uint8_t ACT_SALVE = 1;
constexpr uint8_t ACT_MANA = 2;
constexpr uint8_t ACT_BLINK = 3;

struct ItemDef {
    std::string name;
    float cost = 0, str_ = 0, agi = 0, int_ = 0, dmg = 0, armor = 0, hp = 0, mana = 0, aspd = 0, ms = 0, hpreg = 0, mpreg = 0;
    uint8_t active = ACT_NONE;
    float cd = 0;
    /// Consumables stack in one slot (up to 5 charges).
    bool stack = false;
    std::string color;
    bool operator==(const ItemDef& o) const;
};

struct CreepDef {
    std::string name;
    Kind kind = Kind::Neutral;
    float hp = 0, dmg_min = 0, dmg_max = 0, armor = 0, range = 70, bat = 1, ms = 100, proj_speed = 0, radius = 12, vision = 260;
    float gold = 0, xp = 0;
    /// Damage multiplier against buildings.
    float bldg_mult = 1;
    bool operator==(const CreepDef& o) const;
};

struct TowerDef {
    float hp = 0, dmg_min = 0, dmg_max = 0, armor = 0, range = 0, bat = 1, gold = 0;
    /// Position along the lane (0 = own base, 1 = enemy base) from the owner's side.
    float lane_frac = 0;
    bool operator==(const TowerDef& o) const;
};

struct AncientDef {
    float hp = 0, armor = 0, radius = 0, vision = 0;
    bool operator==(const AncientDef& o) const;
};

constexpr float TOWER_RADIUS = 34.0f;
constexpr float TOWER_VISION = 480.0f;
constexpr float TOWER_PROJ_SPEED = 700.0f;

inline float armor_multiplier(float armor) {
    return 1.0f - (0.06f * armor) / (1.0f + 0.06f * std::fabs(armor));
}

}  // namespace moba
