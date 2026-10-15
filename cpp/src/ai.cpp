// Computer-controlled units: lane creeps, jungle neutrals, towers and hero bots (port of src/ai.rs).
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <tuple>

#include "game.hpp"

namespace moba {

namespace {
constexpr uint8_t AI_STATE_LANE = 0;
constexpr uint8_t AI_STATE_RETREAT = 1;
constexpr uint8_t AI_STATE_SHOP = 2;
}  // namespace

void Game::think(size_t i) {
    switch (units[i].kind) {
        case Kind::Melee:
        case Kind::Ranged:
        case Kind::Siege: think_lane_creep(i); break;
        case Kind::Neutral: think_neutral(i); break;
        case Kind::Tower: think_tower(i); break;
        case Kind::Hero:
            if (heroes[(size_t)units[i].hero].bot) think_bot(i);
            break;
        case Kind::Ancient: break;
    }
}

// =================================================================== lane creeps

void Game::think_lane_creep(size_t i) {
    size_t team = units[i].team, lane = units[i].lane;
    Vec2 pos = units[i].pos;
    const auto& path = lane_paths[team][lane];
    if (path.empty()) return;
    size_t last = path.size() - 1;
    size_t wp = std::min(units[i].wp, last);
    while (wp < last && pos.dist(path[wp]) < 60.0f) wp += 1;
    Vec2 goal = path[wp];
    Unit& u = units[i];
    u.wp = wp;
    u.order = Order::attack_move(goal);
}

// =================================================================== neutrals

void Game::think_neutral(size_t i) {
    Vec2 home = units[i].home, pos = units[i].pos;
    uint32_t tid = units[i].target;
    bool returning = units[i].returning;
    if (returning) {
        Unit& u = units[i];
        u.hp = fmin_rs(u.hp + u.max_hp * 0.10f * DT, u.max_hp);
        if (pos.dist(home) < 14.0f) {
            u.returning = false;
            u.target = 0;
            u.order = Order::stay();
            u.clear_path();
        } else {
            u.target = 0;
            u.order = Order::move(home);
        }
        return;
    }
    if (tid != 0) {
        bool leash_ok = pos.dist(home) <= 480.0f;
        if (leash_ok && live_idx(tid) && target_ok(i, tid, FLT_MAX)) {
            units[i].order = Order::attack(tid);
        } else {
            Unit& u = units[i];
            u.returning = true;
            u.target = 0;
            u.order = Order::move(home);
            u.clear_path();
        }
    } else {
        units[i].order = Order::stay();
    }
}

// =================================================================== towers

void Game::think_tower(size_t i) {
    Vec2 pos = units[i].pos;
    float range = units[i].range;
    uint8_t team = units[i].team;
    uint32_t force = units[i].force_target;
    float force_t = units[i].force_t;
    // 1) A hero that just hit one of our heroes under the tower gets priority.
    if (force != 0 && force_t > 0.0f) {
        if (auto f = live_idx(force)) {
            const Unit& t = units[*f];
            if (t.pos.dist(pos) - t.radius <= range && target_ok(i, force, range)) {
                units[i].target = force;
                units[i].order = Order::attack(force);
                return;
            }
        }
    }
    // 2) Otherwise creeps first, then heroes; nearest within each class.
    uint8_t best_class = 9;
    float best_d = FLT_MAX;
    uint32_t best_id = 0;
    for (size_t j = 0; j < units.size(); ++j) {
        const Unit& t = units[j];
        if (!t.alive || t.team == team || t.team == TEAM_NEUTRAL || t.is_building()) continue;
        float d = t.pos.dist(pos) - t.radius;
        if (d > range) continue;
        uint8_t cls = t.is_hero() ? 1 : 0;
        bool better = cls < best_class || (cls == best_class && d < best_d);  // Rust tuple `<`
        if (better && visible_to(team, j)) {
            best_class = cls;
            best_d = d;
            best_id = t.id;
        }
    }
    Unit& u = units[i];
    u.target = best_id;
    u.order = best_id != 0 ? Order::attack(best_id) : Order::stay();
}

// =================================================================== hero bot

void Game::bot_level_skills(size_t ui) {
    size_t hi = (size_t)units[ui].hero;
    while (heroes[hi].skill_pts > 0) {
        if (learn_ability(ui, 3)) continue;
        const auto& order = content.heroes[heroes[hi].def].skill_order;
        std::vector<std::tuple<uint8_t, size_t, size_t>> cands;
        for (size_t pri = 0; pri < order.size(); ++pri) cands.emplace_back(heroes[hi].ab_level[order[pri]], pri, (size_t)order[pri]);
        std::sort(cands.begin(), cands.end());
        bool learned = false;
        for (auto& c : cands) {
            if (learn_ability(ui, std::get<2>(c))) {
                learned = true;
                break;
            }
        }
        if (!learned) break;
    }
}

void Game::bot_shop(size_t ui) {
    size_t hi = (size_t)units[ui].hero;
    const auto& build = content.heroes[heroes[hi].def].build;
    // Opening: one healing salve if we can afford it alongside the first item.
    uint8_t salve = 0;
    for (size_t k = 0; k < content.items.size(); ++k) {
        if (content.items[k].active == ACT_SALVE) {
            salve = (uint8_t)k;
            break;
        }
    }
    bool have_salve = std::any_of(heroes[hi].items.begin(), heroes[hi].items.end(), [&](const ItemSlot& s) { return s.id == salve; });
    if (heroes[hi].build_i == 0 && !build.empty() && salve != 0 && !have_salve) {
        float first = content.items[build[0]].cost;
        if (heroes[hi].gold >= first + content.items[salve].cost) buy_item(ui, salve);
    }
    while (heroes[hi].build_i < build.size()) {
        uint8_t item = build[heroes[hi].build_i];
        if (heroes[hi].gold >= content.items[item].cost && buy_item(ui, item)) heroes[hi].build_i += 1;
        else break;
    }
    heroes[hi].shop_cd = 25.0f;
}

std::optional<float> Game::next_item_cost(size_t hi) const {
    const auto& build = content.heroes[heroes[hi].def].build;
    const HeroData& h = heroes[hi];
    if (h.build_i >= build.size() || h.free_slot() < 0) return std::nullopt;
    return content.items[build[h.build_i]].cost;
}

Vec2 Game::lane_hold_pos(uint8_t team, size_t lane) const {
    Vec2 enemy_base = base_of((uint8_t)(1 - team));
    Vec2 own_base = base_of(team);
    bool have = false;
    float bd = 0.0f;
    Vec2 front;
    for (const Unit& u : units) {
        if (u.alive && u.team == team && is_lane_creep(u.kind) && (size_t)u.lane == lane) {
            float d = u.pos.dist(enemy_base);
            if (!have || d < bd) {
                have = true;
                bd = d;
                front = u.pos;
            }
        }
    }
    Lane l = ALL_LANES[lane];
    Vec2 p = have ? front : map.lane_point(l, team == 0 ? 0.32f : 0.68f);
    Vec2 back = (own_base - p).norm();
    return map.nearest_walkable(p + back * 70.0f);
}

std::optional<size_t> Game::tower_threat_at(uint8_t team, Vec2 p) const {
    for (size_t j = 0; j < units.size(); ++j) {
        const Unit& t = units[j];
        if (t.alive && t.kind == Kind::Tower && t.team != team && t.pos.dist(p) <= t.range + 40.0f) {
            // Towers prefer creeps: allied creeps inside the tower's range soak the shots.
            bool covered = std::any_of(units.begin(), units.end(), [&](const Unit& c) {
                return c.alive && c.team == team && is_lane_creep(c.kind) && c.pos.dist(t.pos) <= t.range + 10.0f;
            });
            if (!covered) return j;
        }
    }
    return std::nullopt;
}

void Game::think_bot(size_t ui) {
    size_t hi = (size_t)units[ui].hero;
    heroes[hi].think_t -= DT;
    heroes[hi].shop_cd = fmax_rs(heroes[hi].shop_cd - DT, 0.0f);
    if (heroes[hi].think_t > 0.0f) return;
    heroes[hi].think_t = 0.2f + rng.f32() * 0.1f;

    uint8_t team = units[ui].team;
    Vec2 pos = units[ui].pos;
    Vec2 base = base_of(team);
    size_t lane = heroes[hi].lane;

    bot_level_skills(ui);
    if (near_shop(ui)) bot_shop(ui);

    float hp_frac, mana_frac, range, my_dmg;
    {
        const Unit& u = units[ui];
        hp_frac = u.hp / u.max_hp;
        mana_frac = u.mana / fmax_rs(u.max_mana, 1.0f);
        range = u.range;
        my_dmg = u.avg_damage();
    }

    // --- perceive
    std::vector<size_t> enemy_heroes, enemy_creeps, ally_creeps;
    size_t ally_heroes = 0;
    for (size_t j = 0; j < units.size(); ++j) {
        const Unit& v = units[j];
        if (!v.alive || j == ui) continue;
        float d = pos.dist(v.pos);
        if (d > 900.0f) continue;
        if (v.team == team) {
            if (is_lane_creep(v.kind)) ally_creeps.push_back(j);
            else if (v.is_hero()) ally_heroes += 1;
        } else if (v.team != TEAM_NEUTRAL && visible_to(team, j)) {
            if (v.is_hero()) enemy_heroes.push_back(j);
            else if (is_lane_creep(v.kind)) enemy_creeps.push_back(j);
        }
    }
    std::stable_sort(enemy_heroes.begin(), enemy_heroes.end(), [&](size_t a, size_t b) { return pos.dist_sq(units[a].pos) < pos.dist_sq(units[b].pos); });

    uint8_t state = heroes[hi].ai_state;

    // --- state: retreating to the fountain
    if (state == AI_STATE_RETREAT) {
        bool at_base = pos.dist(base) <= content.rules.shop_radius * 0.8f;
        if (at_base) {
            if (hp_frac > 0.9f && mana_frac > 0.55f) heroes[hi].ai_state = AI_STATE_LANE;
            else units[ui].order = Order::stay();
            return;
        }
        bot_escape(ui, enemy_heroes, base);
        units[ui].order = Order::move(base);
        return;
    }

    // --- state: walking home to spend gold
    if (state == AI_STATE_SHOP) {
        if (near_shop(ui)) heroes[hi].ai_state = AI_STATE_LANE;
        else if (hp_frac < 0.3f && !enemy_heroes.empty()) heroes[hi].ai_state = AI_STATE_RETREAT;
        else units[ui].order = Order::move(base);
        return;
    }

    // --- lane state: decide whether to bail out
    float strongest_enemy_frac = 0.0f;
    for (size_t e : enemy_heroes) strongest_enemy_frac = fmax_rs(strongest_enemy_frac, units[e].hp / units[e].max_hp);
    if (hp_frac < 0.33f || (hp_frac < 0.55f && !enemy_heroes.empty() && strongest_enemy_frac > hp_frac + 0.1f)) {
        heroes[hi].ai_state = AI_STATE_RETREAT;
        bot_escape(ui, enemy_heroes, base);
        units[ui].order = Order::move(base);
        return;
    }
    if (auto cost = next_item_cost(hi)) {
        if (heroes[hi].gold >= *cost + 60.0f && heroes[hi].shop_cd <= 0.0f && enemy_heroes.empty()) {
            heroes[hi].ai_state = AI_STATE_SHOP;
            units[ui].order = Order::move(base);
            return;
        }
    }

    // Consumables.
    bot_use_consumables(ui, hp_frac, mana_frac, !enemy_heroes.empty());

    // --- tower danger: step back out of range when there is no creep cover
    if (auto tj = tower_threat_at(team, pos)) {
        Vec2 tp = units[*tj].pos;
        Vec2 away = (pos - tp).norm();
        Vec2 dest = map.nearest_walkable(tp + away * (units[*tj].range + 120.0f));
        units[ui].order = Order::move(dest);
        return;
    }

    // --- fight: trade with / chase enemy heroes when it looks favourable
    if (!enemy_heroes.empty()) {
        size_t target = enemy_heroes.front();
        uint32_t tid = units[target].id;
        Vec2 tpos = units[target].pos;
        float thp_frac = units[target].hp / units[target].max_hp;
        bool covered_dive = !tower_threat_at(team, tpos);
        bool outnumbered = enemy_heroes.size() > ally_heroes + 1;
        float dist = pos.dist(tpos);
        float reach = fmax_rs(range, best_ability_range(ui)) + 140.0f;
        if (covered_dive && !outnumbered && dist <= reach && (hp_frac > 0.5f || thp_frac < 0.25f)) {
            bot_fight_abilities(ui, target, enemy_heroes);
            if (units[ui].order != Order::attack(tid)) {
                units[ui].target = tid;
                units[ui].order = Order::attack(tid);
            }
            return;
        }
    }

    // --- last hitting & denying
    // Judge each creep by the hp it will have when our hit would land, not by its hp now:
    // a melee hero has to walk up and wind up, and the creeps keep hitting in the meantime.
    auto reach_of = [&](size_t e) { return units[ui].range + units[e].radius + units[ui].radius * 0.5f + 35.0f; };
    bool has_lh = false, has_soft = false, has_stalk = false;
    float lh_hp = 0.0f, soft_hp = 0.0f, stalk_hp = 0.0f;
    uint32_t lh_id = 0, soft_id = 0;
    size_t stalk = 0;
    for (size_t e : enemy_creeps) {
        const Unit& v = units[e];
        float vhp = v.hp;
        uint32_t vid = v.id;
        Vec2 vpos = v.pos;
        float exp = my_dmg * armor_multiplier(v.armor);
        float d = pos.dist(vpos);
        float t_hit = bot_hit_time(ui, e);
        float hp_at_hit = vhp - incoming_damage(e, ui, t_hit);
        if (d <= reach_of(e) + 140.0f && hp_at_hit <= exp * 1.05f && hp_at_hit > 0.0f) {
            if (!has_lh || vhp < lh_hp) {
                has_lh = true;
                lh_hp = vhp;
                lh_id = vid;
            }
        } else if (d <= reach_of(e) && vhp <= exp * 2.3f) {
            // Only chip at a creep when our next swing will still be in time for the kill.
            float at = units[ui].attack_time();
            float after = vhp - exp - incoming_damage(e, ui, t_hit + at);
            if (after > 0.0f && (!has_soft || vhp < soft_hp)) {
                has_soft = true;
                soft_hp = vhp;
                soft_id = vid;
            }
        }
        if (d <= 520.0f && !tower_threat_at(team, vpos)) {
            float soon = vhp - incoming_damage(e, ui, 2.0f);
            if (!has_stalk || soon < stalk_hp) {
                has_stalk = true;
                stalk_hp = soon;
                stalk = e;
            }
        }
    }
    bool has_deny = false;
    uint32_t deny = 0;
    for (size_t a : ally_creeps) {
        const Unit& v = units[a];
        if (v.hp < v.max_hp * 0.5f && pos.dist(v.pos) <= reach_of(a)) {
            float exp = my_dmg * armor_multiplier(v.armor);
            if (v.hp <= exp * 1.05f && rng.f32() < 0.6f) {
                has_deny = true;
                deny = v.id;
            }
        }
    }
    if (has_lh || has_deny || has_soft) {
        uint32_t t = has_lh ? lh_id : has_deny ? deny : soft_id;
        units[ui].target = t;
        units[ui].order = Order::attack(t);
        return;
    }

    // Harass: poke a hero standing within reach when it is safe.
    if (!enemy_heroes.empty()) {
        size_t e = enemy_heroes.front();
        uint32_t tid = units[e].id;
        Vec2 tpos = units[e].pos;
        if (pos.dist(tpos) <= range + 60.0f && !tower_threat_at(team, tpos) && hp_frac > 0.6f) {
            units[ui].target = tid;
            units[ui].order = Order::attack(tid);
            return;
        }
    }

    // --- push a tower together with the creep wave
    if (enemy_heroes.empty()) {
        bool have = false;
        float bd = 0.0f;
        size_t tj = 0;
        for (size_t j = 0; j < units.size(); ++j) {
            const Unit& t = units[j];
            if (t.alive && t.is_building() && t.team != team && !building_protected(j)) {
                float d = t.pos.dist(pos);
                if (d < 800.0f && (!have || d < bd)) {
                    have = true;
                    bd = d;
                    tj = j;
                }
            }
        }
        if (have) {
            uint32_t tid = units[tj].id;
            Vec2 tpos = units[tj].pos;
            float trange = units[tj].range;
            size_t cover = (size_t)std::count_if(ally_creeps.begin(), ally_creeps.end(), [&](size_t c) { return units[c].pos.dist(tpos) <= trange + 80.0f; });
            if (cover >= 2 && enemy_creeps.size() < 2) {
                units[ui].target = tid;
                units[ui].order = Order::attack(tid);
                return;
            }
        }
    }

    // --- wait for a last hit, or walk to the lane front
    if (!enemy_creeps.empty() &&
        std::any_of(enemy_creeps.begin(), enemy_creeps.end(), [&](size_t e) { return pos.dist(units[e].pos) < range + 220.0f; })) {
        // Stand within reach of the creep that will die first, so the kill is one swing away.
        units[ui].target = 0;
        units[ui].order = Order::stay();
        if (has_stalk) {
            Vec2 ep = units[stalk].pos;
            float want = reach_of(stalk) - 45.0f;
            if (pos.dist(ep) > want) {
                Vec2 dest = map.nearest_walkable(ep + (pos - ep).norm() * want);
                units[ui].order = Order::move(dest);
            }
        }
        return;
    }
    Vec2 hold = lane_hold_pos(team, lane);
    units[ui].target = 0;
    units[ui].order = Order::attack_move(hold);
}

/// Seconds until a hit from bot `ui` on unit `ti` would land, starting now.
float Game::bot_hit_time(size_t ui, size_t ti) const {
    const Unit& u = units[ui];
    const Unit& t = units[ti];
    float d = u.pos.dist(t.pos);
    float reach = u.range + t.radius + u.radius * 0.5f;
    float walk = fmax_rs(d - reach, 0.0f) / fmax_rs(u.speed(), 1.0f);
    float at = u.attack_time();
    float time = fmax_rs(fmax_rs(walk, u.attack_cd), u.swing_t) + fmax_rs(at * 0.30f, 0.1f);
    if (u.proj_speed > 0.0f) time += fmin_rs(d, reach) / u.proj_speed;
    return time;
}

/// Damage other units will deal to `ti` within the next `t` seconds (projectiles in flight
/// plus the swings of everything attacking it), ignoring unit `skip`.
float Game::incoming_damage(size_t ti, size_t skip, float t) const {
    uint32_t tid = units[ti].id;
    Vec2 tpos = units[ti].pos;
    uint8_t tteam = units[ti].team;
    float mult = armor_multiplier(units[ti].armor);
    uint32_t skip_id = units[skip].id;
    float total = 0.0f;
    for (const Proj& p : projs) {
        if (p.homing == tid && p.src != skip_id && p.ability == 255 && p.pos.dist(tpos) <= p.speed * t) total += p.dmg * mult;
    }
    for (size_t j = 0; j < units.size(); ++j) {
        const Unit& a = units[j];
        if (j == skip || !a.alive || a.team == tteam || a.dmg_max <= 0.0f) continue;
        bool swinging = a.swing_t > 0.0f && a.swing_target == tid;
        if (!swinging && (a.target != tid || !in_attack_range(j, ti))) continue;
        float at = a.attack_time();
        float travel = a.proj_speed > 0.0f ? a.pos.dist(tpos) / a.proj_speed : 0.0f;
        float hit = (swinging ? a.swing_t : a.attack_cd + fmax_rs(at * 0.30f, 0.1f)) + travel;
        int n = 0;
        while (hit <= t && n < 8) {
            total += a.avg_damage() * mult;
            hit += at;
            ++n;
        }
    }
    return total;
}

float Game::best_ability_range(size_t ui) const {
    float r = 0.0f;
    for (size_t slot = 0; slot < 4; ++slot) {
        size_t l = 0;
        const AbilityDef* ab = ability_def(ui, slot, l);
        if (!ab || !ability_ready(ui, slot)) continue;
        float cr;
        switch (ab->cast) {
            case Cast::Target:
            case Cast::Line:
            case Cast::PointAoE: cr = ab->range[l]; break;
            case Cast::SelfAoE: cr = ab->radius[l]; break;
            case Cast::Blink: cr = ab->aoe_on_land ? ab->range[l] + ab->radius[l] * 0.5f : 0.0f; break;
            default: cr = 0.0f; break;
        }
        r = fmax_rs(r, cr);
    }
    return r;
}

void Game::bot_use_consumables(size_t ui, float hp_frac, float mana_frac, bool enemy_near) {
    size_t hi = (size_t)units[ui].hero;
    for (size_t slot = 0; slot < INV_SLOTS; ++slot) {
        ItemSlot s = heroes[hi].items[slot];
        if (s.id == 0 || s.cd > 0.0f) continue;
        uint8_t active = content.items[s.id].active;
        if (active == ACT_SALVE && hp_frac < 0.65f && !enemy_near && units[ui].salve_t <= 0.0f) use_item(ui, slot, Vec2());
        else if (active == ACT_MANA && mana_frac < 0.3f && units[ui].mana_pot_t <= 0.0f) use_item(ui, slot, Vec2());
    }
}

void Game::bot_escape(size_t ui, const std::vector<size_t>& enemy_heroes, Vec2 base) {
    Vec2 pos = units[ui].pos;
    bool danger = std::any_of(enemy_heroes.begin(), enemy_heroes.end(), [&](size_t e) { return units[e].pos.dist(pos) < 500.0f; });
    size_t hi = (size_t)units[ui].hero;
    if (danger) {
        for (size_t slot = 0; slot < 4; ++slot) {
            size_t l = 0;
            const AbilityDef* ab = ability_def(ui, slot, l);
            if (!ab) continue;
            if (!ability_ready(ui, slot)) continue;
            bool fire = ab->cast == Cast::Blink ? !ab->aoe_on_land : ab->cast == Cast::SelfBuff ? (ab->fx & FX_HASTE) != 0 : false;
            if (fire && cast_ability(ui, slot, 0, base)) return;
        }
        for (size_t slot = 0; slot < INV_SLOTS; ++slot) {
            ItemSlot s = heroes[hi].items[slot];
            if (s.id != 0 && s.cd <= 0.0f && content.items[s.id].active == ACT_BLINK && use_item(ui, slot, base)) return;
        }
    }
    float hp_frac = units[ui].hp / units[ui].max_hp;
    bot_use_consumables(ui, hp_frac, units[ui].mana / fmax_rs(units[ui].max_mana, 1.0f), danger);
}

void Game::bot_fight_abilities(size_t ui, size_t target, const std::vector<size_t>& enemy_heroes) {
    Vec2 pos = units[ui].pos;
    const Unit& tu = units[target];
    uint32_t tid = tu.id;
    Vec2 tpos = tu.pos;
    float tr = tu.radius, tstun = tu.stun_t, troot = tu.root_t, tslow = tu.slow_t, tface = tu.facing;
    float dist = pos.dist(tpos);
    float hp_frac = units[ui].hp / units[ui].max_hp;
    for (size_t slot = 0; slot < 4; ++slot) {
        if (!ability_ready(ui, slot)) continue;
        size_t l = 0;
        const AbilityDef& ab = *ability_def(ui, slot, l);
        float range = ab.range[l];
        float radius = ab.radius[l];
        bool fired = false;
        switch (ab.cast) {
            case Cast::Target: fired = dist <= range + tr && cast_ability(ui, slot, tid, tpos); break;
            case Cast::Line: fired = dist <= range * 0.92f && cast_ability(ui, slot, 0, tpos); break;
            case Cast::PointAoE:
                if (dist > range) {
                    fired = false;
                } else if (ab.delay > 0.0f) {
                    // Telegraphed spells: only when the target is locked down or slowed, otherwise lead a walking target.
                    bool locked = tstun > ab.delay * 0.6f || troot > ab.delay * 0.6f;
                    Vec2 lead = locked ? Vec2() : Vec2(std::cos(tface), std::sin(tface)) * (units[target].speed() * ab.delay * 0.6f);
                    fired = (locked || tslow > 0.0f || rng.f32() < 0.35f) && cast_ability(ui, slot, 0, tpos + lead);
                } else {
                    fired = cast_ability(ui, slot, 0, tpos);
                }
                break;
            case Cast::SelfAoE: fired = dist <= radius * 0.95f && cast_ability(ui, slot, 0, pos); break;
            case Cast::SelfBuff: fired = dist <= 520.0f && (ab.fx & FX_ASPD) != 0 && cast_ability(ui, slot, 0, pos); break;
            case Cast::Blink:
                fired = ab.aoe_on_land && dist > 160.0f && dist <= range + radius * 0.5f && hp_frac > 0.5f && enemy_heroes.size() >= 1 &&
                        cast_ability(ui, slot, 0, tpos);
                break;
            case Cast::None: fired = false; break;
        }
        if (fired) return;
    }
    // Sprint-style speed buffs help chase a fleeing, wounded hero.
    if (dist > 260.0f) {
        for (size_t slot = 0; slot < 4; ++slot) {
            size_t l = 0;
            const AbilityDef* ab = ability_def(ui, slot, l);
            if (ab && ab->cast == Cast::SelfBuff && (ab->fx & FX_HASTE) && ability_ready(ui, slot)) {
                cast_ability(ui, slot, 0, pos);
                return;
            }
        }
    }
}

}  // namespace moba
