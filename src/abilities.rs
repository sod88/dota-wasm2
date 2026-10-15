//! Hero abilities and usable items.

use crate::data::*;
use crate::game::*;
use crate::math::*;
use crate::unit::*;

impl Game {
    /// Hit every hostile non-building unit around `center`.
    #[allow(clippy::too_many_arguments)]
    pub fn aoe_hit(&mut self, src: u32, team: u8, center: Vec2, radius: f32, dmg: f32, dtype: DmgType, fx: u8, dur: f32, pct: f32) {
        for ti in 0..self.units.len() {
            let t = &self.units[ti];
            if !t.alive || t.team == team || t.is_building() {
                continue;
            }
            if t.pos.dist(center) <= radius + t.radius * 0.5 {
                self.apply_effects(src, ti, dmg, dtype, fx, dur, pct);
            }
        }
    }

    /// Can this hero cast ability `slot` right now (level, cooldown, mana, not stunned)?
    pub fn ability_ready(&self, ui: usize, slot: usize) -> bool {
        let hi = self.units[ui].hero;
        if hi < 0 || slot > 3 {
            return false;
        }
        let h = &self.heroes[hi as usize];
        let lvl = h.ab_level[slot];
        if lvl == 0 || h.ab_cd[slot] > 0.0 {
            return false;
        }
        let ab = &self.content.abilities[h.def as usize * 4 + slot];
        self.units[ui].mana >= ab.mana[lvl as usize - 1] && !self.units[ui].stunned()
    }

    pub fn ability_def(&self, ui: usize, slot: usize) -> Option<(&'static AbilityDef, usize)> {
        let hi = self.units[ui].hero;
        if hi < 0 {
            return None;
        }
        let h = &self.heroes[hi as usize];
        let lvl = h.ab_level[slot];
        if lvl == 0 {
            return None;
        }
        Some((&self.content.abilities[h.def as usize * 4 + slot], lvl as usize - 1))
    }

    /// Entry point for a player/bot cast command: instant spells fire immediately,
    /// targeted and ground-targeted ones walk into range first.
    pub fn order_cast(&mut self, ui: usize, slot: u8, target: u32, point: Vec2) {
        if slot < 4 {
            let (ab, _) = match self.ability_def(ui, slot as usize) {
                Some(x) => x,
                None => return,
            };
            match ab.cast {
                Cast::Target | Cast::PointAoE => {
                    if !self.ability_ready(ui, slot as usize) {
                        return;
                    }
                    self.set_order(ui, Order::Cast { slot, target, point });
                }
                _ => {
                    self.cast_ability(ui, slot as usize, target, point);
                }
            }
        } else if (10..10 + INV_SLOTS as u8).contains(&slot) {
            self.use_item(ui, (slot - 10) as usize, point);
        }
    }

    /// Order::Cast executor: approach, then cast.
    pub fn act_cast(&mut self, i: usize, slot: u8, target: u32, point: Vec2) {
        let (ab, l) = match self.ability_def(i, slot as usize) {
            Some(x) => x,
            None => {
                self.units[i].order = Order::Idle;
                return;
            }
        };
        let range = ab.range[l];
        let me = self.units[i].pos;
        match ab.cast {
            Cast::Target => {
                let ti = match self.live_idx(target) {
                    Some(t) if self.visible_to(self.units[i].team, t) => t,
                    _ => {
                        self.units[i].order = Order::Idle;
                        return;
                    }
                };
                let (tp, tr) = (self.units[ti].pos, self.units[ti].radius);
                if me.dist(tp) <= range + tr {
                    self.cast_ability(i, slot as usize, target, tp);
                    self.units[i].order = Order::Idle;
                } else if !self.move_toward(i, tp) && self.units[i].speed() <= 0.0 {
                    self.units[i].order = Order::Idle; // rooted/stunned: cannot approach
                }
            }
            Cast::PointAoE => {
                if me.dist(point) <= range {
                    self.cast_ability(i, slot as usize, 0, point);
                    self.units[i].order = Order::Idle;
                } else if !self.move_toward(i, point) && self.units[i].speed() <= 0.0 {
                    self.units[i].order = Order::Idle;
                }
            }
            _ => {
                self.cast_ability(i, slot as usize, target, point);
                self.units[i].order = Order::Idle;
            }
        }
    }

    /// Actually cast hero ability `slot`. Returns false (and costs nothing) if it can't fire.
    pub fn cast_ability(&mut self, ui: usize, slot: usize, target: u32, point: Vec2) -> bool {
        if !self.ability_ready(ui, slot) {
            return false;
        }
        let (ab, l) = self.ability_def(ui, slot).unwrap();
        let hi = self.units[ui].hero as usize;
        let ab_id = (self.heroes[hi].def as usize * 4 + slot) as u8;
        let (src, team, pos) = (self.units[ui].id, self.units[ui].team, self.units[ui].pos);
        let (dmg, dur, pct, radius, range) = (ab.dmg[l], ab.dur[l], ab.pct[l], ab.radius[l], ab.range[l]);
        let mut cast_pt = point;

        let ok = match ab.cast {
            Cast::Target => {
                let ti = match self.live_idx(target) {
                    Some(t) => t,
                    None => return false,
                };
                let t = &self.units[ti];
                if t.team == team || t.is_building() || pos.dist(t.pos) > range + t.radius + 12.0 {
                    return false;
                }
                let tp = t.pos;
                cast_pt = tp;
                self.face(ui, tp);
                if ab.speed > 0.0 {
                    let id = self.alloc_id();
                    self.projs.push(Proj {
                        id,
                        src,
                        team,
                        pos,
                        dir: (tp - pos).norm(),
                        speed: ab.speed,
                        homing: target,
                        dmg,
                        dtype: ab.dtype,
                        fx: ab.fx,
                        dur,
                        pct,
                        ability: ab_id,
                        radius: 0.0,
                        remaining: f32::MAX,
                        hits: Vec::new(),
                    });
                } else {
                    self.apply_effects(src, ti, dmg, ab.dtype, ab.fx, dur, pct);
                }
                true
            }
            Cast::Line => {
                let mut dir = (point - pos).norm();
                if dir.len_sq() < 0.5 {
                    let f = self.units[ui].facing;
                    dir = Vec2::new(f.cos(), f.sin());
                }
                self.face(ui, pos + dir * 10.0);
                let id = self.alloc_id();
                self.projs.push(Proj {
                    id,
                    src,
                    team,
                    pos,
                    dir,
                    speed: ab.speed,
                    homing: 0,
                    dmg,
                    dtype: ab.dtype,
                    fx: ab.fx,
                    dur,
                    pct,
                    ability: ab_id,
                    radius,
                    remaining: range,
                    hits: Vec::new(),
                });
                cast_pt = pos + dir * range;
                true
            }
            Cast::PointAoE => {
                let d = point - pos;
                let p = if d.len() > range { pos + d.norm() * range } else { point };
                cast_pt = p;
                self.face(ui, p);
                if ab.delay > 0.0 {
                    let id = self.alloc_id();
                    self.zones.push(Zone {
                        id,
                        pos: p,
                        radius,
                        t_left: ab.delay,
                        t_total: ab.delay,
                        team,
                        src,
                        dmg,
                        dtype: ab.dtype,
                        fx: ab.fx,
                        dur,
                        pct,
                        ability: ab_id,
                    });
                } else {
                    self.aoe_hit(src, team, p, radius, dmg, ab.dtype, ab.fx, dur, pct);
                }
                true
            }
            Cast::SelfAoE => {
                cast_pt = pos;
                self.aoe_hit(src, team, pos, radius, dmg, ab.dtype, ab.fx, dur, pct);
                true
            }
            Cast::SelfBuff => {
                cast_pt = pos;
                let u = &mut self.units[ui];
                if ab.fx & FX_ASPD != 0 {
                    u.aspd_t = dur;
                    u.aspd_pct = pct;
                }
                if ab.fx & FX_HASTE != 0 {
                    u.haste_t = dur;
                    u.haste_pct = ab.pct2[l];
                }
                true
            }
            Cast::Blink => match self.do_blink(ui, point, range) {
                Some(dest) => {
                    cast_pt = dest;
                    if ab.aoe_on_land {
                        self.aoe_hit(src, team, dest, radius, dmg, ab.dtype, ab.fx, dur, pct);
                    }
                    true
                }
                None => false,
            },
            Cast::None => false,
        };
        if ok {
            self.units[ui].mana -= ab.mana[l];
            self.heroes[hi].ab_cd[slot] = ab.cd[l];
            self.emit([EV_CAST, src as f32, slot as f32, cast_pt.x, cast_pt.y, radius, ab_id as f32, 0.0]);
        }
        ok
    }

    /// Teleport up to `range` towards `point`, stopping at the last walkable spot.
    pub fn do_blink(&mut self, ui: usize, point: Vec2, range: f32) -> Option<Vec2> {
        let pos = self.units[ui].pos;
        let mut d = point - pos;
        if d.len_sq() < 1.0 {
            let f = self.units[ui].facing;
            d = Vec2::new(f.cos(), f.sin()) * range;
        }
        let dist = d.len().min(range);
        let dir = d.norm();
        let mut dd = dist;
        while dd >= 12.0 {
            let cand = pos + dir * dd;
            if self.map.walkable(cand) {
                let id = self.units[ui].id;
                self.emit([EV_BLINK, id as f32, 0.0, pos.x, pos.y, cand.x, cand.y, 0.0]);
                let u = &mut self.units[ui];
                u.pos = cand;
                u.clear_path();
                u.facing = dir.y.atan2(dir.x);
                return Some(cand);
            }
            dd -= 12.0;
        }
        None
    }

    /// Use the active item in inventory slot `slot`.
    pub fn use_item(&mut self, ui: usize, slot: usize, point: Vec2) -> bool {
        let hi = self.units[ui].hero;
        if hi < 0 || slot >= INV_SLOTS || self.units[ui].stunned() {
            return false;
        }
        let hi = hi as usize;
        let s = self.heroes[hi].items[slot];
        if s.id == 0 || s.cd > 0.0 {
            return false;
        }
        let it = self.content.items[s.id as usize];
        let (src, pos) = (self.units[ui].id, self.units[ui].pos);
        let mut cast_pt = pos;
        match it.active {
            ACT_SALVE => {
                let u = &mut self.units[ui];
                if u.hp >= u.max_hp {
                    return false;
                }
                u.salve_t = self.content.rules.salve_time;
                u.salve_rate = self.content.rules.salve_heal / self.content.rules.salve_time;
            }
            ACT_MANA => {
                let u = &mut self.units[ui];
                if u.mana >= u.max_mana {
                    return false;
                }
                u.mana_pot_t = self.content.rules.mana_potion_time;
                u.mana_pot_rate = self.content.rules.mana_potion_restore / self.content.rules.mana_potion_time;
            }
            ACT_BLINK => match self.do_blink(ui, point, self.content.rules.blink_range) {
                Some(d) => cast_pt = d,
                None => return false,
            },
            _ => return false,
        }
        let h = &mut self.heroes[hi];
        if it.stack {
            let sl = &mut h.items[slot];
            sl.charges = sl.charges.saturating_sub(1);
            if sl.charges == 0 {
                *sl = ItemSlot::default();
            } else {
                sl.cd = it.cd;
            }
        } else {
            h.items[slot].cd = it.cd;
        }
        self.emit([EV_CAST, src as f32, (10 + slot) as f32, cast_pt.x, cast_pt.y, 0.0, (100 + s.id as usize) as f32, 0.0]);
        true
    }
}
