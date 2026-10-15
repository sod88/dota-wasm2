// Moddable game content (C++ port of src/content.rs).
//
// Everything a modder can change lives in one `Content` value: heroes, abilities, items, creeps,
// neutrals, camps, towers, the Ancient, the XP table and the game `Rules`. The built-in game is
// `builtin()`; a mod is a JSON document that patches it (`apply_mod`). Format, validation rules and
// error messages are identical to the Rust engine, so a mod behaves the same in both.
#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "data.hpp"

namespace moba {

constexpr size_t MAX_HEROES = 24;
constexpr size_t MAX_ITEMS = 100;
constexpr size_t MAX_NEUTRALS = 16;
constexpr size_t MAX_MOD_BYTES = 1 << 20;

/// Tunable game rules. `RULE_TABLE` (content.cpp) lists them with their JSON names and ranges.
struct Rules {
    float start_gold, passive_gold, wave_interval, first_wave_at, camp_respawn, xp_radius, shop_radius, magic_resist;
    float blink_range, salve_heal, salve_time, mana_potion_restore, mana_potion_time, hero_vision;
    float hp_base, hp_per_str, mana_base, mana_per_int, armor_base, armor_per_agi;
    float hp_regen_base, hp_regen_per_str, mana_regen_base, mana_regen_per_int;
    float respawn_base, respawn_per_level, respawn_max, hero_bounty_base, hero_bounty_per_level, hero_xp_base, hero_xp_per_level;
    uint32_t wave_melee, wave_ranged, wave_siege, siege_every;
    static Rules builtin();
    bool operator==(const Rules& o) const;
};

struct ModInfo {
    std::string name, version, author, description;
    bool operator==(const ModInfo& o) const {
        return name == o.name && version == o.version && author == o.author && description == o.description;
    }
};

struct Content {
    std::vector<HeroDef> heroes;
    /// heroes.size() * 4; index = hero * 4 + slot.
    std::vector<AbilityDef> abilities;
    /// Index 0 is "no item".
    std::vector<ItemDef> items;
    CreepDef melee, ranged, siege;
    std::vector<CreepDef> neutrals;
    /// Neutral indices per camp tier (3 tiers).
    std::vector<std::vector<uint8_t>> camp_comp;
    std::array<TowerDef, 3> towers;
    AncientDef ancient;
    /// Cumulative XP to reach level i + 1; its length is the level cap.
    std::vector<float> xp_table;
    Rules rules;
    /// Mods applied so far (informational).
    std::vector<ModInfo> mods;

    size_t max_level() const { return xp_table.size(); }
    /// The content as a mod file: applying it to the built-in content changes nothing.
    std::string to_mod_json() const;
    bool operator==(const Content& o) const;
};

/// The built-in game (data.cpp).
Content make_builtin_content();
/// The shared built-in content.
std::shared_ptr<const Content> builtin();

/// Apply one mod (JSON bytes) on top of `base`. On success fills `out` and returns true; on any error
/// nothing is applied and `errors` holds up to 20 messages, each starting with the offending path.
bool apply_mod(const Content& base, const std::string& bytes, Content& out, std::vector<std::string>& errors);

}  // namespace moba
