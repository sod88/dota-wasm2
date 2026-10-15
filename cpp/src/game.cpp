#include "game.hpp"

#include <algorithm>
#include <cmath>

namespace moba {

namespace {
constexpr float PI = 3.14159265358979323846264338327950288f;
constexpr float FRAC_PI_4 = 0.785398163397448309615660845819875721f;
}  // namespace

Game::Game(Config c) : Game(c, builtin()) {}

Game::Game(Config c, std::shared_ptr<const Content> ct)
    : cfg(c), content_ptr(std::move(ct)), content(*content_ptr), map(Map::generate()), rng((uint64_t)c.seed) {
    wave_t = content.rules.first_wave_at;
    fog = c.fog;
    for (auto& t : tw_alive)
        for (auto& l : t) l.fill(true);
    build_lane_paths();
    build_structures();
    build_camps();
    build_heroes();
    recompute_tower_flags();
}

// ------------------------------------------------------------------ lookup helpers

std::optional<size_t> Game::idx(uint32_t id) const {
    if (id == 0) return std::nullopt;
    auto it = std::lower_bound(units.begin(), units.end(), id, [](const Unit& u, uint32_t v) { return u.id < v; });
    if (it != units.end() && it->id == id) return (size_t)(it - units.begin());
    return std::nullopt;
}

std::optional<size_t> Game::live_idx(uint32_t id) const {
    auto i = idx(id);
    if (i && units[*i].alive) return i;
    return std::nullopt;
}

bool Game::visible_to(uint8_t team, size_t ui) const {
    // Neutrals see everything; with fog disabled (sandbox mode) the player's team does too.
    if (team > 1 || (!fog && team == player_team)) return true;
    const auto& v = vis[team];
    return ui < v.size() ? (bool)v[ui] : false;
}

// ------------------------------------------------------------------ world setup

void Game::build_lane_paths() {
    for (Lane lane : ALL_LANES) {
        float len = map.lane_length(lane);
        size_t n = (size_t)f2u64(std::ceil(len / 140.0f));
        std::vector<Vec2> pts;
        for (size_t k = 1; k <= n; ++k) pts.push_back(map.lane_point(lane, (float)k / (float)n));
        lane_paths[0][lane_idx(lane)] = pts;
        // Dire walks the same lane backwards, starting from Radiant's side.
        std::reverse(pts.begin(), pts.end());
        std::vector<Vec2> rev(pts.size() > 0 ? pts.begin() + 1 : pts.begin(), pts.end());
        rev.push_back(BASE_RADIANT);
        lane_paths[1][lane_idx(lane)] = rev;
    }
}

void Game::build_structures() {
    for (uint8_t team = 0; team < 2; ++team) {
        // Ancient
        uint32_t id = alloc_id();
        Unit a = Unit::blank(id, Kind::Ancient, team, base_of(team));
        a.radius = content.ancient.radius;
        a.max_hp = content.ancient.hp;
        a.hp = content.ancient.hp;
        a.armor = content.ancient.armor;
        a.vision = content.ancient.vision;
        a.range = 0.0f;
        ancient_ids[team] = id;
        units.push_back(a);
        // Towers: 3 tiers in each of the 3 lanes.
        for (Lane lane : ALL_LANES) {
            for (uint8_t tier = 0; tier < 3; ++tier) {
                const TowerDef& td = content.towers[tier];
                float frac = team == 0 ? td.lane_frac : 1.0f - td.lane_frac;
                Vec2 pos = map.lane_point(lane, frac);
                uint32_t tid = alloc_id();
                Unit t = Unit::blank(tid, Kind::Tower, team, pos);
                t.def = tier;
                t.lane = (uint8_t)lane_idx(lane);
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
                t.order = Order::hold();
                units.push_back(t);
            }
        }
    }
}

void Game::build_camps() {
    for (const CampSpawn& c : map.camps) camps.push_back(Camp{c.pos, c.tier, {}, 0.5f, true});
}

void Game::build_heroes() {
    size_t per = clamp_int<uint8_t>(cfg.heroes_per_team, 1, 5);
    size_t nh = content.heroes.size();
    for (uint8_t team = 0; team < 2; ++team) {
        for (size_t k = 0; k < per; ++k) {
            uint8_t def = team == 0 ? (uint8_t)(((size_t)cfg.player_hero + k) % nh) : (uint8_t)(((size_t)cfg.player_hero + 1 + k) % nh);
            uint8_t lane = LANE_ORDER[k];
            bool is_player = team == 0 && k == 0;
            bool bot = !is_player || cfg.player_is_bot;
            uint32_t uid = spawn_hero(def, team, lane, k, bot);
            if (is_player) {
                player = uid;
                player_team = team;
            }
        }
    }
}

Vec2 Game::hero_spawn_pos(uint8_t team, size_t k) const {
    Vec2 base = base_of(team);
    Vec2 toward = (Vec2(WORLD * 0.5f, WORLD * 0.5f) - base).norm();
    Vec2 side(-toward.y, toward.x);
    return base + toward * 90.0f + side * (((float)k - 2.0f) * 44.0f);
}

uint32_t Game::spawn_hero(uint8_t def, uint8_t team, uint8_t lane, size_t k, bool bot) {
    uint32_t id = alloc_id();
    Vec2 pos = hero_spawn_pos(team, k);
    Unit u = Unit::blank(id, Kind::Hero, team, pos);
    u.def = def;
    u.lane = lane;
    const HeroDef& hd = content.heroes[def];
    u.radius = hd.radius;
    u.range = hd.range;
    u.bat = hd.bat;
    u.proj_speed = hd.proj_speed;
    u.vision = content.rules.hero_vision;
    u.hero = (int32_t)heroes.size();
    u.facing = team == 0 ? -FRAC_PI_4 : PI * 0.75f;
    units.push_back(u);
    heroes.emplace_back(id, def, team, lane, bot, content);
    size_t ui = units.size() - 1;
    recalc_hero(ui, false);
    units[ui].hp = units[ui].max_hp;
    units[ui].mana = units[ui].max_mana;
    return id;
}

// ------------------------------------------------------------------ hero stats

void Game::recalc_hero(size_t ui, bool) {
    int32_t hi = units[ui].hero;
    if (hi < 0) return;
    HeroData& h = heroes[(size_t)hi];
    float level = (float)h.level;
    const HeroDef& d = content.heroes[h.def];
    const Rules& r = content.rules;
    h.base_str = d.str_ + d.gain_str * (level - 1.0f);
    h.base_agi = d.agi + d.gain_agi * (level - 1.0f);
    h.base_int = d.int_ + d.gain_int * (level - 1.0f);
    ItemDef b = h.item_bonus(content);
    float s = h.base_str + b.str_;
    float a = h.base_agi + b.agi;
    float n = h.base_int + b.int_;
    float primary = d.primary == 0 ? s : d.primary == 1 ? a : n;
    Unit& u = units[ui];
    float old_hp = u.max_hp, old_mana = u.max_mana;
    u.max_hp = r.hp_base + r.hp_per_str * s + b.hp;
    u.max_mana = r.mana_base + r.mana_per_int * n + b.mana;
    u.armor = r.armor_base + r.armor_per_agi * a + b.armor;
    u.aspd = a + b.aspd;
    u.move_speed = d.ms + b.ms;
    u.dmg_min = d.dmg_min + primary + b.dmg;
    u.dmg_max = d.dmg_max + primary + b.dmg;
    u.hp_regen = r.hp_regen_base + r.hp_regen_per_str * s + b.hpreg;
    u.mana_regen = r.mana_regen_base + r.mana_regen_per_int * n + b.mpreg;
    if (old_hp > 1.0f) {
        u.hp = fclamp_rs(u.hp + (u.max_hp - old_hp), 1.0f, u.max_hp);
        u.mana = fclamp_rs(u.mana + (u.max_mana - old_mana), 0.0f, u.max_mana);
    }
    u.hp = fmin_rs(u.hp, u.max_hp);
    u.mana = fmin_rs(u.mana, u.max_mana);
}

void Game::add_xp(size_t ui, float xp) {
    int32_t hi = units[ui].hero;
    if (hi < 0 || xp <= 0.0f) return;
    bool leveled = false;
    const auto& xp_table = content.xp_table;
    {
        HeroData& h = heroes[(size_t)hi];
        h.xp += xp;
        while ((size_t)h.level < xp_table.size() && h.xp >= xp_table[h.level]) {
            h.level += 1;
            h.skill_pts += 1;
            leveled = true;
        }
    }
    if (leveled) {
        recalc_hero(ui, true);
        const Unit& u = units[ui];
        emit({EV_LEVEL, (float)u.id, 0.0f, u.pos.x, u.pos.y, (float)heroes[(size_t)hi].level, 0.0f, 0.0f});
    }
}

void Game::add_gold(size_t ui, float amount) {
    int32_t hi = units[ui].hero;
    if (hi < 0 || amount <= 0.0f) return;
    HeroData& h = heroes[(size_t)hi];
    h.gold += amount;
    h.networth += amount;
}

bool Game::learn_ability(size_t ui, size_t slot) {
    int32_t hi = units[ui].hero;
    if (hi < 0 || slot > 3) return false;
    HeroData& h = heroes[(size_t)hi];
    const AbilityDef& def = content.abilities[(size_t)h.def * 4 + slot];
    uint8_t cur = h.ab_level[slot];
    if (h.skill_pts == 0 || cur >= def.max_level || h.level < def.req_level[cur]) return false;
    h.ab_level[slot] += 1;
    h.skill_pts -= 1;
    emit({EV_LEARN, (float)h.unit, (float)slot, 0.0f, 0.0f, (float)h.ab_level[slot], 0.0f, 0.0f});
    return true;
}

bool Game::near_shop(size_t ui) const {
    const Unit& u = units[ui];
    return u.alive && u.pos.dist(base_of(u.team)) <= content.rules.shop_radius;
}

bool Game::buy_item(size_t ui, uint8_t item) {
    int32_t hi = units[ui].hero;
    if (hi < 0 || item == 0 || (size_t)item >= content.items.size() || !near_shop(ui)) return false;
    const ItemDef& it = content.items[item];
    HeroData& h = heroes[(size_t)hi];
    if (h.gold < it.cost) return false;
    // Stack consumables.
    if (it.stack) {
        for (ItemSlot& s : h.items) {
            if (s.id == item && s.charges < 5) {
                s.charges += 1;
                h.gold -= it.cost;
                emit({EV_ITEM, (float)h.unit, (float)item, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f});
                return true;
            }
        }
    }
    int slot = h.free_slot();
    if (slot < 0) return false;
    h.items[(size_t)slot] = ItemSlot{item, (uint8_t)(it.stack ? 1 : 0), 0.0f};
    h.gold -= it.cost;
    uint32_t id = h.unit;
    recalc_hero(ui, true);
    emit({EV_ITEM, (float)id, (float)item, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f});
    return true;
}

bool Game::sell_item(size_t ui, size_t slot) {
    int32_t hi = units[ui].hero;
    if (hi < 0 || slot >= INV_SLOTS || !near_shop(ui)) return false;
    HeroData& h = heroes[(size_t)hi];
    ItemSlot s = h.items[slot];
    if (s.id == 0) return false;
    const ItemDef& it = content.items[s.id];
    float refund = it.cost * 0.5f * (float)std::max<uint8_t>(s.charges, 1);
    h.items[slot] = ItemSlot{};
    h.gold += refund;
    uint32_t id = h.unit;
    recalc_hero(ui, true);
    emit({EV_ITEM, (float)id, (float)s.id, 0.0f, 0.0f, 2.0f, 0.0f, 0.0f});
    return true;
}

// ------------------------------------------------------------------ spawning

uint32_t Game::spawn_creep(const CreepDef& def, uint8_t team, uint8_t lane, Vec2 pos) {
    uint32_t id = alloc_id();
    Unit u = Unit::blank(id, def.kind, team, pos);
    float m = fmin_rs(time / 60.0f, 40.0f);
    float hp_scale = 1.0f + 0.015f * m;
    float dmg_scale = 1.0f + 0.012f * m;
    u.max_hp = def.hp * hp_scale;
    u.hp = u.max_hp;
    u.armor = def.armor + 0.03f * m;
    u.dmg_min = def.dmg_min * dmg_scale;
    u.dmg_max = def.dmg_max * dmg_scale;
    u.range = def.range;
    u.bat = def.bat;
    u.move_speed = def.ms;
    u.proj_speed = def.proj_speed;
    u.radius = def.radius;
    u.vision = def.vision;
    u.bounty_gold = def.gold + 0.25f * m;
    u.bounty_xp = def.xp;
    u.bldg_mult = def.bldg_mult;
    u.lane = lane;
    u.wp = 0;
    const auto& path = lane_paths[team][lane];
    if (!path.empty()) {
        Vec2 p = path.front();
        u.order = Order::attack_move(p);
        u.facing = std::atan2((p - pos).y, (p - pos).x);
    }
    units.push_back(u);
    return id;
}

void Game::spawn_wave() {
    wave_no += 1;
    uint32_t wave = wave_no;
    for (uint8_t team = 0; team < 2; ++team) {
        for (Lane lane : ALL_LANES) {
            float spawn_frac = team == 0 ? 0.10f : 0.90f;
            Vec2 p0 = map.lane_point(lane, spawn_frac);
            Vec2 p1 = map.lane_point(lane, team == 0 ? 0.13f : 0.87f);
            Vec2 dir = (p1 - p0).norm();
            Vec2 side(-dir.y, dir.x);
            const Rules& r = content.rules;
            std::vector<const CreepDef*> comp;
            for (uint32_t k = 0; k < r.wave_melee; ++k) comp.push_back(&content.melee);
            for (uint32_t k = 0; k < r.wave_ranged; ++k) comp.push_back(&content.ranged);
            if (r.siege_every > 0 && wave % r.siege_every == 0)
                for (uint32_t k = 0; k < r.wave_siege; ++k) comp.push_back(&content.siege);
            for (size_t k = 0; k < comp.size(); ++k) {
                float jitter = (rng.f32() - 0.5f) * 20.0f;
                Vec2 pos = p0 - dir * ((float)k * 26.0f) + side * jitter;
                pos = map.nearest_walkable(pos);
                spawn_creep(*comp[k], team, (uint8_t)lane_idx(lane), pos);
            }
        }
    }
    emit({EV_WAVE, (float)wave, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f});
}

void Game::spawn_camp(size_t ci) {
    Vec2 pos = camps[ci].pos;
    size_t tier = camps[ci].tier;
    const auto& comp = content.camp_comp[tier];
    std::vector<uint32_t> ids;
    for (size_t k = 0; k < comp.size(); ++k) {
        uint8_t nd = comp[k];
        const CreepDef& def = content.neutrals[nd];
        float ang = (float)k * 2.1f + 0.5f;
        Vec2 p = map.nearest_walkable(pos + Vec2(std::cos(ang), std::sin(ang)) * 26.0f);
        uint32_t id = alloc_id();
        Unit u = Unit::blank(id, Kind::Neutral, TEAM_NEUTRAL, p);
        float m = fmin_rs(time / 60.0f, 40.0f);
        u.def = nd;
        u.max_hp = def.hp * (1.0f + 0.01f * m);
        u.hp = u.max_hp;
        u.armor = def.armor;
        u.dmg_min = def.dmg_min * (1.0f + 0.01f * m);
        u.dmg_max = def.dmg_max * (1.0f + 0.01f * m);
        u.range = def.range;
        u.bat = def.bat;
        u.move_speed = def.ms;
        u.proj_speed = def.proj_speed;
        u.radius = def.radius;
        u.vision = def.vision;
        u.bounty_gold = def.gold * (1.0f + 0.01f * m);
        u.bounty_xp = def.xp;
        u.home = p;
        u.camp = (int32_t)ci;
        u.order = Order::stay();
        units.push_back(u);
        ids.push_back(id);
    }
    camps[ci].members = ids;
    camps[ci].waiting = false;
}

void Game::spawn_logic() {
    // Passive income (one pulse per second keeps gold integer-friendly for the UI).
    gold_t_ += DT;
    if (gold_t_ >= 1.0f) {
        gold_t_ -= 1.0f;
        for (size_t hi = 0; hi < heroes.size(); ++hi)
            if (auto ui = live_idx(heroes[hi].unit)) add_gold(*ui, content.rules.passive_gold);
    }
    // Lane waves.
    wave_t -= DT;
    if (wave_t <= 0.0f) {
        wave_t += content.rules.wave_interval;
        spawn_wave();
    }
    // Jungle camps.
    for (size_t ci = 0; ci < camps.size(); ++ci) {
        std::vector<uint32_t> members = std::move(camps[ci].members);
        members.erase(std::remove_if(members.begin(), members.end(), [&](uint32_t id) { return !live_idx(id); }), members.end());
        bool empty = members.empty();
        camps[ci].members = std::move(members);
        if (empty) {
            if (!camps[ci].waiting) {
                camps[ci].waiting = true;
                camps[ci].respawn_t = content.rules.camp_respawn;
            }
            camps[ci].respawn_t -= DT;
            if (camps[ci].respawn_t <= 0.0f) spawn_camp(ci);
        }
    }
    // Hero respawns.
    for (size_t hi = 0; hi < heroes.size(); ++hi) {
        uint32_t id = heroes[hi].unit;
        auto oi = idx(id);
        if (!oi) continue;
        size_t ui = *oi;
        if (units[ui].alive) continue;
        heroes[hi].respawn_t -= DT;
        if (heroes[hi].respawn_t <= 0.0f) {
            uint8_t team = units[ui].team;
            size_t k = hi % (size_t)clamp_int<uint8_t>(cfg.heroes_per_team, 1, 5);
            Vec2 pos = hero_spawn_pos(team, k);
            Unit& u = units[ui];
            u.alive = true;
            u.pos = pos;
            u.hp = u.max_hp;
            u.mana = u.max_mana;
            u.order = Order::idle();
            u.target = 0;
            u.swing_t = 0.0f;
            u.stun_t = 0.0f;
            u.root_t = 0.0f;
            u.slow_t = 0.0f;
            u.haste_t = 0.0f;
            u.aspd_t = 0.0f;
            u.salve_t = 0.0f;
            u.clear_path();
            heroes[hi].ai_state = 0;
            emit({EV_RESPAWN, (float)id, 0.0f, pos.x, pos.y, 0.0f, 0.0f, 0.0f});
        }
    }
}

// ------------------------------------------------------------------ vision & flags

void Game::recompute_tower_flags() {
    std::array<std::array<std::array<bool, 3>, 3>, 2> f{};
    for (const Unit& u : units)
        if (u.alive && u.kind == Kind::Tower) f[u.team][u.lane][u.def] = true;
    tw_alive = f;
}

bool Game::building_protected(size_t ui) const {
    const Unit& u = units[ui];
    size_t t = u.team;
    switch (u.kind) {
        case Kind::Tower: return u.def > 0 && tw_alive[t][u.lane][u.def - 1];
        case Kind::Ancient: return tw_alive[t][0][2] && tw_alive[t][1][2] && tw_alive[t][2][2];
        default: return false;
    }
}

void Game::compute_vision() {
    size_t n = units.size();
    for (auto& v : vis) {
        v.clear();
        v.resize(n, false);
    }
    for (size_t a = 0; a < n; ++a) {
        const Unit& ua = units[a];
        if (!ua.alive || ua.team > 1) continue;
        size_t t = ua.team;
        float r2 = ua.vision * ua.vision;
        Vec2 pa = ua.pos;
        for (size_t b = 0; b < n; ++b) {
            if (vis[t][b]) continue;
            const Unit& ub = units[b];
            if (ub.alive && pa.dist_sq(ub.pos) <= r2) vis[t][b] = true;
        }
    }
}

// ------------------------------------------------------------------ main loop

void Game::step() {
    if (winner >= 0) return;
    apply_commands();
    time += DT;
    tick += 1;
    recompute_tower_flags();
    compute_vision();
    spawn_logic();
    size_t n = units.size();
    for (size_t i = 0; i < n; ++i)
        if (units[i].alive) update_timers(i);
    for (size_t i = 0; i < n; ++i) {
        if (!units[i].alive) continue;
        think(i);
        if (units[i].alive) act(i);
    }
    update_projectiles();
    update_zones();
    separate_units();
    cleanup();
}

void Game::cleanup() {
    units.erase(std::remove_if(units.begin(), units.end(), [](const Unit& u) { return !(u.alive || u.kind == Kind::Hero); }), units.end());
}

// ------------------------------------------------------------------ commands

std::optional<size_t> Game::controllable(uint32_t id) const {
    auto i = live_idx(id);
    if (i && units[*i].is_hero()) return i;
    return std::nullopt;
}

Vec2 Game::clamp_world(Vec2 p) const { return Vec2(fclamp_rs(p.x, 1.0f, WORLD - 1.0f), fclamp_rs(p.y, 1.0f, WORLD - 1.0f)); }

void Game::apply_commands() {
    std::vector<Cmd> list = std::move(cmds);
    cmds.clear();
    for (const Cmd& c : list) {
        auto i = controllable(c.unit);
        if (!i) continue;
        switch (c.k) {
            case Cmd::Move: set_order(*i, Order::move(clamp_world(c.to))); break;
            case Cmd::AttackMove: set_order(*i, Order::attack_move(clamp_world(c.to))); break;
            case Cmd::Attack:
                if (auto t = live_idx(c.target))
                    if (can_attack(*i, *t)) set_order(*i, Order::attack(c.target));
                break;
            case Cmd::Stop: set_order(*i, Order::idle()); break;
            case Cmd::Hold: set_order(*i, Order::hold()); break;
            case Cmd::Cast: order_cast(*i, c.slot, c.target, c.to); break;
            case Cmd::LevelUp: learn_ability(*i, c.slot); break;
            case Cmd::Buy: buy_item(*i, c.item); break;
            case Cmd::Sell: sell_item(*i, c.slot); break;
        }
    }
}

void Game::set_order(size_t i, Order o) {
    Unit& u = units[i];
    u.order = o;
    u.target = 0;
    u.swing_t = 0.0f;  // issuing a new order cancels a wind-up (animation cancel)
    u.clear_path();
}

// ------------------------------------------------------------------ introspection

uint64_t Game::state_hash() const {
    uint64_t h = 0xcbf29ce484222325ull;
    auto mix = [&](uint64_t v) {
        h ^= v;
        h *= 0x100000001b3ull;
    };
    mix(tick);
    for (const Unit& u : units) {
        mix(u.id);
        mix(float_bits(u.pos.x));
        mix(float_bits(u.pos.y));
        mix(float_bits(u.hp));
        mix(u.alive ? 1 : 0);
    }
    for (const HeroData& hd : heroes) {
        mix(float_bits(hd.gold));
        mix(float_bits(hd.xp));
        mix(hd.kills);
    }
    return h;
}

size_t Game::count_kind(uint8_t team, Kind kind) const {
    return (size_t)std::count_if(units.begin(), units.end(), [&](const Unit& u) { return u.alive && u.team == team && u.kind == kind; });
}

}  // namespace moba
