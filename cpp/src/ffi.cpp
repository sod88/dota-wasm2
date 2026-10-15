// C ABI between the simulation and JavaScript (port of src/ffi.rs). ABI version 2.
//
// JavaScript never sees C++ objects. It drives the engine through a few `extern "C"` functions that
// only take numbers, and reads results as flat f32 tables straight out of WebAssembly memory. The
// functions, table layouts, info JSON and mod interface are byte-for-byte the same as the Rust
// engine's, so the web front end (and any tool) can use either engine unchanged:
//
//   moba_new(seed, hero, per_team, fog, bot)    start / restart a match
//   moba_step(n)                                advance n fixed ticks (30 Hz)
//   moba_cmd_*(...)                             queue commands for the player's hero
//   moba_snapshot()                             rebuild the tables below (also drains events)
//   moba_units_ptr() / heroes / projs / zones / events / global   -> pointer to f32 rows
//   moba_info_ptr() / moba_info_len()           static JSON: layouts, heroes, abilities, items, map
//   moba_terrain_ptr()                          GRID*GRID bytes of terrain codes
//   moba_alloc(len) -> ptr                      staging buffer for a mod file (JS writes the bytes)
//   moba_mod_load(len) -> errors                parse + apply the staged mod; 0 = ok (stackable)
//   moba_mod_error_ptr() / moba_mod_error_len() newline-separated error messages of the last load
//   moba_mod_clear() / moba_mod_count()         back to the built-in content / mods applied
//   moba_content_export() -> len, moba_content_ptr()   the active content as a mod file
//
// Fog of war is enforced here: enemy units, projectiles, zones and events that the player's team
// cannot currently see are simply not written into the tables.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "content.hpp"
#include "game.hpp"
#include "json.hpp"

#if defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h>
#define MOBA_API extern "C" EMSCRIPTEN_KEEPALIVE
#else
#define MOBA_API extern "C" __attribute__((visibility("default"), used))
#endif

namespace moba {
namespace {

constexpr uint32_t ABI_VERSION = 2;

const char* const UNIT_FIELDS[] = {"id",     "kind",    "def",   "team",   "alive",  "x",     "y",     "facing", "radius",
                                   "hp",     "max_hp",  "mana",  "max_mana", "order", "target", "visible", "swing_t", "swing_len",
                                   "status", "level",   "hero",  "range",  "vision", "armor", "speed", "dmg"};
const char* const HERO_FIELDS[] = {"unit",     "def",       "team",   "level",  "xp",      "xp_prev",  "xp_next", "skill_pts", "gold",
                                   "networth", "kills",     "deaths", "assists", "last_hits", "denies", "respawn", "bot",      "alive",
                                   "ab0",      "ab1",       "ab2",    "ab3",    "cd0",     "cd1",      "cd2",     "cd3",       "it0",
                                   "it1",      "it2",       "it3",    "it4",    "it5",     "ch0",      "ch1",     "ch2",       "ch3",
                                   "ch4",      "ch5",       "icd0",   "icd1",   "icd2",    "icd3",     "icd4",    "icd5",      "str",
                                   "agi",      "int",       "dmg_min", "dmg_max", "armor", "ms",       "atk_time", "hp_regen", "mana_regen"};
const char* const PROJ_FIELDS[] = {"id", "x", "y", "dx", "dy", "fx", "team", "radius", "ability", "homing", "speed"};
const char* const ZONE_FIELDS[] = {"id", "x", "y", "radius", "t_left", "t_total", "team", "ability", "fx"};
const char* const EVENT_FIELDS[] = {"type", "a", "b", "x", "y", "v", "w", "t", "visible"};
const char* const GLOBAL_FIELDS[] = {"time",    "tick",    "wave_no", "next_wave", "winner",  "player",    "player_team", "fog",
                                     "n_units", "n_heroes", "n_projs", "n_zones",  "n_events", "near_shop", "player_hero"};
template <size_t N>
constexpr size_t count(const char* const (&)[N]) {
    return N;
}

/// Bits of the unit `status` column.
constexpr uint32_t ST_STUN = 1, ST_ROOT = 2, ST_SLOW = 4, ST_HASTE = 8, ST_ASPD = 16, ST_SALVE = 32, ST_MANA = 64;

/// Keep at most this many undelivered events if JS stops taking snapshots (background tab).
constexpr size_t MAX_EVENTS = 4096;
/// Largest mod file the staging buffer accepts.
constexpr size_t MAX_STAGE = 8u << 20;

inline float b2f(bool b) { return b ? 1.0f : 0.0f; }

struct Bridge {
    std::unique_ptr<Game> game;
    std::string info;
    std::vector<float> units, heroes, projs, zones, events, global;
};

std::unique_ptr<Bridge> BRIDGE;

/// Can the player's team currently see world point `p`?
bool seen(const Game& g, Vec2 p) {
    if (!g.fog) return true;
    for (const Unit& u : g.units)
        if (u.alive && u.team == g.player_team && u.pos.dist_sq(p) <= u.vision * u.vision) return true;
    return false;
}

bool is_positional(float t) {
    return t == EV_DAMAGE || t == EV_KILL || t == EV_CAST || t == EV_ATTACK || t == EV_BUILDING || t == EV_GOLD || t == EV_LEVEL ||
           t == EV_RESPAWN || t == EV_ASSIST || t == EV_BLINK;
}

void snapshot(Bridge& b) {
    Game& g = *b.game;
    // Unit indices shift when dead units are swept, so vision must be rebuilt for *now*.
    g.compute_vision();
    uint8_t pt = g.player_team;

    // ---- units (enemies in the fog are omitted; buildings are always listed, flagged hidden)
    auto& units = b.units;
    units.clear();
    for (size_t ui = 0; ui < g.units.size(); ++ui) {
        const Unit& u = g.units[ui];
        if (!u.alive) continue;
        bool visible = u.team == pt || g.visible_to(pt, ui);
        if (!visible && !u.is_building()) continue;
        float level = u.hero >= 0 ? (float)g.heroes[(size_t)u.hero].level : 0.0f;
        float hero = u.hero >= 0 ? (float)u.hero : -1.0f;
        uint32_t status = 0;
        if (u.stun_t > 0.0f) status |= ST_STUN;
        if (u.root_t > 0.0f) status |= ST_ROOT;
        if (u.slow_t > 0.0f) status |= ST_SLOW;
        if (u.haste_t > 0.0f) status |= ST_HASTE;
        if (u.aspd_t > 0.0f) status |= ST_ASPD;
        if (u.salve_t > 0.0f) status |= ST_SALVE;
        if (u.mana_pot_t > 0.0f) status |= ST_MANA;
        units.insert(units.end(), {(float)u.id, (float)(uint8_t)u.kind, (float)u.def, (float)u.team, 1.0f, u.pos.x, u.pos.y, u.facing,
                                   u.radius, u.hp, u.max_hp, u.mana, u.max_mana, u.order.code(), (float)u.target, b2f(visible), u.swing_t,
                                   u.swing_len, (float)status, level, hero, u.range, u.vision, u.armor, u.speed(), u.avg_damage()});
    }

    // ---- heroes
    auto& heroes = b.heroes;
    heroes.clear();
    for (const HeroData& h : g.heroes) {
        auto oi = g.idx(h.unit);
        if (!oi) continue;
        const Unit& u = g.units[*oi];
        size_t lvl = h.level;
        size_t max_level = g.content.xp_table.size();
        float xp_prev = g.content.xp_table[std::min(lvl - 1, max_level - 1)];
        float xp_next = g.content.xp_table[std::min(lvl, max_level - 1)];
        heroes.insert(heroes.end(), {(float)h.unit, (float)h.def, (float)h.team, (float)h.level, h.xp, xp_prev, xp_next, (float)h.skill_pts,
                                     h.gold, h.networth, (float)h.kills, (float)h.deaths, (float)h.assists, (float)h.last_hits,
                                     (float)h.denies, u.alive ? 0.0f : fmax_rs(h.respawn_t, 0.0f), b2f(h.bot), b2f(u.alive)});
        for (uint8_t v : h.ab_level) heroes.push_back((float)v);
        for (float v : h.ab_cd) heroes.push_back(fmax_rs(v, 0.0f));
        for (const ItemSlot& s : h.items) heroes.push_back((float)s.id);
        for (const ItemSlot& s : h.items) heroes.push_back((float)s.charges);
        for (const ItemSlot& s : h.items) heroes.push_back(fmax_rs(s.cd, 0.0f));
        ItemDef bonus = h.item_bonus(g.content);
        heroes.insert(heroes.end(), {h.base_str + bonus.str_, h.base_agi + bonus.agi, h.base_int + bonus.int_, u.dmg_min, u.dmg_max, u.armor,
                                     u.speed(), u.attack_time(), u.hp_regen, u.mana_regen});
    }

    // ---- projectiles & zones
    b.projs.clear();
    for (const Proj& p : g.projs) {
        if (!seen(g, p.pos)) continue;
        b.projs.insert(b.projs.end(), {(float)p.id, p.pos.x, p.pos.y, p.dir.x, p.dir.y, (float)p.fx, (float)p.team, p.radius, (float)p.ability,
                                       b2f(p.homing != 0), p.speed});
    }
    b.zones.clear();
    for (const Zone& z : g.zones) {
        if (z.team != pt && !seen(g, z.pos)) continue;
        b.zones.insert(b.zones.end(), {(float)z.id, z.pos.x, z.pos.y, z.radius, z.t_left, z.t_total, (float)z.team, (float)z.ability, (float)z.fx});
    }

    // ---- events (drained)
    b.events.clear();
    std::vector<Event> evs = std::move(g.events);
    g.events.clear();
    size_t first = evs.size() > MAX_EVENTS ? evs.size() - MAX_EVENTS : 0;
    for (size_t k = first; k < evs.size(); ++k) {
        const Event& e = evs[k];
        bool vis = !is_positional(e[0]) || seen(g, Vec2(e[3], e[4]));
        b.events.insert(b.events.end(), e.begin(), e.end());
        b.events.push_back(b2f(vis));
    }

    // ---- global
    auto pi = g.player_idx();
    int32_t player_hero = pi ? g.units[*pi].hero : -1;
    bool near_shop = pi ? g.near_shop(*pi) : false;
    b.global = {g.time,
                (float)g.tick,
                (float)g.wave_no,
                fmax_rs(g.wave_t, 0.0f),
                (float)g.winner,
                (float)g.player,
                (float)g.player_team,
                b2f(g.fog),
                (float)(units.size() / count(UNIT_FIELDS)),
                (float)(heroes.size() / count(HERO_FIELDS)),
                (float)(b.projs.size() / count(PROJ_FIELDS)),
                (float)(b.zones.size() / count(ZONE_FIELDS)),
                (float)(b.events.size() / count(EVENT_FIELDS)),
                b2f(near_shop),
                (float)player_hero};
}

// ------------------------------------------------------------------------------ static info

using json::fmt_f32;
using json::quote;
std::string num(float v) { return std::isfinite(v) ? fmt_f32(v) : "0"; }
std::string boolean(bool v) { return v ? "true" : "false"; }
template <typename T>
std::string I(T v) {
    return std::to_string(v);
}

void arr(std::string& s, const float* v, size_t n) {
    s += '[';
    for (size_t i = 0; i < n; ++i) {
        if (i > 0) s += ',';
        s += num(v[i]);
    }
    s += ']';
}
void name_list(std::string& s, const std::vector<std::string>& v) {
    s += '[';
    for (size_t i = 0; i < v.size(); ++i) {
        if (i > 0) s += ',';
        s += quote(v[i]);
    }
    s += ']';
}
template <size_t N>
void name_list(std::string& s, const char* const (&v)[N]) {
    name_list(s, std::vector<std::string>(v, v + N));
}
void pt_list(std::string& s, const std::vector<Vec2>& pts) {
    s += '[';
    for (size_t i = 0; i < pts.size(); ++i) {
        if (i > 0) s += ',';
        float xy[2] = {pts[i].x, pts[i].y};
        arr(s, xy, 2);
    }
    s += ']';
}

std::string build_info(const Game& g) {
    const Content& c = g.content;
    std::string s;
    s.reserve(32 * 1024);
    s += "{\"abi\":" + I(ABI_VERSION) + ",\"tick_hz\":" + fmt_f32(TICK_HZ) + ",\"world\":" + fmt_f32(WORLD) + ",\"cell\":" + fmt_f32(CELL) +
         ",\"grid\":" + I(GRID) + ",\"shop_radius\":" + fmt_f32(c.rules.shop_radius) + ",\"max_level\":" + I(c.max_level()) +
         ",\"inv_slots\":" + I(INV_SLOTS) + ",\"wave_interval\":" + fmt_f32(c.rules.wave_interval) + ",\"xp_radius\":" + fmt_f32(c.rules.xp_radius) +
         ",\"start_gold\":" + fmt_f32(c.rules.start_gold) + ",\"blink_dagger_range\":" + fmt_f32(c.rules.blink_range);
    s += ",\"xp_table\":";
    arr(s, c.xp_table.data(), c.xp_table.size());
    s += ",\"mods\":[";
    for (size_t i = 0; i < c.mods.size(); ++i) {
        if (i > 0) s += ',';
        const ModInfo& m = c.mods[i];
        s += "{\"name\":" + quote(m.name) + ",\"version\":" + quote(m.version) + ",\"author\":" + quote(m.author) + ",\"description\":" +
             quote(m.description) + "}";
    }
    s += ']';

    // field layouts
    s += ",\"unit_fields\":";
    name_list(s, UNIT_FIELDS);
    s += ",\"hero_fields\":";
    name_list(s, HERO_FIELDS);
    s += ",\"proj_fields\":";
    name_list(s, PROJ_FIELDS);
    s += ",\"zone_fields\":";
    name_list(s, ZONE_FIELDS);
    s += ",\"event_fields\":";
    name_list(s, EVENT_FIELDS);
    s += ",\"global_fields\":";
    name_list(s, GLOBAL_FIELDS);

    // enums / codes
    s += ",\"kind\":{\"hero\":0,\"melee\":1,\"ranged\":2,\"siege\":3,\"neutral\":4,\"tower\":5,\"ancient\":6}";
    s += ",\"cast\":{\"none\":0,\"target\":1,\"line\":2,\"point\":3,\"self_aoe\":4,\"self_buff\":5,\"blink\":6}";
    s += ",\"status\":{\"stun\":" + I(ST_STUN) + ",\"root\":" + I(ST_ROOT) + ",\"slow\":" + I(ST_SLOW) + ",\"haste\":" + I(ST_HASTE) +
         ",\"aspd\":" + I(ST_ASPD) + ",\"salve\":" + I(ST_SALVE) + ",\"mana\":" + I(ST_MANA) + "}";
    s += ",\"fx\":{\"stun\":" + I(FX_STUN) + ",\"slow\":" + I(FX_SLOW) + ",\"root\":" + I(FX_ROOT) + ",\"haste\":" + I(FX_HASTE) +
         ",\"aspd\":" + I(FX_ASPD) + "}";
    s += ",\"act\":{\"none\":" + I(ACT_NONE) + ",\"salve\":" + I(ACT_SALVE) + ",\"mana\":" + I(ACT_MANA) + ",\"blink\":" + I(ACT_BLINK) + "}";
    s += ",\"ev\":{\"damage\":" + fmt_f32(EV_DAMAGE) + ",\"kill\":" + fmt_f32(EV_KILL) + ",\"heal\":" + fmt_f32(EV_HEAL) + ",\"level\":" +
         fmt_f32(EV_LEVEL) + ",\"building\":" + fmt_f32(EV_BUILDING) + ",\"cast\":" + fmt_f32(EV_CAST) + ",\"gold\":" + fmt_f32(EV_GOLD) +
         ",\"gameover\":" + fmt_f32(EV_GAMEOVER) + ",\"wave\":" + fmt_f32(EV_WAVE) + ",\"respawn\":" + fmt_f32(EV_RESPAWN) +
         ",\"attack\":" + fmt_f32(EV_ATTACK) + ",\"item\":" + fmt_f32(EV_ITEM) + ",\"learn\":" + fmt_f32(EV_LEARN) + ",\"blink\":" +
         fmt_f32(EV_BLINK) + ",\"assist\":" + fmt_f32(EV_ASSIST) + "}";

    // names
    s += ",\"kind_names\":";
    name_list(s, std::vector<std::string>{"Hero", c.melee.name, c.ranged.name, c.siege.name, "Neutral", "Tower", "Ancient"});
    s += ",\"neutral_names\":";
    std::vector<std::string> nn;
    for (auto& n : c.neutrals) nn.push_back(n.name);
    name_list(s, nn);

    // map
    s += ",\"bases\":";
    pt_list(s, {BASE_RADIANT, BASE_DIRE});
    s += ",\"lanes\":[";
    for (size_t i = 0; i < g.map.lanes.size(); ++i) {
        if (i > 0) s += ',';
        pt_list(s, g.map.lanes[i]);
    }
    s += "],\"camps\":[";
    for (size_t i = 0; i < g.map.camps.size(); ++i) {
        if (i > 0) s += ',';
        const CampSpawn& cs = g.map.camps[i];
        s += "{\"x\":" + fmt_f32(cs.pos.x) + ",\"y\":" + fmt_f32(cs.pos.y) + ",\"tier\":" + I((unsigned)cs.tier) + "}";
    }
    s += ']';

    // heroes
    s += ",\"heroes\":[";
    for (size_t i = 0; i < c.heroes.size(); ++i) {
        if (i > 0) s += ',';
        const HeroDef& h = c.heroes[i];
        s += "{\"name\":" + quote(h.name) + ",\"title\":" + quote(h.title);
        s += ",\"primary\":" + I((unsigned)h.primary) + ",\"str\":" + fmt_f32(h.str_) + ",\"agi\":" + fmt_f32(h.agi) + ",\"int\":" + fmt_f32(h.int_) +
             ",\"gain_str\":" + fmt_f32(h.gain_str) + ",\"gain_agi\":" + fmt_f32(h.gain_agi) + ",\"gain_int\":" + fmt_f32(h.gain_int) +
             ",\"dmg_min\":" + fmt_f32(h.dmg_min) + ",\"dmg_max\":" + fmt_f32(h.dmg_max) + ",\"range\":" + fmt_f32(h.range) +
             ",\"bat\":" + fmt_f32(h.bat) + ",\"ms\":" + fmt_f32(h.ms) + ",\"proj_speed\":" + fmt_f32(h.proj_speed) +
             ",\"radius\":" + fmt_f32(h.radius) + ",\"glyph\":" + quote(h.glyph) + ",\"color\":" + quote(h.color) + "}";
    }

    // abilities: index = hero * 4 + slot
    s += "],\"abilities\":[";
    for (size_t i = 0; i < c.abilities.size(); ++i) {
        if (i > 0) s += ',';
        const AbilityDef& a = c.abilities[i];
        s += "{\"name\":" + quote(a.name) + ",\"desc\":" + quote(a.desc);
        s += ",\"cast\":" + I((unsigned)(uint8_t)a.cast) + ",\"dtype\":" + I((unsigned)(uint8_t)a.dtype) + ",\"fx\":" + I((unsigned)a.fx) +
             ",\"max_level\":" + I((unsigned)a.max_level) + ",\"speed\":" + fmt_f32(a.speed) + ",\"delay\":" + fmt_f32(a.delay) +
             ",\"aoe_on_land\":" + boolean(a.aoe_on_land) + ",\"icon\":" + quote(a.icon) + ",\"color\":" + quote(a.color);
        float req[4] = {(float)a.req_level[0], (float)a.req_level[1], (float)a.req_level[2], (float)a.req_level[3]};
        const std::pair<const char*, const float*> lists[] = {{"req_level", req},       {"range", a.range.data()}, {"radius", a.radius.data()},
                                                              {"mana", a.mana.data()},  {"cd", a.cd.data()},       {"dmg", a.dmg.data()},
                                                              {"dur", a.dur.data()},    {"pct", a.pct.data()},     {"pct2", a.pct2.data()}};
        for (auto& kv : lists) {
            s += ",\"";
            s += kv.first;
            s += "\":";
            arr(s, kv.second, 4);
        }
        s += '}';
    }

    // items (index 0 = empty slot)
    s += "],\"items\":[";
    for (size_t i = 0; i < c.items.size(); ++i) {
        if (i > 0) s += ',';
        const ItemDef& it = c.items[i];
        s += "{\"name\":" + quote(it.name);
        s += ",\"cost\":" + fmt_f32(it.cost) + ",\"str\":" + fmt_f32(it.str_) + ",\"agi\":" + fmt_f32(it.agi) + ",\"int\":" + fmt_f32(it.int_) +
             ",\"dmg\":" + fmt_f32(it.dmg) + ",\"armor\":" + fmt_f32(it.armor) + ",\"hp\":" + fmt_f32(it.hp) + ",\"mana\":" + fmt_f32(it.mana) +
             ",\"aspd\":" + fmt_f32(it.aspd) + ",\"ms\":" + fmt_f32(it.ms) + ",\"hpreg\":" + fmt_f32(it.hpreg) + ",\"mpreg\":" + fmt_f32(it.mpreg) +
             ",\"active\":" + I((unsigned)it.active) + ",\"cd\":" + fmt_f32(it.cd) + ",\"stack\":" + boolean(it.stack) + ",\"color\":" + quote(it.color) +
             "}";
    }
    s += "]}";
    return s;
}

// ------------------------------------------------------------------------------ mods

struct ModState {
    /// Content after all successfully loaded mods (empty = built-in).
    std::optional<Content> pending;
    /// Shared copy of `pending` handed to matches; rebuilt only when the mods change.
    std::shared_ptr<const Content> active;
    std::string stage, error, exported;
};

ModState& mods() {
    static ModState st;
    return st;
}

/// The content the next match uses.
std::shared_ptr<const Content> active_content() {
    ModState& st = mods();
    if (!st.pending) return builtin();
    if (!st.active) st.active = std::make_shared<const Content>(*st.pending);
    return st.active;
}

Cmd make_cmd(Cmd::K k, Vec2 to = Vec2()) {
    Cmd c{};
    c.k = k;
    c.to = to;
    return c;
}

void command(const Cmd& c0) {
    if (!BRIDGE) return;
    Cmd c = c0;
    c.unit = BRIDGE->game->player;
    BRIDGE->game->push_cmd(c);
}

}  // namespace
}  // namespace moba

using namespace moba;

// ------------------------------------------------------------------------------ exports

MOBA_API uint32_t moba_abi_version() { return ABI_VERSION; }

/// Start (or restart) a match. `hero` picks the player's hero, `per_team` is 1..=5, `fog == 0` is
/// sandbox mode (the player's team sees the whole map), `bot != 0` makes the bot AI drive the
/// player's hero too (watch mode).
MOBA_API void moba_new(uint32_t seed, uint32_t hero, uint32_t per_team, uint32_t fog, uint32_t bot) {
    auto content = active_content();
    Config cfg;
    cfg.seed = seed;
    cfg.player_hero = (uint8_t)(hero % (uint32_t)content->heroes.size());
    cfg.heroes_per_team = (uint8_t)clamp_int<uint32_t>(per_team, 1, 5);
    cfg.fog = fog != 0;
    cfg.player_is_bot = bot != 0;
    auto b = std::make_unique<Bridge>();
    b->game = std::make_unique<Game>(cfg, content);
    b->info = build_info(*b->game);
    snapshot(*b);
    BRIDGE = std::move(b);
}

/// Reserve `len` bytes for a mod file and return their address (0 if too large).
MOBA_API uintptr_t moba_alloc(size_t len) {
    if (len > MAX_STAGE) return 0;
    ModState& st = mods();
    st.stage.assign(std::max<size_t>(len, 1), '\0');
    return (uintptr_t)st.stage.data();
}

/// Apply the first `len` staged bytes as a mod on top of the current content. Returns 0 on success,
/// otherwise the number of error messages (see moba_mod_error_*). A failed mod changes nothing.
MOBA_API uint32_t moba_mod_load(size_t len) {
    ModState& st = mods();
    size_t n = std::min(len, st.stage.size());
    const Content& base = st.pending ? *st.pending : *builtin();
    Content out;
    std::vector<std::string> errors;
    if (apply_mod(base, st.stage.substr(0, n), out, errors)) {
        st.pending = std::move(out);
        st.active.reset();
        st.error.clear();
        return 0;
    }
    st.error.clear();
    for (size_t i = 0; i < errors.size(); ++i) {
        if (i) st.error += '\n';
        st.error += errors[i];
    }
    return (uint32_t)std::max<size_t>(errors.size(), 1);
}

MOBA_API uintptr_t moba_mod_error_ptr() { return (uintptr_t)mods().error.data(); }
MOBA_API size_t moba_mod_error_len() { return mods().error.size(); }

/// Drop all mods: the next match uses the built-in content again.
MOBA_API void moba_mod_clear() {
    ModState& st = mods();
    st.pending.reset();
    st.active.reset();
    st.error.clear();
}

MOBA_API uint32_t moba_mod_count() {
    const ModState& st = mods();
    return st.pending ? (uint32_t)st.pending->mods.size() : 0;
}

/// Serialise the active content as a mod file; returns its length (read it at moba_content_ptr).
MOBA_API size_t moba_content_export() {
    ModState& st = mods();
    st.exported = st.pending ? st.pending->to_mod_json() : builtin()->to_mod_json();
    return st.exported.size();
}
MOBA_API uintptr_t moba_content_ptr() { return (uintptr_t)mods().exported.data(); }

/// Advance the simulation by `n` ticks (1/30 s each).
MOBA_API void moba_step(uint32_t n) {
    if (!BRIDGE) return;
    Game& g = *BRIDGE->game;
    for (uint32_t i = 0; i < std::min<uint32_t>(n, 600); ++i) g.step();
    if (g.events.size() > 2 * MAX_EVENTS) g.events.erase(g.events.begin(), g.events.end() - MAX_EVENTS);
}

/// Rebuild all tables from the current state and drain the event queue.
MOBA_API void moba_snapshot() {
    if (BRIDGE) snapshot(*BRIDGE);
}

#define PTR_GETTER(name, field)                                                    \
    MOBA_API uintptr_t name() { return BRIDGE ? (uintptr_t)BRIDGE->field.data() : 0; }
PTR_GETTER(moba_units_ptr, units)
PTR_GETTER(moba_heroes_ptr, heroes)
PTR_GETTER(moba_projs_ptr, projs)
PTR_GETTER(moba_zones_ptr, zones)
PTR_GETTER(moba_events_ptr, events)
PTR_GETTER(moba_global_ptr, global)
PTR_GETTER(moba_info_ptr, info)
#undef PTR_GETTER

MOBA_API size_t moba_info_len() { return BRIDGE ? BRIDGE->info.size() : 0; }

/// GRID * GRID terrain codes (see map.hpp T_*), row-major.
MOBA_API uintptr_t moba_terrain_ptr() { return BRIDGE ? (uintptr_t)BRIDGE->game->map.terrain.data() : 0; }

/// Low / high 32 bits of the deterministic state hash (desync and replay checks).
MOBA_API uint32_t moba_hash_lo() { return BRIDGE ? (uint32_t)BRIDGE->game->state_hash() : 0; }
MOBA_API uint32_t moba_hash_hi() { return BRIDGE ? (uint32_t)(BRIDGE->game->state_hash() >> 32) : 0; }

// ---- commands for the player's hero (applied at the start of the next tick)

MOBA_API void moba_cmd_move(float x, float y) { command(make_cmd(Cmd::Move, Vec2(x, y))); }
MOBA_API void moba_cmd_attack_move(float x, float y) { command(make_cmd(Cmd::AttackMove, Vec2(x, y))); }
MOBA_API void moba_cmd_attack(uint32_t target) {
    Cmd c = make_cmd(Cmd::Attack);
    c.target = target;
    command(c);
}
MOBA_API void moba_cmd_stop() { command(make_cmd(Cmd::Stop)); }
MOBA_API void moba_cmd_hold() { command(make_cmd(Cmd::Hold)); }
/// `slot` 0..=3 casts an ability, 10..=15 uses an inventory item. `target` is a unit id (0 if none);
/// `x`,`y` is the aimed point.
MOBA_API void moba_cmd_cast(uint32_t slot, uint32_t target, float x, float y) {
    Cmd c{Cmd::Cast, 0, Vec2(x, y)};
    c.slot = (uint8_t)slot;
    c.target = target;
    command(c);
}
MOBA_API void moba_cmd_level_up(uint32_t slot) {
    Cmd c = make_cmd(Cmd::LevelUp);
    c.slot = (uint8_t)slot;
    command(c);
}
MOBA_API void moba_cmd_buy(uint32_t item) {
    Cmd c = make_cmd(Cmd::Buy);
    c.item = (uint8_t)item;
    command(c);
}
MOBA_API void moba_cmd_sell(uint32_t slot) {
    Cmd c = make_cmd(Cmd::Sell);
    c.slot = (uint8_t)slot;
    command(c);
}

/// Hand the player's hero to the bot AI (`on != 0`) or take control back.
MOBA_API void moba_set_bot(uint32_t on) {
    if (!BRIDGE) return;
    Game& g = *BRIDGE->game;
    if (auto i = g.player_idx()) {
        int32_t hi = g.units[*i].hero;
        if (hi >= 0) g.heroes[(size_t)hi].bot = on != 0;
    }
}
