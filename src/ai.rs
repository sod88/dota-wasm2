//! Computer-controlled units: lane creeps, jungle neutrals, towers and hero bots.

use crate::data::*;
use crate::game::*;
use crate::map::*;
use crate::math::*;
use crate::unit::*;

const AI_STATE_LANE: u8 = 0;
const AI_STATE_RETREAT: u8 = 1;
const AI_STATE_SHOP: u8 = 2;

impl Game {
    pub fn think(&mut self, i: usize) {
        match self.units[i].kind {
            Kind::Melee | Kind::Ranged | Kind::Siege => self.think_lane_creep(i),
            Kind::Neutral => self.think_neutral(i),
            Kind::Tower => self.think_tower(i),
            Kind::Hero => {
                let hi = self.units[i].hero as usize;
                if self.heroes[hi].bot {
                    self.think_bot(i);
                }
            }
            Kind::Ancient => {}
        }
    }

    // =================================================================== lane creeps

    fn think_lane_creep(&mut self, i: usize) {
        let (team, lane, pos) = (self.units[i].team as usize, self.units[i].lane as usize, self.units[i].pos);
        let path = &self.lane_paths[team][lane];
        if path.is_empty() {
            return;
        }
        let last = path.len() - 1;
        let mut wp = self.units[i].wp.min(last);
        while wp < last && pos.dist(path[wp]) < 60.0 {
            wp += 1;
        }
        let goal = path[wp];
        let u = &mut self.units[i];
        u.wp = wp;
        u.order = Order::AttackMove(goal);
    }

    // =================================================================== neutrals

    fn think_neutral(&mut self, i: usize) {
        let (home, pos, tid, returning) = {
            let u = &self.units[i];
            (u.home, u.pos, u.target, u.returning)
        };
        if returning {
            let u = &mut self.units[i];
            u.hp = (u.hp + u.max_hp * 0.10 * DT).min(u.max_hp);
            if pos.dist(home) < 14.0 {
                u.returning = false;
                u.target = 0;
                u.order = Order::Stay;
                u.clear_path();
            } else {
                u.target = 0;
                u.order = Order::Move(home);
            }
            return;
        }
        if tid != 0 {
            let leash_ok = pos.dist(home) <= 480.0;
            if leash_ok && self.live_idx(tid).is_some() && self.target_ok(i, tid, f32::MAX) {
                self.units[i].order = Order::Attack(tid);
            } else {
                let u = &mut self.units[i];
                u.returning = true;
                u.target = 0;
                u.order = Order::Move(home);
                u.clear_path();
            }
        } else {
            self.units[i].order = Order::Stay;
        }
    }

    // =================================================================== towers

    fn think_tower(&mut self, i: usize) {
        let (pos, range, team, force, force_t) = {
            let u = &self.units[i];
            (u.pos, u.range, u.team, u.force_target, u.force_t)
        };
        // 1) A hero that just hit one of our heroes under the tower gets priority.
        if force != 0 && force_t > 0.0 {
            if let Some(f) = self.live_idx(force) {
                let t = &self.units[f];
                if t.pos.dist(pos) - t.radius <= range && self.target_ok(i, force, range) {
                    self.units[i].target = force;
                    self.units[i].order = Order::Attack(force);
                    return;
                }
            }
        }
        // 2) Otherwise creeps first, then heroes; nearest within each class.
        let mut best: (u8, f32, u32) = (9, f32::MAX, 0);
        for (j, t) in self.units.iter().enumerate() {
            if !t.alive || t.team == team || t.team == TEAM_NEUTRAL || t.is_building() {
                continue;
            }
            let d = t.pos.dist(pos) - t.radius;
            if d > range {
                continue;
            }
            let class = if t.is_hero() { 1 } else { 0 };
            if (class, d) < (best.0, best.1) && self.visible_to(team, j) {
                best = (class, d, t.id);
            }
        }
        let u = &mut self.units[i];
        u.target = best.2;
        u.order = if best.2 != 0 { Order::Attack(best.2) } else { Order::Stay };
    }

    // =================================================================== hero bot

    fn bot_level_skills(&mut self, ui: usize) {
        let hi = self.units[ui].hero as usize;
        while self.heroes[hi].skill_pts > 0 {
            if self.learn_ability(ui, 3) {
                continue;
            }
            let order = self.content.heroes[self.heroes[hi].def as usize].skill_order;
            let mut cands: Vec<(u8, usize, usize)> = Vec::new();
            for (pri, &slot) in order.iter().enumerate() {
                cands.push((self.heroes[hi].ab_level[slot as usize], pri, slot as usize));
            }
            cands.sort();
            let mut learned = false;
            for (_, _, slot) in cands {
                if self.learn_ability(ui, slot) {
                    learned = true;
                    break;
                }
            }
            if !learned {
                break;
            }
        }
    }

    fn bot_shop(&mut self, ui: usize) {
        let hi = self.units[ui].hero as usize;
        let build = self.content.heroes[self.heroes[hi].def as usize].build;
        // Opening: one healing salve if we can afford it alongside the first item.
        let salve = self.content.items.iter().position(|it| it.active == ACT_SALVE).unwrap_or(0) as u8;
        if self.heroes[hi].build_i == 0 && !build.is_empty() && salve != 0 && !self.heroes[hi].items.iter().any(|s| s.id == salve) {
            let first = self.content.items[build[0] as usize].cost;
            if self.heroes[hi].gold >= first + self.content.items[salve as usize].cost {
                self.buy_item(ui, salve);
            }
        }
        while self.heroes[hi].build_i < build.len() {
            let item = build[self.heroes[hi].build_i];
            if self.heroes[hi].gold >= self.content.items[item as usize].cost && self.buy_item(ui, item) {
                self.heroes[hi].build_i += 1;
            } else {
                break;
            }
        }
        self.heroes[hi].shop_cd = 25.0;
    }

    fn next_item_cost(&self, hi: usize) -> Option<f32> {
        let build = self.content.heroes[self.heroes[hi].def as usize].build;
        let h = &self.heroes[hi];
        if h.build_i >= build.len() || h.free_slot().is_none() {
            return None;
        }
        Some(self.content.items[build[h.build_i] as usize].cost)
    }

    /// Where a bot should stand while laning: just behind its own creep line.
    fn lane_hold_pos(&self, team: u8, lane: usize) -> Vec2 {
        let enemy_base = base_of(1 - team);
        let own_base = base_of(team);
        let mut front: Option<(f32, Vec2)> = None;
        for u in &self.units {
            if u.alive && u.team == team && u.kind.is_lane_creep() && u.lane as usize == lane {
                let d = u.pos.dist(enemy_base);
                if front.is_none_or(|(bd, _)| d < bd) {
                    front = Some((d, u.pos));
                }
            }
        }
        let l = Lane::ALL[lane];
        let p = match front {
            Some((_, p)) => p,
            None => self.map.lane_point(l, if team == 0 { 0.32 } else { 0.68 }),
        };
        let back = (own_base - p).norm();
        self.map.nearest_walkable(p + back * 70.0)
    }

    /// Enemy tower currently able to hurt something standing at `p` that has no creep cover.
    fn tower_threat_at(&self, team: u8, p: Vec2) -> Option<usize> {
        for (j, t) in self.units.iter().enumerate() {
            if t.alive && t.kind == Kind::Tower && t.team != team && t.pos.dist(p) <= t.range + 40.0 {
                // Towers prefer creeps: allied creeps inside the tower's range soak the shots.
                let covered =
                    self.units.iter().any(|c| c.alive && c.team == team && c.kind.is_lane_creep() && c.pos.dist(t.pos) <= t.range + 10.0);
                if !covered {
                    return Some(j);
                }
            }
        }
        None
    }

    fn think_bot(&mut self, ui: usize) {
        let hi = self.units[ui].hero as usize;
        self.heroes[hi].think_t -= DT;
        self.heroes[hi].shop_cd = (self.heroes[hi].shop_cd - DT).max(0.0);
        if self.heroes[hi].think_t > 0.0 {
            return;
        }
        self.heroes[hi].think_t = 0.2 + self.rng.f32() * 0.1;

        let (team, pos, id) = (self.units[ui].team, self.units[ui].pos, self.units[ui].id);
        let base = base_of(team);
        let lane = self.heroes[hi].lane as usize;

        self.bot_level_skills(ui);
        if self.near_shop(ui) {
            self.bot_shop(ui);
        }

        let (hp_frac, mana_frac, range, my_dmg) = {
            let u = &self.units[ui];
            (u.hp / u.max_hp, u.mana / u.max_mana.max(1.0), u.range, u.avg_damage())
        };

        // --- perceive
        let mut enemy_heroes: Vec<usize> = Vec::new();
        let mut enemy_creeps: Vec<usize> = Vec::new();
        let mut ally_creeps: Vec<usize> = Vec::new();
        let mut ally_heroes = 0;
        for (j, v) in self.units.iter().enumerate() {
            if !v.alive || j == ui {
                continue;
            }
            let d = pos.dist(v.pos);
            if d > 900.0 {
                continue;
            }
            if v.team == team {
                if v.kind.is_lane_creep() {
                    ally_creeps.push(j);
                } else if v.is_hero() {
                    ally_heroes += 1;
                }
            } else if v.team != TEAM_NEUTRAL && self.visible_to(team, j) {
                if v.is_hero() {
                    enemy_heroes.push(j);
                } else if v.kind.is_lane_creep() {
                    enemy_creeps.push(j);
                }
            }
        }
        enemy_heroes.sort_by(|&a, &b| pos.dist_sq(self.units[a].pos).total_cmp(&pos.dist_sq(self.units[b].pos)));

        let state = self.heroes[hi].ai_state;

        // --- state: retreating to the fountain
        if state == AI_STATE_RETREAT {
            let at_base = pos.dist(base) <= self.content.rules.shop_radius * 0.8;
            if at_base {
                if hp_frac > 0.9 && mana_frac > 0.55 {
                    self.heroes[hi].ai_state = AI_STATE_LANE;
                } else {
                    self.units[ui].order = Order::Stay;
                }
                return;
            }
            self.bot_escape(ui, &enemy_heroes, base);
            self.units[ui].order = Order::Move(base);
            return;
        }

        // --- state: walking home to spend gold
        if state == AI_STATE_SHOP {
            if self.near_shop(ui) {
                self.heroes[hi].ai_state = AI_STATE_LANE;
            } else if hp_frac < 0.3 && !enemy_heroes.is_empty() {
                self.heroes[hi].ai_state = AI_STATE_RETREAT;
            } else {
                self.units[ui].order = Order::Move(base);
            }
            return;
        }

        // --- lane state: decide whether to bail out
        let strongest_enemy_frac = enemy_heroes.iter().map(|&e| self.units[e].hp / self.units[e].max_hp).fold(0.0_f32, f32::max);
        if hp_frac < 0.33 || (hp_frac < 0.55 && !enemy_heroes.is_empty() && strongest_enemy_frac > hp_frac + 0.1) {
            self.heroes[hi].ai_state = AI_STATE_RETREAT;
            self.bot_escape(ui, &enemy_heroes, base);
            self.units[ui].order = Order::Move(base);
            return;
        }
        if let Some(cost) = self.next_item_cost(hi) {
            if self.heroes[hi].gold >= cost + 60.0 && self.heroes[hi].shop_cd <= 0.0 && enemy_heroes.is_empty() {
                self.heroes[hi].ai_state = AI_STATE_SHOP;
                self.units[ui].order = Order::Move(base);
                return;
            }
        }

        // Consumables.
        self.bot_use_consumables(ui, hp_frac, mana_frac, !enemy_heroes.is_empty());

        // --- tower danger: step back out of range when there is no creep cover
        if let Some(tj) = self.tower_threat_at(team, pos) {
            let tp = self.units[tj].pos;
            let away = (pos - tp).norm();
            let dest = self.map.nearest_walkable(tp + away * (self.units[tj].range + 120.0));
            self.units[ui].order = Order::Move(dest);
            return;
        }

        // --- fight: trade with / chase enemy heroes when it looks favourable
        if let Some(&target) = enemy_heroes.first() {
            let (tid, tpos, thp_frac) = {
                let t = &self.units[target];
                (t.id, t.pos, t.hp / t.max_hp)
            };
            let covered_dive = self.tower_threat_at(team, tpos).is_none();
            let outnumbered = enemy_heroes.len() > ally_heroes + 1;
            let dist = pos.dist(tpos);
            let reach = range.max(self.best_ability_range(ui)) + 140.0;
            if covered_dive && !outnumbered && dist <= reach && (hp_frac > 0.5 || thp_frac < 0.25) {
                self.bot_fight_abilities(ui, target, &enemy_heroes);
                if self.units[ui].order != Order::Attack(tid) {
                    self.units[ui].target = tid;
                    self.units[ui].order = Order::Attack(tid);
                }
                return;
            }
        }

        // --- last hitting & denying
        // Judge each creep by the hp it will have when our hit would land, not by its hp now:
        // a melee hero has to walk up and wind up, and the creeps keep hitting in the meantime.
        let reach_of = |s: &Game, e: usize| s.units[ui].range + s.units[e].radius + s.units[ui].radius * 0.5 + 35.0;
        let mut last_hit: Option<(f32, u32)> = None;
        let mut soft: Option<(f32, u32)> = None;
        let mut stalk: Option<(f32, usize)> = None;
        for &e in &enemy_creeps {
            let v = &self.units[e];
            let (vhp, vid, vpos) = (v.hp, v.id, v.pos);
            let exp = my_dmg * armor_multiplier(v.armor);
            let d = pos.dist(vpos);
            let t_hit = self.bot_hit_time(ui, e);
            let hp_at_hit = vhp - self.incoming_damage(e, ui, t_hit);
            if d <= reach_of(self, e) + 140.0 && hp_at_hit <= exp * 1.05 && hp_at_hit > 0.0 {
                if last_hit.is_none_or(|(h, _)| vhp < h) {
                    last_hit = Some((vhp, vid));
                }
            } else if d <= reach_of(self, e) && vhp <= exp * 2.3 {
                // Only chip at a creep when our next swing will still be in time for the kill.
                let at = self.units[ui].attack_time();
                let after = vhp - exp - self.incoming_damage(e, ui, t_hit + at);
                if after > 0.0 && soft.is_none_or(|(h, _)| vhp < h) {
                    soft = Some((vhp, vid));
                }
            }
            if d <= 520.0 && self.tower_threat_at(team, vpos).is_none() {
                let soon = vhp - self.incoming_damage(e, ui, 2.0);
                if stalk.is_none_or(|(h, _)| soon < h) {
                    stalk = Some((soon, e));
                }
            }
        }
        let mut deny: Option<u32> = None;
        for &a in &ally_creeps {
            let v = &self.units[a];
            if v.hp < v.max_hp * 0.5 && pos.dist(v.pos) <= reach_of(self, a) {
                let exp = my_dmg * armor_multiplier(v.armor);
                if v.hp <= exp * 1.05 && self.rng.f32() < 0.6 {
                    deny = Some(v.id);
                }
            }
        }
        let chosen = last_hit.map(|x| x.1).or(deny).or(soft.map(|x| x.1));
        if let Some(t) = chosen {
            self.units[ui].target = t;
            self.units[ui].order = Order::Attack(t);
            return;
        }

        // Harass: poke a hero standing within reach when it is safe.
        if let Some(&e) = enemy_heroes.first() {
            let (tid, tpos) = (self.units[e].id, self.units[e].pos);
            if pos.dist(tpos) <= range + 60.0 && self.tower_threat_at(team, tpos).is_none() && hp_frac > 0.6 {
                self.units[ui].target = tid;
                self.units[ui].order = Order::Attack(tid);
                return;
            }
        }

        // --- push a tower together with the creep wave
        if enemy_heroes.is_empty() {
            let mut best: Option<(f32, usize)> = None;
            for (j, t) in self.units.iter().enumerate() {
                if t.alive && t.kind.is_building() && t.team != team && !self.building_protected(j) {
                    let d = t.pos.dist(pos);
                    if d < 800.0 && best.is_none_or(|(bd, _)| d < bd) {
                        best = Some((d, j));
                    }
                }
            }
            if let Some((_, tj)) = best {
                let (tid, tpos, trange) = (self.units[tj].id, self.units[tj].pos, self.units[tj].range);
                let cover = ally_creeps.iter().filter(|&&c| self.units[c].pos.dist(tpos) <= trange + 80.0).count();
                if cover >= 2 && enemy_creeps.len() < 2 {
                    self.units[ui].target = tid;
                    self.units[ui].order = Order::Attack(tid);
                    return;
                }
            }
        }

        // --- wait for a last hit, or walk to the lane front
        if !enemy_creeps.is_empty() && enemy_creeps.iter().any(|&e| pos.dist(self.units[e].pos) < range + 220.0) {
            // Stand within reach of the creep that will die first, so the kill is one swing away.
            self.units[ui].target = 0;
            self.units[ui].order = Order::Stay;
            if let Some((_, e)) = stalk {
                let ep = self.units[e].pos;
                let want = reach_of(self, e) - 45.0;
                if pos.dist(ep) > want {
                    let dest = self.map.nearest_walkable(ep + (pos - ep).norm() * want);
                    self.units[ui].order = Order::Move(dest);
                }
            }
            return;
        }
        let hold = self.lane_hold_pos(team, lane);
        self.units[ui].target = 0;
        self.units[ui].order = Order::AttackMove(hold);
        let _ = id;
    }

    /// Seconds until a hit from bot `ui` on unit `ti` would land, starting now.
    fn bot_hit_time(&self, ui: usize, ti: usize) -> f32 {
        let u = &self.units[ui];
        let t = &self.units[ti];
        let d = u.pos.dist(t.pos);
        let reach = u.range + t.radius + u.radius * 0.5;
        let walk = (d - reach).max(0.0) / u.speed().max(1.0);
        let at = u.attack_time();
        let mut time = walk.max(u.attack_cd).max(u.swing_t) + (at * 0.30).max(0.1);
        if u.proj_speed > 0.0 {
            time += d.min(reach) / u.proj_speed;
        }
        time
    }

    /// Damage other units will deal to `ti` within the next `t` seconds (projectiles in flight
    /// plus the swings of everything attacking it), ignoring unit `skip`.
    fn incoming_damage(&self, ti: usize, skip: usize, t: f32) -> f32 {
        let (tid, tpos, tteam, mult) = {
            let v = &self.units[ti];
            (v.id, v.pos, v.team, armor_multiplier(v.armor))
        };
        let skip_id = self.units[skip].id;
        let mut total = 0.0_f32;
        for p in &self.projs {
            if p.homing == tid && p.src != skip_id && p.ability == 255 && p.pos.dist(tpos) <= p.speed * t {
                total += p.dmg * mult;
            }
        }
        for (j, a) in self.units.iter().enumerate() {
            if j == skip || !a.alive || a.team == tteam || a.dmg_max <= 0.0 {
                continue;
            }
            let swinging = a.swing_t > 0.0 && a.swing_target == tid;
            if !swinging && (a.target != tid || !self.in_attack_range(j, ti)) {
                continue;
            }
            let at = a.attack_time();
            let travel = if a.proj_speed > 0.0 { a.pos.dist(tpos) / a.proj_speed } else { 0.0 };
            let mut hit = if swinging { a.swing_t } else { a.attack_cd + (at * 0.30).max(0.1) } + travel;
            let mut n = 0;
            while hit <= t && n < 8 {
                total += a.avg_damage() * mult;
                hit += at;
                n += 1;
            }
        }
        total
    }

    fn best_ability_range(&self, ui: usize) -> f32 {
        let mut r = 0.0_f32;
        for slot in 0..4 {
            if let Some((ab, l)) = self.ability_def(ui, slot) {
                if self.ability_ready(ui, slot) {
                    let cr = match ab.cast {
                        Cast::Target | Cast::Line | Cast::PointAoE => ab.range[l],
                        Cast::SelfAoE => ab.radius[l],
                        Cast::Blink if ab.aoe_on_land => ab.range[l] + ab.radius[l] * 0.5,
                        _ => 0.0,
                    };
                    r = r.max(cr);
                }
            }
        }
        r
    }

    fn bot_use_consumables(&mut self, ui: usize, hp_frac: f32, mana_frac: f32, enemy_near: bool) {
        let hi = self.units[ui].hero as usize;
        for slot in 0..INV_SLOTS {
            let s = self.heroes[hi].items[slot];
            if s.id == 0 || s.cd > 0.0 {
                continue;
            }
            match self.content.items[s.id as usize].active {
                ACT_SALVE if hp_frac < 0.65 && !enemy_near && self.units[ui].salve_t <= 0.0 => {
                    self.use_item(ui, slot, Vec2::ZERO);
                }
                ACT_MANA if mana_frac < 0.3 && self.units[ui].mana_pot_t <= 0.0 => {
                    self.use_item(ui, slot, Vec2::ZERO);
                }
                _ => {}
            }
        }
    }

    /// Panic buttons while running home: blink away, sprint, etc.
    fn bot_escape(&mut self, ui: usize, enemy_heroes: &[usize], base: Vec2) {
        let pos = self.units[ui].pos;
        let danger = enemy_heroes.iter().any(|&e| self.units[e].pos.dist(pos) < 500.0);
        let hi = self.units[ui].hero as usize;
        if danger {
            for slot in 0..4 {
                if let Some((ab, _)) = self.ability_def(ui, slot) {
                    if !self.ability_ready(ui, slot) {
                        continue;
                    }
                    let fire = match ab.cast {
                        Cast::Blink => !ab.aoe_on_land,
                        Cast::SelfBuff => ab.fx & FX_HASTE != 0,
                        _ => false,
                    };
                    if fire && self.cast_ability(ui, slot, 0, base) {
                        return;
                    }
                }
            }
            for slot in 0..INV_SLOTS {
                let s = self.heroes[hi].items[slot];
                if s.id != 0 && s.cd <= 0.0 && self.content.items[s.id as usize].active == ACT_BLINK && self.use_item(ui, slot, base) {
                    return;
                }
            }
        }
        let hp_frac = self.units[ui].hp / self.units[ui].max_hp;
        self.bot_use_consumables(ui, hp_frac, self.units[ui].mana / self.units[ui].max_mana.max(1.0), danger);
    }

    /// Fire off whatever fits the situation against `target` (one spell per think).
    fn bot_fight_abilities(&mut self, ui: usize, target: usize, enemy_heroes: &[usize]) {
        let pos = self.units[ui].pos;
        let (tid, tpos, tr, tstun, troot, tslow, tface) = {
            let t = &self.units[target];
            (t.id, t.pos, t.radius, t.stun_t, t.root_t, t.slow_t, t.facing)
        };
        let dist = pos.dist(tpos);
        let hp_frac = self.units[ui].hp / self.units[ui].max_hp;
        for slot in 0..4 {
            if !self.ability_ready(ui, slot) {
                continue;
            }
            let (ab, l) = self.ability_def(ui, slot).unwrap();
            let range = ab.range[l];
            let radius = ab.radius[l];
            let fired = match ab.cast {
                Cast::Target => dist <= range + tr && self.cast_ability(ui, slot, tid, tpos),
                Cast::Line => dist <= range * 0.92 && self.cast_ability(ui, slot, 0, tpos),
                Cast::PointAoE => {
                    if dist > range {
                        false
                    } else if ab.delay > 0.0 {
                        // Telegraphed spells: only when the target is locked down or slowed,
                        // otherwise lead a walking target.
                        let locked = tstun > ab.delay * 0.6 || troot > ab.delay * 0.6;
                        let lead = if locked {
                            Vec2::ZERO
                        } else {
                            Vec2::new(tface.cos(), tface.sin()) * (self.units[target].speed() * ab.delay * 0.6)
                        };
                        (locked || tslow > 0.0 || self.rng.f32() < 0.35) && self.cast_ability(ui, slot, 0, tpos + lead)
                    } else {
                        self.cast_ability(ui, slot, 0, tpos)
                    }
                }
                Cast::SelfAoE => dist <= radius * 0.95 && self.cast_ability(ui, slot, 0, pos),
                Cast::SelfBuff => dist <= 520.0 && ab.fx & FX_ASPD != 0 && self.cast_ability(ui, slot, 0, pos),
                Cast::Blink => {
                    if ab.aoe_on_land {
                        dist > 160.0
                            && dist <= range + radius * 0.5
                            && hp_frac > 0.5
                            && !enemy_heroes.is_empty()
                            && self.cast_ability(ui, slot, 0, tpos)
                    } else {
                        false
                    }
                }
                Cast::None => false,
            };
            if fired {
                return;
            }
        }
        // Sprint-style speed buffs help chase a fleeing, wounded hero.
        if dist > 260.0 {
            for slot in 0..4 {
                if let Some((ab, _)) = self.ability_def(ui, slot) {
                    if ab.cast == Cast::SelfBuff && ab.fx & FX_HASTE != 0 && self.ability_ready(ui, slot) {
                        self.cast_ability(ui, slot, 0, pos);
                        return;
                    }
                }
            }
        }
    }
}
