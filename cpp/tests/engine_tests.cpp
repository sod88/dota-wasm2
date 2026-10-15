// Tests for the C++ engine. The expectations are shared with the Rust engine:
//  * the state hashes are the ones the original (pre-mod) Rust engine produced,
//  * the mod error messages come from tests/mod_cases.json, which the Rust tests read too.
// Build and run:  cmake -S cpp -B cpp/build && cmake --build cpp/build -j && cpp/build/engine_tests
#include <cinttypes>
#include <cstdio>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

#include "content.hpp"
#include "game.hpp"
#include "json.hpp"

using namespace moba;

static int g_failed = 0, g_checks = 0;
#define CHECK(cond, ...)                                              \
    do {                                                              \
        ++g_checks;                                                   \
        if (!(cond)) {                                                \
            ++g_failed;                                               \
            std::printf("  FAIL %s:%d: %s\n    ", __FILE__, __LINE__, #cond); \
            std::printf(__VA_ARGS__);                                 \
            std::printf("\n");                                        \
        }                                                             \
    } while (0)

static std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static Config bots(uint8_t per, uint32_t seed, uint8_t hero) {
    Config c;
    c.seed = seed;
    c.player_hero = hero;
    c.heroes_per_team = per;
    c.fog = true;
    c.player_is_bot = true;
    return c;
}

static std::unique_ptr<Game> play(std::shared_ptr<const Content> content, Config cfg, float minutes) {
    auto g = std::make_unique<Game>(cfg, std::move(content));
    while (g->winner < 0 && g->time < minutes * 60.0f) g->step();
    return g;
}

static bool load(const Content& base, const std::string& text, Content& out, std::vector<std::string>& errs) {
    return apply_mod(base, text, out, errs);
}

// ---------------------------------------------------------------------------------------------

static void builtin_content_plays_exactly_like_the_original_engine() {
    struct Case {
        uint8_t per;
        uint32_t seed;
        float mins;
        uint64_t hash;
    };
    const Case cases[] = {{1, 3, 8.0f, 0x7c1a7d559e22af65ull}, {2, 5, 10.0f, 0x9973291e5d27b766ull}, {3, 7, 12.0f, 0x28e896966a9369feull},
                          {5, 11, 6.0f, 0x960501e309a6dcd1ull}};
    for (const Case& c : cases) {
        auto g = play(builtin(), bots(c.per, c.seed, 0), c.mins);
        CHECK(g->state_hash() == c.hash, "%uv%u seed %u: got %016" PRIx64 ", Rust engine %016" PRIx64, c.per, c.per, c.seed, g->state_hash(), c.hash);
    }
}

static void shared_mod_cases_give_the_expected_errors() {
    std::string text = read_file(std::string(MOBA_REPO_DIR) + "/tests/mod_cases.json");
    json::Value cases;
    std::string err;
    CHECK(json::parse(text, cases, err), "mod_cases.json: %s", err.c_str());
    CHECK(cases.type == json::Value::Arr && cases.arr.size() >= 40, "expected at least 40 cases");
    for (const json::Value& c : cases.arr) {
        auto get = [&](const char* k) -> const json::Value* {
            for (auto& kv : c.obj)
                if (kv.first == k) return &kv.second;
            return nullptr;
        };
        std::string name = get("name")->s;
        std::string input = get("mod_text") ? get("mod_text")->s : json::to_pretty(*get("mod"));
        std::vector<std::string> expected;
        for (auto& e : get("errors")->arr) expected.push_back(e.s);
        Content out;
        std::vector<std::string> got;
        load(*builtin(), input, out, got);
        bool same = got == expected;
        std::string detail;
        if (!same) {
            detail = "got:";
            for (auto& g : got) detail += "\n      " + g;
            detail += "\n    expected:";
            for (auto& e : expected) detail += "\n      " + e;
        }
        CHECK(same, "case \"%s\"\n    %s", name.c_str(), detail.c_str());
    }
}

static void invalid_utf8_and_oversized_files_are_rejected() {
    Content out;
    std::vector<std::string> errs;
    load(*builtin(), std::string("{\xff}"), out, errs);
    CHECK(errs == std::vector<std::string>{"mod file is not valid UTF-8 text"}, "utf8");
    load(*builtin(), std::string((1 << 20) + 1, ' '), out, errs);
    CHECK(errs == std::vector<std::string>{"mod file is larger than 1 MiB"}, "size");
}

static void exported_builtin_is_a_valid_no_op_mod() {
    std::string exported = builtin()->to_mod_json();
    Content back;
    std::vector<std::string> errs;
    CHECK(load(*builtin(), exported, back, errs), "the exported content must load (%zu errors)", errs.size());
    Content expect = *builtin();
    expect.mods = back.mods;
    CHECK(back == expect, "loading the export changes nothing");
    auto g = play(std::make_shared<const Content>(back), bots(2, 5, 0), 10.0f);
    CHECK(g->state_hash() == 0x9973291e5d27b766ull, "hash %016" PRIx64, g->state_hash());
}

static const char* NIGHTBLADE = R"JSON({
  "name": "Nightblade pack", "version": "1.0", "author": "test",
  "items": [{"name": "Shadow Cloak", "cost": 600, "agi": 8, "armor": 2, "ms": 10}],
  "heroes": [{
    "name": "Nightblade", "title": "Agility - assassin", "primary": "agi",
    "str": 18, "agi": 28, "int": 14, "gain_str": 2, "gain_agi": 3.2, "gain_int": 1.2,
    "dmg_min": 30, "dmg_max": 36, "range": 110, "bat": 1.6, "ms": 185, "radius": 15,
    "glyph": "bolt", "color": "#6a5acd",
    "build": ["Boots of Speed", "Shadow Cloak", "Blink Dagger"], "skill_order": [1, 0, 2],
    "abilities": [
      {"name": "Shadow Dart", "desc": "A fast dart.", "cast": "line", "range": 600, "radius": 25, "mana": 50, "cd": [8, 7, 6, 5], "dmg": [70, 110, 150, 190], "speed": 900},
      {"name": "Venom Strike", "desc": "Slows.", "cast": "target", "fx": ["slow"], "range": 300, "mana": 60, "cd": 9, "dmg": [50, 90, 130, 170], "dur": 3, "pct": [30, 40, 50, 60], "speed": 800},
      {"name": "Fleet Step", "desc": "Haste.", "cast": "self_buff", "fx": ["haste", "aspd"], "mana": 40, "cd": 18, "dur": 5, "pct": [30, 40, 50, 60], "pct2": [15, 20, 25, 30]},
      {"name": "Death Mark", "desc": "Leap and strike.", "cast": "blink", "dtype": "pure", "fx": ["stun"], "max_level": 3, "req_level": [6, 12, 18], "range": 450, "radius": 200, "mana": [150, 200, 250], "cd": [60, 50, 40], "dmg": [200, 320, 440], "dur": [1, 1.5, 2], "aoe_on_land": true}
    ]
  }]
})JSON";

static void a_new_hero_is_playable_and_deterministic() {
    Content c;
    std::vector<std::string> errs;
    CHECK(load(*builtin(), NIGHTBLADE, c, errs), "valid mod");
    CHECK(c.heroes.size() == 4 && c.abilities.size() == 16 && c.heroes[3].name == "Nightblade", "roster");
    CHECK(c.heroes[3].build.size() == 3 && c.abilities[15].name == "Death Mark" && c.mods[0].name == "Nightblade pack", "details");
    auto shared = std::make_shared<const Content>(c);
    auto a = play(shared, bots(2, 9, 3), 12.0f);
    auto b = play(shared, bots(2, 9, 3), 12.0f);
    CHECK(a->state_hash() == b->state_hash(), "same mod + seed => same match");
    const HeroData* nb = nullptr;
    for (auto& h : a->heroes)
        if (h.def == 3) nb = &h;
    CHECK(nb != nullptr, "Nightblade was spawned");
    if (nb) {
        bool has_item = false;
        for (auto& s : nb->items) has_item = has_item || s.id != 0;
        CHECK(has_item && nb->level > 1 && nb->last_hits > 0, "it bought items, levelled up and farmed");
    }
}

static void mods_stack_and_rules_drive_the_simulation() {
    Content first, second, out;
    std::vector<std::string> errs;
    CHECK(load(*builtin(), R"({"name":"one","rules":{"start_gold":700,"wave_melee":2}})", first, errs), "one");
    CHECK(load(first, R"({"name":"two","rules":{"start_gold":900}})", second, errs), "two");
    CHECK(second.rules.start_gold == 900.0f && second.rules.wave_melee == 2 && second.mods.size() == 2, "stacking");
    CHECK(load(*builtin(), R"({"rules":{"start_gold":1234,"wave_melee":0,"wave_ranged":0,"siege_every":0,"first_wave_at":1}})", out, errs), "rules");
    Game g(Config{}, std::make_shared<const Content>(out));
    CHECK(g.heroes[0].gold == 1234.0f, "start gold");
    g.run(40 * 30);
    bool creeps = false;
    for (auto& u : g.units) creeps = creeps || is_lane_creep(u.kind);
    CHECK(!creeps, "waves are empty");
}

int main() {
    const std::pair<const char*, std::function<void()>> tests[] = {
        {"builtin content plays exactly like the original engine", builtin_content_plays_exactly_like_the_original_engine},
        {"shared mod cases give the expected errors", shared_mod_cases_give_the_expected_errors},
        {"invalid UTF-8 and oversized files are rejected", invalid_utf8_and_oversized_files_are_rejected},
        {"exported built-in content is a valid no-op mod", exported_builtin_is_a_valid_no_op_mod},
        {"a new hero is playable and deterministic", a_new_hero_is_playable_and_deterministic},
        {"mods stack and rules drive the simulation", mods_stack_and_rules_drive_the_simulation},
    };
    for (auto& t : tests) {
        int before = g_failed;
        t.second();
        std::printf("%s  %s\n", g_failed == before ? "ok  " : "FAIL", t.first);
    }
    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
