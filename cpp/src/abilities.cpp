// Hero abilities and usable items (port of src/abilities.rs).
#include <cfloat>
#include <cmath>

#include "game.hpp"

namespace moba {

void Game::aoe_hit(uint32_t src, uint8_t team, Vec2 center, float radius, float dmg, DmgType dtype, uint8_t fx, float dur, float pct) {
    for (size_t ti = 0; ti < units.size(); ++ti) {
        const Unit& t = units[ti];
        if (!t.alive || t.team == team || t.is_building()) continue;
        if (t.pos.dist(center) <= radius + t.radius * 0.5f) apply_effects(src, ti, dmg, dtype, fx, dur, pct);
    }
}

bool Game::ability_ready(size_t ui, size_t slot) const {
    int32_t hi = units[ui].hero;
    if (hi < 0 || slot > 3) return false;
    const HeroData& h = heroes[(size_t)hi];
    uint8_t lvl = h.ab_level[slot];
    if (lvl == 0 || h.ab_cd[slot] > 0.0f) return false;
    const AbilityDef& ab = content.abilities[(size_t)h.def * 4 + slot];
    return units[ui].mana >= ab.mana[lvl - 1] && !units[ui].stunned();
}

const AbilityDef* Game::ability_def(size_t ui, size_t slot, size_t& level_idx) const {
    int32_t hi = units[ui].hero;
    if (hi < 0) return nullptr;
    const HeroData& h = heroes[(size_t)hi];
    uint8_t lvl = h.ab_level[slot];
    if (lvl == 0) return nullptr;
    level_idx = (size_t)lvl - 1;
    return &content.abilities[(size_t)h.def * 4 + slot];
}

void Game::order_cast(size_t ui, uint8_t slot, uint32_t target, Vec2 point) {
    if (slot < 4) {
        size_t l = 0;
        const AbilityDef* ab = ability_def(ui, slot, l);
        if (!ab) return;
        if (ab->cast == Cast::Target || ab->cast == Cast::PointAoE) {
            if (!ability_ready(ui, slot)) return;
            set_order(ui, Order::cast(slot, target, point));
        } else {
            cast_ability(ui, slot, target, point);
        }
    } else if (slot >= 10 && slot < 10 + INV_SLOTS) {
        use_item(ui, (size_t)(slot - 10), point);
    }
}

void Game::act_cast(size_t i, uint8_t slot, uint32_t target, Vec2 point) {
    size_t l = 0;
    const AbilityDef* ab = ability_def(i, slot, l);
    if (!ab) {
        units[i].order = Order::idle();
        return;
    }
    float range = ab->range[l];
    Vec2 me = units[i].pos;
    switch (ab->cast) {
        case Cast::Target: {
            auto oti = live_idx(target);
            if (!oti || !visible_to(units[i].team, *oti)) {
                units[i].order = Order::idle();
                return;
            }
            Vec2 tp = units[*oti].pos;
            float tr = units[*oti].radius;
            if (me.dist(tp) <= range + tr) {
                cast_ability(i, slot, target, tp);
                units[i].order = Order::idle();
            } else if (!move_toward(i, tp) && units[i].speed() <= 0.0f) {
                units[i].order = Order::idle();  // rooted/stunned: cannot approach
            }
            break;
        }
        case Cast::PointAoE:
            if (me.dist(point) <= range) {
                cast_ability(i, slot, 0, point);
                units[i].order = Order::idle();
            } else if (!move_toward(i, point) && units[i].speed() <= 0.0f) {
                units[i].order = Order::idle();
            }
            break;
        default:
            cast_ability(i, slot, target, point);
            units[i].order = Order::idle();
            break;
    }
}

bool Game::cast_ability(size_t ui, size_t slot, uint32_t target, Vec2 point) {
    if (!ability_ready(ui, slot)) return false;
    size_t l = 0;
    const AbilityDef* abp = ability_def(ui, slot, l);
    const AbilityDef& ab = *abp;
    size_t hi = (size_t)units[ui].hero;
    uint8_t ab_id = (uint8_t)((size_t)heroes[hi].def * 4 + slot);
    uint32_t src = units[ui].id;
    uint8_t team = units[ui].team;
    Vec2 pos = units[ui].pos;
    float dmg = ab.dmg[l], dur = ab.dur[l], pct = ab.pct[l], radius = ab.radius[l], range = ab.range[l];
    Vec2 cast_pt = point;

    bool ok = false;
    switch (ab.cast) {
        case Cast::Target: {
            auto oti = live_idx(target);
            if (!oti) return false;
            size_t ti = *oti;
            const Unit& t = units[ti];
            if (t.team == team || t.is_building() || pos.dist(t.pos) > range + t.radius + 12.0f) return false;
            Vec2 tp = t.pos;
            cast_pt = tp;
            face(ui, tp);
            if (ab.speed > 0.0f) {
                uint32_t id = alloc_id();
                projs.push_back(Proj{id, src, team, pos, (tp - pos).norm(), ab.speed, target, dmg, ab.dtype, ab.fx, dur, pct, ab_id, 0.0f, FLT_MAX, {}});
            } else {
                apply_effects(src, ti, dmg, ab.dtype, ab.fx, dur, pct);
            }
            ok = true;
            break;
        }
        case Cast::Line: {
            Vec2 dir = (point - pos).norm();
            if (dir.len_sq() < 0.5f) {
                float f = units[ui].facing;
                dir = Vec2(std::cos(f), std::sin(f));
            }
            face(ui, pos + dir * 10.0f);
            uint32_t id = alloc_id();
            projs.push_back(Proj{id, src, team, pos, dir, ab.speed, 0, dmg, ab.dtype, ab.fx, dur, pct, ab_id, radius, range, {}});
            cast_pt = pos + dir * range;
            ok = true;
            break;
        }
        case Cast::PointAoE: {
            Vec2 d = point - pos;
            Vec2 p = d.len() > range ? pos + d.norm() * range : point;
            cast_pt = p;
            face(ui, p);
            if (ab.delay > 0.0f) {
                uint32_t id = alloc_id();
                zones.push_back(Zone{id, p, radius, ab.delay, ab.delay, team, src, dmg, ab.dtype, ab.fx, dur, pct, ab_id});
            } else {
                aoe_hit(src, team, p, radius, dmg, ab.dtype, ab.fx, dur, pct);
            }
            ok = true;
            break;
        }
        case Cast::SelfAoE:
            cast_pt = pos;
            aoe_hit(src, team, pos, radius, dmg, ab.dtype, ab.fx, dur, pct);
            ok = true;
            break;
        case Cast::SelfBuff: {
            cast_pt = pos;
            Unit& u = units[ui];
            if (ab.fx & FX_ASPD) {
                u.aspd_t = dur;
                u.aspd_pct = pct;
            }
            if (ab.fx & FX_HASTE) {
                u.haste_t = dur;
                u.haste_pct = ab.pct2[l];
            }
            ok = true;
            break;
        }
        case Cast::Blink:
            if (auto dest = do_blink(ui, point, range)) {
                cast_pt = *dest;
                if (ab.aoe_on_land) aoe_hit(src, team, *dest, radius, dmg, ab.dtype, ab.fx, dur, pct);
                ok = true;
            }
            break;
        case Cast::None: ok = false; break;
    }
    if (ok) {
        units[ui].mana -= ab.mana[l];
        heroes[hi].ab_cd[slot] = ab.cd[l];
        emit({EV_CAST, (float)src, (float)slot, cast_pt.x, cast_pt.y, radius, (float)ab_id, 0.0f});
    }
    return ok;
}

std::optional<Vec2> Game::do_blink(size_t ui, Vec2 point, float range) {
    Vec2 pos = units[ui].pos;
    Vec2 d = point - pos;
    if (d.len_sq() < 1.0f) {
        float f = units[ui].facing;
        d = Vec2(std::cos(f), std::sin(f)) * range;
    }
    float dist = fmin_rs(d.len(), range);
    Vec2 dir = d.norm();
    float dd = dist;
    while (dd >= 12.0f) {
        Vec2 cand = pos + dir * dd;
        if (map.walkable(cand)) {
            emit({EV_BLINK, (float)units[ui].id, 0.0f, pos.x, pos.y, cand.x, cand.y, 0.0f});
            Unit& u = units[ui];
            u.pos = cand;
            u.clear_path();
            u.facing = std::atan2(dir.y, dir.x);
            return cand;
        }
        dd -= 12.0f;
    }
    return std::nullopt;
}

bool Game::use_item(size_t ui, size_t slot, Vec2 point) {
    int32_t hi = units[ui].hero;
    if (hi < 0 || slot >= INV_SLOTS || units[ui].stunned()) return false;
    ItemSlot s = heroes[(size_t)hi].items[slot];
    if (s.id == 0 || s.cd > 0.0f) return false;
    const ItemDef& it = content.items[s.id];
    uint32_t src = units[ui].id;
    Vec2 cast_pt = units[ui].pos;
    const Rules& r = content.rules;
    switch (it.active) {
        case ACT_SALVE: {
            Unit& u = units[ui];
            if (u.hp >= u.max_hp) return false;
            u.salve_t = r.salve_time;
            u.salve_rate = r.salve_heal / r.salve_time;
            break;
        }
        case ACT_MANA: {
            Unit& u = units[ui];
            if (u.mana >= u.max_mana) return false;
            u.mana_pot_t = r.mana_potion_time;
            u.mana_pot_rate = r.mana_potion_restore / r.mana_potion_time;
            break;
        }
        case ACT_BLINK:
            if (auto d = do_blink(ui, point, r.blink_range)) cast_pt = *d;
            else return false;
            break;
        default: return false;
    }
    HeroData& h = heroes[(size_t)hi];
    if (it.stack) {
        ItemSlot& sl = h.items[slot];
        sl.charges = sl.charges > 0 ? (uint8_t)(sl.charges - 1) : 0;
        if (sl.charges == 0) sl = ItemSlot{};
        else sl.cd = it.cd;
    } else {
        h.items[slot].cd = it.cd;
    }
    emit({EV_CAST, (float)src, (float)(10 + slot), cast_pt.x, cast_pt.y, 0.0f, (float)(100 + (size_t)s.id), 0.0f});
    return true;
}

}  // namespace moba
