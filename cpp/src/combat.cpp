// Per-unit simulation: timers, order execution, movement, attacks, damage and deaths (port of src/combat.rs).
#include <algorithm>
#include <cfloat>
#include <cmath>

#include "game.hpp"

namespace moba {

namespace {
float mass(const Unit& u) {
    switch (u.kind) {
        case Kind::Hero: return 3.0f;
        case Kind::Neutral: return 2.0f;
        default: return 1.0f;
    }
}
}  // namespace

// =================================================================== timers & regen

void Game::update_timers(size_t i) {
    bool near_fountain;
    {
        const Unit& u = units[i];
        near_fountain = u.is_hero() && u.pos.dist(base_of(u.team)) <= content.rules.shop_radius;
    }
    {
        Unit& u = units[i];
        u.attack_cd = fmax_rs(u.attack_cd - DT, 0.0f);
        u.since_dmg += DT;
        u.repath_t = fmax_rs(u.repath_t - DT, 0.0f);
        u.stun_t = fmax_rs(u.stun_t - DT, 0.0f);
        u.root_t = fmax_rs(u.root_t - DT, 0.0f);
        u.slow_t = fmax_rs(u.slow_t - DT, 0.0f);
        u.haste_t = fmax_rs(u.haste_t - DT, 0.0f);
        u.aspd_t = fmax_rs(u.aspd_t - DT, 0.0f);
        u.force_t = fmax_rs(u.force_t - DT, 0.0f);

        float hp_gain = u.hp_regen;
        float mana_gain = u.mana_regen;
        if (u.salve_t > 0.0f) {
            u.salve_t -= DT;
            hp_gain += u.salve_rate;
        }
        if (u.mana_pot_t > 0.0f) {
            u.mana_pot_t -= DT;
            mana_gain += u.mana_pot_rate;
        }
        if (near_fountain) {
            hp_gain += u.max_hp * 0.08f;
            mana_gain += u.max_mana * 0.10f;
        }
        if (u.kind == Kind::Ancient && u.since_dmg > 5.0f) hp_gain += 12.0f;
        u.hp = fmin_rs(u.hp + hp_gain * DT, u.max_hp);
        u.mana = fmin_rs(u.mana + mana_gain * DT, u.max_mana);
    }
    int32_t hi = units[i].hero;
    if (hi >= 0) {
        HeroData& h = heroes[(size_t)hi];
        for (float& c : h.ab_cd) c = fmax_rs(c - DT, 0.0f);
        for (ItemSlot& s : h.items) s.cd = fmax_rs(s.cd - DT, 0.0f);
        for (auto& r : h.recent) r.second += DT;
        h.recent.erase(std::remove_if(h.recent.begin(), h.recent.end(), [](const std::pair<uint32_t, float>& r) { return !(r.second < 15.0f); }),
                       h.recent.end());
    }
}

// =================================================================== target rules

bool Game::can_attack(size_t i, size_t ti) const {
    const Unit& a = units[i];
    const Unit& t = units[ti];
    if (!a.alive || !t.alive || a.id == t.id || a.dmg_max <= 0.0f) return false;
    if (t.is_building() && building_protected(ti)) return false;
    if (a.team == t.team) {
        // Deny: heroes may finish off their own wounded creeps.
        return a.is_hero() && is_lane_creep(t.kind) && t.hp < t.max_hp * 0.5f;
    }
    return true;
}

bool Game::target_ok(size_t i, uint32_t tid, float max_edge_dist) const {
    auto oti = live_idx(tid);
    if (!oti) return false;
    size_t ti = *oti;
    if (!can_attack(i, ti)) return false;
    const Unit& a = units[i];
    if (!visible_to(a.team, ti)) return false;
    return a.pos.dist(units[ti].pos) - units[ti].radius <= max_edge_dist;
}

uint32_t Game::acquire(size_t i, float radius, bool allow_neutral) const {
    const Unit& a = units[i];
    float best_score = FLT_MAX;
    uint32_t best_id = 0;
    for (size_t j = 0; j < units.size(); ++j) {
        const Unit& t = units[j];
        if (!t.alive || j == i || t.team == a.team) continue;
        if (t.team == TEAM_NEUTRAL && !allow_neutral) continue;
        if (t.is_building() && building_protected(j)) continue;
        if (!visible_to(a.team, j)) continue;
        float d = a.pos.dist(t.pos) - t.radius;
        if (d > radius) continue;
        float score = d;
        if (t.is_building()) score += 600.0f;                    // fight units before structures
        if (t.is_hero() && is_lane_creep(a.kind)) score += 80.0f;  // creeps prefer creeps
        if (score < best_score) {
            best_score = score;
            best_id = t.id;
        }
    }
    return best_id;
}

// =================================================================== order execution

void Game::act(size_t i) {
    if (units[i].stunned()) {
        units[i].swing_t = 0.0f;
        return;
    }
    if (units[i].swing_t > 0.0f) {
        units[i].swing_t -= DT;
        if (units[i].swing_t <= 0.0f) finish_swing(i);
        return;
    }
    Order order = units[i].order;
    switch (order.k) {
        case Order::Stay: break;
        case Order::Idle: act_idle(i, false); break;
        case Order::Hold: act_idle(i, true); break;
        case Order::Move:
            if (move_toward(i, order.pt)) units[i].order = Order::idle();
            break;
        case Order::AttackMove: act_attack_move(i, order.pt); break;
        case Order::Attack:
            if (target_ok(i, order.target, FLT_MAX)) {
                units[i].target = order.target;
                attack_or_chase(i, order.target);
            } else {
                units[i].order = Order::idle();
                units[i].target = 0;
            }
            break;
        case Order::Cast: act_cast(i, order.slot, order.target, order.pt); break;
    }
}

float Game::acquire_radius(size_t i) const {
    const Unit& u = units[i];
    switch (u.kind) {
        case Kind::Hero: return fmax_rs(u.range + 130.0f, 380.0f);
        case Kind::Tower: return u.range;
        default: return fmax_rs(330.0f, u.range + 40.0f);
    }
}

void Game::act_idle(size_t i, bool hold) {
    const Unit& u = units[i];
    float reach = hold ? u.range : u.range + 50.0f;
    uint32_t tid = u.target;
    if (!target_ok(i, tid, reach + 20.0f)) {
        tid = acquire(i, reach, false);
        units[i].target = tid;
    }
    if (tid != 0) {
        if (hold) {
            // Hold position: only swing if in range, never walk.
            if (auto ti = live_idx(tid)) {
                if (in_attack_range(i, *ti)) attack_or_chase(i, tid);
                else units[i].target = 0;
            }
        } else {
            attack_or_chase(i, tid);
        }
    }
}

void Game::act_attack_move(size_t i, Vec2 to) {
    float acq = acquire_radius(i);
    // A human hero a-moving into a camp should attack it; bots never jungle on purpose.
    bool allow_neutral;
    {
        const Unit& u = units[i];
        allow_neutral = u.is_hero() && u.hero >= 0 && !heroes[(size_t)u.hero].bot;
    }
    uint32_t tid = units[i].target;
    if (!target_ok(i, tid, acq * 1.25f)) {
        tid = acquire(i, acq, allow_neutral);
        units[i].target = tid;
    }
    if (tid != 0) attack_or_chase(i, tid);
    else if (move_toward(i, to)) units[i].order = Order::idle();
}

bool Game::in_attack_range(size_t i, size_t ti) const {
    const Unit& u = units[i];
    const Unit& t = units[ti];
    return u.pos.dist(t.pos) <= u.range + t.radius + u.radius * 0.5f;
}

void Game::face(size_t i, Vec2 p) {
    Vec2 d = p - units[i].pos;
    if (d.len_sq() > 1e-4f) units[i].facing = std::atan2(d.y, d.x);
}

void Game::attack_or_chase(size_t i, uint32_t tid) {
    auto oti = live_idx(tid);
    if (!oti) return;
    size_t ti = *oti;
    Vec2 tp = units[ti].pos;
    if (in_attack_range(i, ti)) {
        face(i, tp);
        units[i].clear_path();
        if (units[i].attack_cd <= 0.0f) {
            Unit& u = units[i];
            float at = u.attack_time();
            u.swing_len = fmax_rs(at * 0.30f, 0.1f);
            u.swing_t = u.swing_len;
            u.swing_target = tid;
            u.attack_cd = at;
            emit({EV_ATTACK, (float)u.id, (float)tid, tp.x, tp.y, 0.0f, 0.0f, 0.0f});
        }
    } else if (units[i].kind != Kind::Tower) {
        move_toward(i, tp);
    }
}

/// The hit lands: melee deals damage immediately, ranged units launch a homing projectile.
void Game::finish_swing(size_t i) {
    uint32_t tid = units[i].swing_target;
    auto oti = live_idx(tid);
    if (!oti) return;
    size_t ti = *oti;
    // The target may have walked away during the wind-up.
    const Unit& u = units[i];
    const Unit& t = units[ti];
    float reach = u.range + t.radius + u.radius * 0.5f;
    if (u.pos.dist(t.pos) > reach * 1.6f + 20.0f || !can_attack(i, ti)) return;
    float dmg = rng.range(u.dmg_min, u.dmg_max + 1.0f);
    if (t.is_building()) dmg *= u.bldg_mult;
    uint32_t src = u.id;
    uint8_t team = u.team;
    Vec2 pos = u.pos;
    float speed = u.proj_speed;
    Vec2 tpos = t.pos;
    if (speed > 0.0f) {
        uint32_t id = alloc_id();
        Vec2 dir = (tpos - pos).norm();
        projs.push_back(Proj{id, src, team, pos, dir, speed, tid, dmg, DmgType::Physical, 0, 0.0f, 0.0f, 255, 0.0f, FLT_MAX, {}});
    } else {
        deal_damage(src, ti, dmg, DmgType::Physical);
    }
}

// =================================================================== movement

bool Game::move_toward(size_t i, Vec2 goal) {
    Vec2 pos = units[i].pos;
    if (pos.dist(goal) < 5.0f) {
        units[i].clear_path();
        return true;
    }
    float speed = units[i].speed();
    if (speed <= 0.0f) return false;
    float step = speed * DT;
    if (map.los(pos, goal)) {
        units[i].clear_path();
        Vec2 next = pos.move_toward(goal, step);
        apply_move(i, next);
        return units[i].pos.dist(goal) < 5.0f;
    }
    // Path around obstacles (re-plan when the goal moved or the route ran out).
    bool need_path;
    {
        const Unit& u = units[i];
        need_path = u.path_i >= u.path.size() || u.path_goal.dist(goal) > CELL * 1.5f;
    }
    if (need_path && units[i].repath_t <= 0.0f) {
        auto found = finder.find(map, pos, goal);
        Unit& u = units[i];
        u.repath_t = 0.4f;
        u.path_goal = goal;
        u.path_i = 0;
        u.path = found ? std::move(*found) : std::vector<Vec2>{};
    }
    // Consume waypoints along this tick's movement budget.
    int guard = 0;
    while (step > 0.0f && guard < 4) {
        guard += 1;
        const Unit& u = units[i];
        if (u.path_i >= u.path.size()) return false;
        Vec2 wp = u.path[u.path_i], p = u.pos;
        float d = p.dist(wp);
        if (d <= step) {
            apply_move(i, wp);
            step -= d;
            units[i].path_i += 1;
        } else {
            Vec2 next = p.move_toward(wp, step);
            apply_move(i, next);
            step = 0.0f;
        }
    }
    return false;
}

/// Move to `next` if walkable, sliding along walls otherwise.
void Game::apply_move(size_t i, Vec2 next) {
    Vec2 old = units[i].pos;
    Vec2 d = next - old;
    if (d.len_sq() < 1e-8f) return;
    Vec2 target;
    if (map.walkable(next)) target = next;
    else if (map.walkable(Vec2(next.x, old.y))) target = Vec2(next.x, old.y);
    else if (map.walkable(Vec2(old.x, next.y))) target = Vec2(old.x, next.y);
    else target = old;
    Unit& u = units[i];
    u.pos = target;
    u.facing = std::atan2(d.y, d.x);
}

/// Soft collision: push overlapping units apart (buildings never move).
void Game::separate_units() {
    size_t n = units.size();
    std::vector<Vec2> push(n);
    for (size_t a = 0; a < n; ++a) {
        if (!units[a].alive) continue;
        for (size_t b = a + 1; b < n; ++b) {
            if (!units[b].alive) continue;
            const Unit& ua = units[a];
            const Unit& ub = units[b];
            if (ua.is_building() && ub.is_building()) continue;
            float dx = ub.pos.x - ua.pos.x;
            if (std::fabs(dx) > 80.0f) continue;
            float dy = ub.pos.y - ua.pos.y;
            float min = (ua.radius + ub.radius) * 0.9f;
            float d2 = dx * dx + dy * dy;
            if (d2 >= min * min) continue;
            float d = std::sqrt(d2);
            Vec2 dir;
            if (d > 1e-3f) {
                dir = Vec2(dx / d, dy / d);
            } else {
                // Exactly stacked: separate along a deterministic pseudo-random axis.
                float ang = (float)(((ua.id * 2654435761u) ^ ub.id) % 628u) / 100.0f;
                dir = Vec2(std::cos(ang), std::sin(ang));
            }
            float overlap = fmin_rs(min - d, 6.0f);
            float ma = mass(ua), mb = mass(ub);
            float total = ma + mb;
            // Heavier units get pushed less; buildings (infinite mass) not at all.
            float fa, fb;
            if (ua.is_building()) {
                fa = 0.0f;
                fb = 1.0f;
            } else if (ub.is_building()) {
                fa = 1.0f;
                fb = 0.0f;
            } else {
                fa = mb / total;
                fb = ma / total;
            }
            push[a] -= dir * (overlap * fa);
            push[b] += dir * (overlap * fb);
        }
    }
    for (size_t i = 0; i < n; ++i) {
        if (push[i].len_sq() < 1e-8f) continue;
        Vec2 np = units[i].pos + push[i];
        if (map.walkable(np)) units[i].pos = np;
    }
}

// =================================================================== damage & death

float Game::deal_damage(uint32_t src, size_t di, float amount, DmgType dtype) {
    if (!units[di].alive || amount <= 0.0f) return 0.0f;
    if (units[di].is_building() && building_protected(di)) return 0.0f;
    uint32_t vid = units[di].id;
    uint8_t vteam = units[di].team;
    Kind vkind = units[di].kind;
    Vec2 vpos = units[di].pos;
    float varmor = units[di].armor;
    float mult = dtype == DmgType::Physical ? armor_multiplier(varmor) : dtype == DmgType::Magical ? 1.0f - content.rules.magic_resist : 1.0f;
    float dmg = amount * mult;
    {
        Unit& u = units[di];
        u.hp -= dmg;
        u.since_dmg = 0.0f;
        u.salve_t = 0.0f;  // damage breaks the healing salve
    }
    emit({EV_DAMAGE, (float)vid, (float)src, vpos.x, vpos.y, dmg, (float)(uint8_t)dtype, (float)vteam});

    if (auto si = idx(src)) {
        uint8_t steam = units[*si].team;
        bool shero = units[*si].is_hero();
        if (steam != vteam) {
            // Hero-on-hero fights: remember for assists and pull enemy towers onto the attacker.
            if (shero && vkind == Kind::Hero) {
                auto& rec = heroes[(size_t)units[di].hero].recent;
                auto r = std::find_if(rec.begin(), rec.end(), [&](const std::pair<uint32_t, float>& x) { return x.first == src; });
                if (r != rec.end()) r->second = 0.0f;
                else rec.emplace_back(src, 0.0f);
                for (Unit& t : units) {
                    if (t.alive && t.kind == Kind::Tower && t.team == vteam && t.pos.dist(vpos) <= t.range + 30.0f) {
                        t.force_target = src;
                        t.force_t = 3.0f;
                    }
                }
            }
            // Neutrals retaliate, and the whole camp joins in.
            if (vkind == Kind::Neutral) {
                int32_t camp = units[di].camp;
                if (camp >= 0) {
                    std::vector<uint32_t> members = camps[(size_t)camp].members;
                    for (uint32_t m : members) {
                        if (auto mi = live_idx(m)) {
                            if (units[*mi].target == 0 || !units[*mi].returning) {
                                units[*mi].target = src;
                                units[*mi].returning = false;
                            }
                        }
                    }
                }
            }
        }
    }
    if (units[di].hp <= 0.0f) kill_unit(di, src);
    return dmg;
}

void Game::apply_effects(uint32_t src, size_t di, float dmg, DmgType dtype, uint8_t fx, float dur, float pct) {
    if (dmg > 0.0f) deal_damage(src, di, dmg, dtype);
    Unit& u = units[di];
    if (!u.alive) return;
    if (fx & FX_STUN) {
        u.stun_t = fmax_rs(u.stun_t, dur);
        u.swing_t = 0.0f;
    }
    if (fx & FX_ROOT) u.root_t = fmax_rs(u.root_t, dur);
    if (fx & FX_SLOW) {
        if (u.slow_t <= 0.0f) u.slow_pct = pct;
        else u.slow_pct = fmax_rs(u.slow_pct, pct);
        u.slow_t = fmax_rs(u.slow_t, dur);
    }
}

void Game::award_xp(uint8_t team, Vec2 center, float xp) {
    std::vector<size_t> rec;
    for (size_t i = 0; i < units.size(); ++i) {
        const Unit& u = units[i];
        if (u.alive && u.is_hero() && u.team == team && u.pos.dist(center) <= content.rules.xp_radius) rec.push_back(i);
    }
    if (rec.empty()) return;
    float share = xp / (float)rec.size();
    for (size_t i : rec) add_xp(i, share);
}

void Game::award_gold(size_t ui, float gold, Vec2 at) {
    if (gold <= 0.0f) return;
    add_gold(ui, gold);
    emit({EV_GOLD, (float)units[ui].id, 0.0f, at.x, at.y, gold, 0.0f, 0.0f});
}

void Game::kill_unit(size_t di, uint32_t killer_id) {
    if (!units[di].alive) return;
    uint32_t vid;
    Kind vkind;
    uint8_t vteam;
    Vec2 vpos;
    float vgold, vxp;
    {
        Unit& u = units[di];
        u.alive = false;
        u.hp = 0.0f;
        u.swing_t = 0.0f;
        u.order = Order::idle();
        vid = u.id;
        vkind = u.kind;
        vteam = u.team;
        vpos = u.pos;
        vgold = u.bounty_gold;
        vxp = u.bounty_xp;
    }
    auto killer = idx(killer_id);
    uint8_t kteam = killer ? units[*killer].team : 255;
    bool khero = killer ? units[*killer].is_hero() : false;
    uint8_t enemy_team = (uint8_t)(1 - std::min<uint8_t>(vteam, 1));
    float gold_given = 0.0f;

    switch (vkind) {
        case Kind::Hero: {
            size_t hi = (size_t)units[di].hero;
            float level = (float)heroes[hi].level;
            const Rules& rules = content.rules;
            {
                HeroData& h = heroes[hi];
                h.deaths += 1;
                h.respawn_t = fmin_rs(rules.respawn_base + level * rules.respawn_per_level, rules.respawn_max);
                h.ai_state = 0;
            }
            float bounty = rules.hero_bounty_base + rules.hero_bounty_per_level * level;
            float xp = rules.hero_xp_base + rules.hero_xp_per_level * level;
            std::vector<uint32_t> assist_ids;
            for (auto& r : heroes[hi].recent)
                if (r.first != killer_id && r.second <= 12.0f) assist_ids.push_back(r.first);
            heroes[hi].recent.clear();
            if (khero && kteam != vteam) {
                size_t k = *killer;
                heroes[(size_t)units[k].hero].kills += 1;
                award_gold(k, bounty, vpos);
                gold_given = bounty;
                for (uint32_t aid : assist_ids) {
                    if (auto ai = idx(aid)) {
                        if (units[*ai].team == kteam) {
                            heroes[(size_t)units[*ai].hero].assists += 1;
                            award_gold(*ai, bounty * 0.3f, vpos);
                            emit({EV_ASSIST, (float)aid, (float)vid, vpos.x, vpos.y, 0.0f, 0.0f, 0.0f});
                        }
                    }
                }
            }
            award_xp(enemy_team, vpos, xp);
            break;
        }
        case Kind::Melee:
        case Kind::Ranged:
        case Kind::Siege:
            if (khero && kteam == vteam) {
                // Deny: the enemy gets half XP and nobody gets gold.
                size_t k = *killer;
                heroes[(size_t)units[k].hero].denies += 1;
                award_xp(enemy_team, vpos, vxp * 0.5f);
            } else {
                if (khero && kteam != vteam) {
                    size_t k = *killer;
                    heroes[(size_t)units[k].hero].last_hits += 1;
                    award_gold(k, vgold, vpos);
                    gold_given = vgold;
                }
                award_xp(enemy_team, vpos, vxp);
            }
            break;
        case Kind::Neutral:
            if (khero) {
                size_t k = *killer;
                heroes[(size_t)units[k].hero].last_hits += 1;
                award_gold(k, vgold, vpos);
                gold_given = vgold;
                award_xp(kteam, vpos, vxp);
            }
            break;
        case Kind::Tower:
        case Kind::Ancient:
            // Everyone on the destroying team shares the bounty.
            if (kteam <= 1 && kteam != vteam) {
                std::vector<size_t> ids;
                for (size_t ui = 0; ui < units.size(); ++ui)
                    if (units[ui].is_hero() && units[ui].team == kteam) ids.push_back(ui);
                for (size_t ui : ids) {
                    add_gold(ui, vgold);
                    emit({EV_GOLD, (float)units[ui].id, 0.0f, vpos.x, vpos.y, vgold, 0.0f, 0.0f});
                }
                gold_given = vgold;
            }
            emit({EV_BUILDING, (float)vid, (float)(uint8_t)vkind, vpos.x, vpos.y, 0.0f, 0.0f, (float)vteam});
            recompute_tower_flags();
            if (vkind == Kind::Ancient) {
                winner = enemy_team;
                emit({EV_GAMEOVER, (float)enemy_team, 0.0f, vpos.x, vpos.y, 0.0f, 0.0f, 0.0f});
            }
            break;
    }
    emit({EV_KILL, (float)vid, (float)killer_id, vpos.x, vpos.y, gold_given, (float)(uint8_t)vkind, (float)vteam});
}

// =================================================================== projectiles & zones

void Game::update_projectiles() {
    std::vector<Proj> ps = std::move(projs);
    projs.clear();
    std::vector<Proj> keep;
    keep.reserve(ps.size());
    for (Proj& p : ps) {
        float step = p.speed * DT;
        if (p.homing != 0) {
            auto oti = live_idx(p.homing);
            if (!oti) continue;  // target died: the projectile fizzles
            size_t ti = *oti;
            Vec2 tp = units[ti].pos;
            float tr = units[ti].radius;
            if (p.pos.dist(tp) <= step + tr * 0.6f + 4.0f) {
                if (p.ability == 255) deal_damage(p.src, ti, p.dmg, p.dtype);
                else apply_effects(p.src, ti, p.dmg, p.dtype, p.fx, p.dur, p.pct);
                continue;
            }
            p.pos = p.pos.move_toward(tp, step);
            p.dir = (tp - p.pos).norm();
            keep.push_back(std::move(p));
        } else {
            Vec2 old = p.pos;
            float adv = fmin_rs(step, p.remaining);
            p.pos = old + p.dir * adv;
            p.remaining -= adv;
            for (size_t ti = 0; ti < units.size(); ++ti) {
                const Unit& t = units[ti];
                if (!t.alive || t.team == p.team || t.is_building() || std::find(p.hits.begin(), p.hits.end(), t.id) != p.hits.end()) continue;
                Vec2 q = closest_point_on_segment(t.pos, old, p.pos);
                if (q.dist(t.pos) <= p.radius + t.radius) {
                    p.hits.push_back(t.id);
                    apply_effects(p.src, ti, p.dmg, p.dtype, p.fx, p.dur, p.pct);
                }
            }
            if (p.remaining > 0.01f) keep.push_back(std::move(p));
        }
    }
    // Anything launched while resolving hits is appended after the survivors.
    std::vector<Proj> launched = std::move(projs);
    projs = std::move(keep);
    for (Proj& p : launched) projs.push_back(std::move(p));
}

void Game::update_zones() {
    std::vector<Zone> zs = std::move(zones);
    zones.clear();
    std::vector<Zone> keep;
    keep.reserve(zs.size());
    for (Zone& z : zs) {
        z.t_left -= DT;
        if (z.t_left <= 0.0f) aoe_hit(z.src, z.team, z.pos, z.radius, z.dmg, z.dtype, z.fx, z.dur, z.pct);
        else keep.push_back(z);
    }
    std::vector<Zone> added = std::move(zones);
    zones = std::move(keep);
    for (Zone& z : added) zones.push_back(z);
}

}  // namespace moba
