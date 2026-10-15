// Entity model: one flat `Unit` struct covers heroes, creeps, neutrals and buildings.
// Hero-only state lives in `HeroData` (linked through `Unit::hero`). Port of src/unit.rs.
#pragma once
#include <array>
#include <cstdint>
#include <utility>
#include <vector>

#include "content.hpp"
#include "data.hpp"
#include "math.hpp"

namespace moba {

/// What a unit is doing. Mirrors Rust's `Order` enum: only the fields of the active kind matter.
struct Order {
    enum K : uint8_t { Idle, Move, AttackMove, Attack, Cast, Hold, Stay };
    K k = Idle;
    Vec2 pt;          // Move / AttackMove target, Cast point
    uint32_t target = 0;  // Attack target, Cast target
    uint8_t slot = 0;     // Cast: 0..=3 hero abilities, 10..=15 inventory items

    static Order idle() { return Order{}; }
    static Order move(Vec2 p) { Order o; o.k = Move; o.pt = p; return o; }
    static Order attack_move(Vec2 p) { Order o; o.k = AttackMove; o.pt = p; return o; }
    static Order attack(uint32_t t) { Order o; o.k = Attack; o.target = t; return o; }
    static Order cast(uint8_t slot, uint32_t t, Vec2 p) { Order o; o.k = Cast; o.slot = slot; o.target = t; o.pt = p; return o; }
    static Order hold() { Order o; o.k = Hold; return o; }
    static Order stay() { Order o; o.k = Stay; return o; }

    /// Compact id for the snapshot (UI/debug).
    float code() const { return (float)k; }
    bool operator==(const Order& o) const {
        if (k != o.k) return false;
        switch (k) {
            case Move:
            case AttackMove: return pt == o.pt;
            case Attack: return target == o.target;
            case Cast: return slot == o.slot && target == o.target && pt == o.pt;
            default: return true;
        }
    }
    bool operator!=(const Order& o) const { return !(*this == o); }
};

struct Unit {
    uint32_t id;
    Kind kind;
    /// Hero def index / neutral def index / tower tier.
    uint8_t def = 0;
    uint8_t team;
    bool alive = true;
    Vec2 pos;
    float facing = 0.0f;
    float radius = 12.0f;

    float hp = 1.0f, mana = 0.0f, max_hp = 1.0f, max_mana = 0.0f, armor = 0.0f, dmg_min = 0.0f, dmg_max = 0.0f;
    float range = 70.0f, bat = 1.0f;
    /// Attack-speed bonus in percent (agility + items), before temporary buffs.
    float aspd = 0.0f;
    float move_speed = 100.0f, hp_regen = 0.0f, mana_regen = 0.0f, vision = 300.0f, proj_speed = 0.0f, bldg_mult = 1.0f;
    float bounty_gold = 0.0f, bounty_xp = 0.0f;

    // --- orders / movement
    Order order;
    uint32_t target = 0;
    std::vector<Vec2> path;
    size_t path_i = 0;
    Vec2 path_goal;
    float repath_t = 0.0f;

    // --- attack state machine
    float swing_t = 0.0f, swing_len = 0.0f;
    uint32_t swing_target = 0;
    float attack_cd = 0.0f;

    // --- status effects
    float stun_t = 0, root_t = 0, slow_t = 0, slow_pct = 0, haste_t = 0, haste_pct = 0, aspd_t = 0, aspd_pct = 0;
    float salve_t = 0, salve_rate = 0, mana_pot_t = 0, mana_pot_rate = 0;

    /// Seconds since this unit last took damage.
    float since_dmg = 99.0f;

    // --- lane creeps
    uint8_t lane = 1;
    size_t wp = 0;
    // --- neutrals
    Vec2 home;
    int32_t camp = -1;
    bool returning = false;
    // --- towers: forced target after an enemy hero hits one of our heroes
    uint32_t force_target = 0;
    float force_t = 0.0f;

    int32_t hero = -1;

    static Unit blank(uint32_t id, Kind kind, uint8_t team, Vec2 pos) {
        Unit u;
        u.id = id;
        u.kind = kind;
        u.team = team;
        u.pos = pos;
        u.home = pos;
        return u;
    }

    bool is_hero() const { return kind == Kind::Hero; }
    bool is_building() const { return moba::is_building(kind); }
    bool stunned() const { return stun_t > 0.0f; }
    bool rooted() const { return root_t > 0.0f; }

    /// Effective movement speed including slows and haste.
    float speed() const {
        if (stunned() || rooted()) return 0.0f;
        float ms = move_speed;
        if (haste_t > 0.0f) ms *= 1.0f + haste_pct / 100.0f;
        if (slow_t > 0.0f) ms *= fmax_rs(1.0f - slow_pct / 100.0f, 0.2f);
        return ms;
    }
    /// Seconds between attacks (attack-speed adjusted).
    float attack_time() const {
        float a = aspd;
        if (aspd_t > 0.0f) a += aspd_pct;
        a = fclamp_rs(a, -50.0f, 400.0f);
        return bat / (1.0f + a / 100.0f);
    }
    float avg_damage() const { return (dmg_min + dmg_max) * 0.5f; }
    void clear_path() {
        path.clear();
        path_i = 0;
    }
};

struct ItemSlot {
    uint8_t id = 0;
    uint8_t charges = 0;
    float cd = 0.0f;
};

struct HeroData {
    uint32_t unit;
    uint8_t def, team, lane;
    bool bot;
    uint8_t level = 1;
    float xp = 0.0f;
    uint8_t skill_pts = 1;
    std::array<uint8_t, 4> ab_level{};
    std::array<float, 4> ab_cd{};
    float base_str, base_agi, base_int;
    float gold, networth;
    std::array<ItemSlot, INV_SLOTS> items{};
    uint32_t kills = 0, deaths = 0, assists = 0, last_hits = 0, denies = 0;
    float respawn_t = 0.0f;
    /// Enemy heroes that recently damaged us: (unit id, seconds ago) — for assists.
    std::vector<std::pair<uint32_t, float>> recent;
    // --- bot brain
    float think_t = 0.0f;
    uint8_t ai_state = 0;
    size_t build_i = 0;
    float shop_cd = 0.0f;
    Vec2 lane_front;

    HeroData(uint32_t unit_, uint8_t def_, uint8_t team_, uint8_t lane_, bool bot_, const Content& c)
        : unit(unit_), def(def_), team(team_), lane(lane_), bot(bot_) {
        const HeroDef& d = c.heroes[def];
        base_str = d.str_;
        base_agi = d.agi;
        base_int = d.int_;
        gold = c.rules.start_gold;
        networth = c.rules.start_gold;
    }

    /// Sum of passive bonuses from all carried items.
    ItemDef item_bonus(const Content& c) const {
        ItemDef b = c.items[0];
        for (const ItemSlot& s : items) {
            if (s.id == 0) continue;
            const ItemDef& it = c.items[s.id];
            b.str_ += it.str_;
            b.agi += it.agi;
            b.int_ += it.int_;
            b.dmg += it.dmg;
            b.armor += it.armor;
            b.hp += it.hp;
            b.mana += it.mana;
            b.aspd += it.aspd;
            b.ms += it.ms;
            b.hpreg += it.hpreg;
            b.mpreg += it.mpreg;
        }
        return b;
    }
    int free_slot() const {
        for (size_t i = 0; i < items.size(); ++i)
            if (items[i].id == 0) return (int)i;
        return -1;
    }
};

struct Proj {
    uint32_t id, src;
    uint8_t team;
    Vec2 pos, dir;
    float speed;
    /// Target unit id for homing projectiles; 0 for linear ones.
    uint32_t homing;
    float dmg;
    DmgType dtype;
    uint8_t fx;
    float dur, pct;
    /// hero*4+slot, or 255 for a basic attack.
    uint8_t ability;
    float radius, remaining;
    std::vector<uint32_t> hits;
};

struct Zone {
    uint32_t id;
    Vec2 pos;
    float radius, t_left, t_total;
    uint8_t team;
    uint32_t src;
    float dmg;
    DmgType dtype;
    uint8_t fx;
    float dur, pct;
    uint8_t ability;
};

struct Camp {
    Vec2 pos;
    uint8_t tier;
    std::vector<uint32_t> members;
    float respawn_t;
    bool waiting;
};

struct Cmd {
    enum K : uint8_t { Move, AttackMove, Attack, Stop, Hold, Cast, LevelUp, Buy, Sell };
    K k;
    uint32_t unit = 0;
    Vec2 to;               // Move / AttackMove / Cast point
    uint32_t target = 0;   // Attack / Cast
    uint8_t slot = 0;      // Cast / LevelUp / Sell
    uint8_t item = 0;      // Buy
};

}  // namespace moba
