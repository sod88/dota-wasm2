// The simulation core: world state, fixed-timestep pipeline, spawning, vision, hero stats
// (C++ port of src/game.rs; combat.cpp, abilities.cpp and ai.cpp implement the rest of `Game`).
//
// One `Game::step()` advances the world by 1 / TICK_HZ seconds. Given the same `Config`, content and
// stream of commands the simulation is fully deterministic, and bit-identical to the Rust engine.
#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "content.hpp"
#include "map.hpp"
#include "math.hpp"
#include "path.hpp"
#include "unit.hpp"

namespace moba {

// ---- event type codes (see `Game::events`: [type, a, b, x, y, v, w, t]) ----
constexpr float EV_DAMAGE = 1.0f;    // a=victim b=attacker v=amount w=dmg type
constexpr float EV_KILL = 2.0f;      // a=victim b=killer v=gold w=victim kind t=victim team
constexpr float EV_HEAL = 3.0f;      // a=unit v=amount
constexpr float EV_LEVEL = 4.0f;     // a=hero unit v=new level
constexpr float EV_BUILDING = 5.0f;  // a=building id b=kind t=team
constexpr float EV_CAST = 6.0f;      // a=caster b=slot x,y=point v=radius w=ability id (hero*4+slot) or 100+item
constexpr float EV_GOLD = 7.0f;      // a=hero unit v=amount
constexpr float EV_GAMEOVER = 8.0f;  // a=winning team
constexpr float EV_WAVE = 9.0f;      // a=wave number
constexpr float EV_RESPAWN = 10.0f;  // a=hero unit
constexpr float EV_ATTACK = 11.0f;   // a=attacker b=target
constexpr float EV_ITEM = 12.0f;     // a=hero b=item v=1 bought, 2 sold
constexpr float EV_LEARN = 13.0f;    // a=hero b=slot v=new ability level
constexpr float EV_BLINK = 14.0f;    // a=unit x,y=from v,w=to
constexpr float EV_ASSIST = 15.0f;

constexpr std::array<uint8_t, 5> LANE_ORDER{{1, 0, 2, 0, 2}};  // mid, top, bot, top, bot

using Event = std::array<float, 8>;

struct Config {
    uint32_t seed = 1;
    uint8_t player_hero = 0;
    uint8_t heroes_per_team = 1;
    bool fog = true;
    /// If true the "player" hero is also driven by the bot AI (watch mode / tests).
    bool player_is_bot = false;
};

inline Vec2 base_of(uint8_t team) { return team == TEAM_RADIANT ? BASE_RADIANT : BASE_DIRE; }

class Game {
   public:
    Config cfg;
    /// The rule set and content tables this match runs on (built-in or modded).
    std::shared_ptr<const Content> content_ptr;
    const Content& content;
    float time = 0.0f;
    uint32_t tick = 0;
    Map map;
    PathFinder finder;
    Rng rng;
    std::vector<Unit> units;
    std::vector<HeroData> heroes;
    std::vector<Proj> projs;
    std::vector<Zone> zones;
    std::vector<Camp> camps;
    uint32_t next_id = 1;
    std::vector<Event> events;
    std::vector<Cmd> cmds;
    float wave_t;
    uint32_t wave_no = 0;
    /// -1 while the game is running, otherwise the winning team.
    int32_t winner = -1;
    /// Unit id of the human-controlled hero.
    uint32_t player = 0;
    uint8_t player_team = TEAM_RADIANT;
    bool fog;
    std::array<std::vector<bool>, 2> vis;
    std::array<std::array<std::vector<Vec2>, 3>, 2> lane_paths;
    std::array<uint32_t, 2> ancient_ids{};
    /// tw_alive[team][lane][tier]
    std::array<std::array<std::array<bool, 3>, 3>, 2> tw_alive;

    /// A match on the built-in content.
    explicit Game(Config cfg);
    /// A match on custom (modded) content.
    Game(Config cfg, std::shared_ptr<const Content> content);
    Game(const Game&) = delete;
    Game& operator=(const Game&) = delete;

    // ---- lookup helpers (game.cpp)
    uint32_t alloc_id() { return next_id++; }
    /// Index of a unit by id (units stay sorted by id, so this is a binary search).
    std::optional<size_t> idx(uint32_t id) const;
    /// Index of a unit that exists and is alive.
    std::optional<size_t> live_idx(uint32_t id) const;
    void emit(const Event& ev) { events.push_back(ev); }
    std::optional<size_t> player_idx() const { return idx(player); }
    bool visible_to(uint8_t team, size_t ui) const;

    // ---- heroes, economy (game.cpp)
    uint32_t spawn_hero(uint8_t def, uint8_t team, uint8_t lane, size_t k, bool bot);
    void recalc_hero(size_t ui, bool grow);
    void add_xp(size_t ui, float xp);
    void add_gold(size_t ui, float amount);
    bool learn_ability(size_t ui, size_t slot);
    bool near_shop(size_t ui) const;
    bool buy_item(size_t ui, uint8_t item);
    bool sell_item(size_t ui, size_t slot);
    uint32_t spawn_creep(const CreepDef& def, uint8_t team, uint8_t lane, Vec2 pos);

    // ---- vision, structures, main loop (game.cpp)
    void recompute_tower_flags();
    bool building_protected(size_t ui) const;
    void compute_vision();
    void step();
    void run(uint32_t n) {
        for (uint32_t i = 0; i < n; ++i) step();
    }
    void push_cmd(const Cmd& c) { cmds.push_back(c); }
    void set_order(size_t i, Order o);
    /// FNV-1a hash of the simulation state, used to detect desyncs and to test determinism.
    uint64_t state_hash() const;
    size_t count_kind(uint8_t team, Kind kind) const;

    // ---- per-unit simulation (combat.cpp)
    void update_timers(size_t i);
    bool can_attack(size_t i, size_t ti) const;
    bool target_ok(size_t i, uint32_t tid, float max_edge_dist) const;
    uint32_t acquire(size_t i, float radius, bool allow_neutral) const;
    void act(size_t i);
    bool in_attack_range(size_t i, size_t ti) const;
    void face(size_t i, Vec2 p);
    bool move_toward(size_t i, Vec2 goal);
    void separate_units();
    float deal_damage(uint32_t src, size_t di, float amount, DmgType dtype);
    void apply_effects(uint32_t src, size_t di, float dmg, DmgType dtype, uint8_t fx, float dur, float pct);
    void kill_unit(size_t di, uint32_t killer_id);
    void update_projectiles();
    void update_zones();

    // ---- abilities and items (abilities.cpp)
    void aoe_hit(uint32_t src, uint8_t team, Vec2 center, float radius, float dmg, DmgType dtype, uint8_t fx, float dur, float pct);
    bool ability_ready(size_t ui, size_t slot) const;
    /// The learned ability in `slot` and its level index, or nullptr.
    const AbilityDef* ability_def(size_t ui, size_t slot, size_t& level_idx) const;
    void order_cast(size_t ui, uint8_t slot, uint32_t target, Vec2 point);
    void act_cast(size_t i, uint8_t slot, uint32_t target, Vec2 point);
    bool cast_ability(size_t ui, size_t slot, uint32_t target, Vec2 point);
    std::optional<Vec2> do_blink(size_t ui, Vec2 point, float range);
    bool use_item(size_t ui, size_t slot, Vec2 point);

    // ---- AI (ai.cpp)
    void think(size_t i);

   private:
    float gold_t_ = 0.0f;

    void build_lane_paths();
    void build_structures();
    void build_camps();
    void build_heroes();
    Vec2 hero_spawn_pos(uint8_t team, size_t k) const;
    void spawn_wave();
    void spawn_camp(size_t ci);
    void spawn_logic();
    void cleanup();
    std::optional<size_t> controllable(uint32_t id) const;
    Vec2 clamp_world(Vec2 p) const;
    void apply_commands();

    // combat.cpp
    float acquire_radius(size_t i) const;
    void act_idle(size_t i, bool hold);
    void act_attack_move(size_t i, Vec2 to);
    void attack_or_chase(size_t i, uint32_t tid);
    void finish_swing(size_t i);
    void apply_move(size_t i, Vec2 next);
    void award_xp(uint8_t team, Vec2 center, float xp);
    void award_gold(size_t ui, float gold, Vec2 at);

    // ai.cpp
    void think_lane_creep(size_t i);
    void think_neutral(size_t i);
    void think_tower(size_t i);
    void bot_level_skills(size_t ui);
    void bot_shop(size_t ui);
    std::optional<float> next_item_cost(size_t hi) const;
    Vec2 lane_hold_pos(uint8_t team, size_t lane) const;
    std::optional<size_t> tower_threat_at(uint8_t team, Vec2 p) const;
    void think_bot(size_t ui);
    float best_ability_range(size_t ui) const;
    void bot_use_consumables(size_t ui, float hp_frac, float mana_frac, bool enemy_near);
    void bot_escape(size_t ui, const std::vector<size_t>& enemy_heroes, Vec2 base);
    void bot_fight_abilities(size_t ui, size_t target, const std::vector<size_t>& enemy_heroes);
};

}  // namespace moba
