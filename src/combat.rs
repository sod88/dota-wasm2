//! Per-unit simulation: timers, order execution, movement, attacks, damage and deaths.

use crate::data::*;
use crate::game::*;
use crate::map::*;
use crate::math::*;
use crate::unit::*;

impl Game {
    // =================================================================== timers & regen

    pub fn update_timers(&mut self, i: usize) {
        let near_fountain;
        {
            let u = &self.units[i];
            near_fountain = u.is_hero() && u.pos.dist(base_of(u.team)) <= self.content.rules.shop_radius;
        }
        {
            let u = &mut self.units[i];
            u.attack_cd = (u.attack_cd - DT).max(0.0);
            u.since_dmg += DT;
            u.repath_t = (u.repath_t - DT).max(0.0);
            u.stun_t = (u.stun_t - DT).max(0.0);
            u.root_t = (u.root_t - DT).max(0.0);
            u.slow_t = (u.slow_t - DT).max(0.0);
            u.haste_t = (u.haste_t - DT).max(0.0);
            u.aspd_t = (u.aspd_t - DT).max(0.0);
            u.force_t = (u.force_t - DT).max(0.0);

            let mut hp_gain = u.hp_regen;
            let mut mana_gain = u.mana_regen;
            if u.salve_t > 0.0 {
                u.salve_t -= DT;
                hp_gain += u.salve_rate;
            }
            if u.mana_pot_t > 0.0 {
                u.mana_pot_t -= DT;
                mana_gain += u.mana_pot_rate;
            }
            if near_fountain {
                hp_gain += u.max_hp * 0.08;
                mana_gain += u.max_mana * 0.10;
            }
            if u.kind == Kind::Ancient && u.since_dmg > 5.0 {
                hp_gain += 12.0;
            }
            u.hp = (u.hp + hp_gain * DT).min(u.max_hp);
            u.mana = (u.mana + mana_gain * DT).min(u.max_mana);
        }
        let hi = self.units[i].hero;
        if hi >= 0 {
            let h = &mut self.heroes[hi as usize];
            for c in h.ab_cd.iter_mut() {
                *c = (*c - DT).max(0.0);
            }
            for s in h.items.iter_mut() {
                s.cd = (s.cd - DT).max(0.0);
            }
            for r in h.recent.iter_mut() {
                r.1 += DT;
            }
            h.recent.retain(|r| r.1 < 15.0);
        }
    }

    // =================================================================== target rules

    /// Can unit `i` legally attack unit `ti` right now (ignoring range)?
    pub fn can_attack(&self, i: usize, ti: usize) -> bool {
        let a = &self.units[i];
        let t = &self.units[ti];
        if !a.alive || !t.alive || a.id == t.id || a.dmg_max <= 0.0 {
            return false;
        }
        if t.is_building() && self.building_protected(ti) {
            return false;
        }
        if a.team == t.team {
            // Deny: heroes may finish off their own wounded creeps.
            return a.is_hero() && t.kind.is_lane_creep() && t.hp < t.max_hp * 0.5;
        }
        true
    }

    /// Validate a remembered target: alive, attackable, visible and not too far away.
    pub fn target_ok(&self, i: usize, tid: u32, max_edge_dist: f32) -> bool {
        let ti = match self.live_idx(tid) {
            Some(t) => t,
            None => return false,
        };
        if !self.can_attack(i, ti) {
            return false;
        }
        let a = &self.units[i];
        if !self.visible_to(a.team, ti) {
            return false;
        }
        a.pos.dist(self.units[ti].pos) - self.units[ti].radius <= max_edge_dist
    }

    /// Pick the best enemy within `radius` (edge distance) of unit `i`.
    pub fn acquire(&self, i: usize, radius: f32, allow_neutral: bool) -> u32 {
        let a = &self.units[i];
        let mut best = (f32::MAX, 0u32);
        for (j, t) in self.units.iter().enumerate() {
            if !t.alive || j == i || t.team == a.team {
                continue;
            }
            if t.team == TEAM_NEUTRAL && !allow_neutral {
                continue;
            }
            if t.is_building() && self.building_protected(j) {
                continue;
            }
            if !self.visible_to(a.team, j) {
                continue;
            }
            let d = a.pos.dist(t.pos) - t.radius;
            if d > radius {
                continue;
            }
            let mut score = d;
            if t.is_building() {
                score += 600.0; // fight units before structures
            }
            if t.is_hero() && a.kind.is_lane_creep() {
                score += 80.0; // creeps prefer creeps
            }
            if score < best.0 {
                best = (score, t.id);
            }
        }
        best.1
    }

    // =================================================================== order execution

    pub fn act(&mut self, i: usize) {
        if self.units[i].stunned() {
            self.units[i].swing_t = 0.0;
            return;
        }
        if self.units[i].swing_t > 0.0 {
            self.units[i].swing_t -= DT;
            if self.units[i].swing_t <= 0.0 {
                self.finish_swing(i);
            }
            return;
        }
        let order = self.units[i].order;
        match order {
            Order::Stay => {}
            Order::Idle => self.act_idle(i, false),
            Order::Hold => self.act_idle(i, true),
            Order::Move(to) => {
                if self.move_toward(i, to) {
                    self.units[i].order = Order::Idle;
                }
            }
            Order::AttackMove(to) => self.act_attack_move(i, to),
            Order::Attack(t) => {
                if self.target_ok(i, t, f32::MAX) {
                    self.units[i].target = t;
                    self.attack_or_chase(i, t);
                } else {
                    self.units[i].order = Order::Idle;
                    self.units[i].target = 0;
                }
            }
            Order::Cast { slot, target, point } => self.act_cast(i, slot, target, point),
        }
    }

    fn acquire_radius(&self, i: usize) -> f32 {
        let u = &self.units[i];
        match u.kind {
            Kind::Hero => (u.range + 130.0).max(380.0),
            Kind::Tower => u.range,
            _ => 330.0_f32.max(u.range + 40.0),
        }
    }

    fn act_idle(&mut self, i: usize, hold: bool) {
        let u = &self.units[i];
        let reach = if hold { u.range } else { u.range + 50.0 };
        let mut tid = u.target;
        if !self.target_ok(i, tid, reach + 20.0) {
            tid = self.acquire(i, reach, false);
            self.units[i].target = tid;
        }
        if tid != 0 {
            if hold {
                // Hold position: only swing if in range, never walk.
                if let Some(ti) = self.live_idx(tid) {
                    let in_range = self.in_attack_range(i, ti);
                    if in_range {
                        self.attack_or_chase(i, tid);
                    } else {
                        self.units[i].target = 0;
                    }
                }
            } else {
                self.attack_or_chase(i, tid);
            }
        }
    }

    fn act_attack_move(&mut self, i: usize, to: Vec2) {
        let acq = self.acquire_radius(i);
        // A human hero a-moving into a camp should attack it. Bots never jungle on purpose, and
        // letting them auto-target neutrals at the edge of vision made them flip-flop in place.
        let allow_neutral = {
            let u = &self.units[i];
            u.is_hero() && u.hero >= 0 && !self.heroes[u.hero as usize].bot
        };
        let mut tid = self.units[i].target;
        if !self.target_ok(i, tid, acq * 1.25) {
            tid = self.acquire(i, acq, allow_neutral);
            self.units[i].target = tid;
        }
        if tid != 0 {
            self.attack_or_chase(i, tid);
        } else if self.move_toward(i, to) {
            self.units[i].order = Order::Idle;
        }
    }

    pub fn in_attack_range(&self, i: usize, ti: usize) -> bool {
        let u = &self.units[i];
        let t = &self.units[ti];
        u.pos.dist(t.pos) <= u.range + t.radius + u.radius * 0.5
    }

    pub fn face(&mut self, i: usize, p: Vec2) {
        let d = p - self.units[i].pos;
        if d.len_sq() > 1e-4 {
            self.units[i].facing = d.y.atan2(d.x);
        }
    }

    fn attack_or_chase(&mut self, i: usize, tid: u32) {
        let ti = match self.live_idx(tid) {
            Some(t) => t,
            None => return,
        };
        let tp = self.units[ti].pos;
        if self.in_attack_range(i, ti) {
            self.face(i, tp);
            self.units[i].clear_path();
            if self.units[i].attack_cd <= 0.0 {
                let u = &mut self.units[i];
                let at = u.attack_time();
                u.swing_len = (at * 0.30).max(0.1);
                u.swing_t = u.swing_len;
                u.swing_target = tid;
                u.attack_cd = at;
                let id = u.id;
                self.emit([EV_ATTACK, id as f32, tid as f32, tp.x, tp.y, 0.0, 0.0, 0.0]);
            }
        } else if self.units[i].kind != Kind::Tower {
            self.move_toward(i, tp);
        }
    }

    /// The hit lands: melee deals damage immediately, ranged units launch a homing projectile.
    fn finish_swing(&mut self, i: usize) {
        let tid = self.units[i].swing_target;
        let ti = match self.live_idx(tid) {
            Some(t) => t,
            None => return,
        };
        // The target may have walked away during the wind-up.
        let u = &self.units[i];
        let t = &self.units[ti];
        let reach = u.range + t.radius + u.radius * 0.5;
        if u.pos.dist(t.pos) > reach * 1.6 + 20.0 || !self.can_attack(i, ti) {
            return;
        }
        let mut dmg = self.rng.range(u.dmg_min, u.dmg_max + 1.0);
        if t.is_building() {
            dmg *= u.bldg_mult;
        }
        let (src, team, pos, speed) = (u.id, u.team, u.pos, u.proj_speed);
        let tpos = t.pos;
        if speed > 0.0 {
            let id = self.alloc_id();
            let dir = (tpos - pos).norm();
            self.projs.push(Proj {
                id,
                src,
                team,
                pos,
                dir,
                speed,
                homing: tid,
                dmg,
                dtype: DmgType::Physical,
                fx: 0,
                dur: 0.0,
                pct: 0.0,
                ability: 255,
                radius: 0.0,
                remaining: f32::MAX,
                hits: Vec::new(),
            });
        } else {
            self.deal_damage(src, ti, dmg, DmgType::Physical);
        }
    }

    // =================================================================== movement

    /// Walk towards `goal`. Returns true once the unit has arrived.
    pub fn move_toward(&mut self, i: usize, goal: Vec2) -> bool {
        let pos = self.units[i].pos;
        if pos.dist(goal) < 5.0 {
            self.units[i].clear_path();
            return true;
        }
        let speed = self.units[i].speed();
        if speed <= 0.0 {
            return false;
        }
        let mut step = speed * DT;
        if self.map.los(pos, goal) {
            self.units[i].clear_path();
            let next = pos.move_toward(goal, step);
            self.apply_move(i, next);
            return self.units[i].pos.dist(goal) < 5.0;
        }
        // Path around obstacles (re-plan when the goal moved or the route ran out).
        let need_path = {
            let u = &self.units[i];
            u.path_i >= u.path.len() || u.path_goal.dist(goal) > CELL * 1.5
        };
        if need_path && self.units[i].repath_t <= 0.0 {
            let found = self.finder.find(&self.map, pos, goal);
            let u = &mut self.units[i];
            u.repath_t = 0.4;
            u.path_goal = goal;
            u.path_i = 0;
            u.path = found.unwrap_or_default();
        }
        // Consume waypoints along this tick's movement budget.
        let mut guard = 0;
        while step > 0.0 && guard < 4 {
            guard += 1;
            let (wp, p) = {
                let u = &self.units[i];
                if u.path_i >= u.path.len() {
                    return false;
                }
                (u.path[u.path_i], u.pos)
            };
            let d = p.dist(wp);
            if d <= step {
                self.apply_move(i, wp);
                step -= d;
                self.units[i].path_i += 1;
            } else {
                let next = p.move_toward(wp, step);
                self.apply_move(i, next);
                step = 0.0;
            }
        }
        false
    }

    /// Move to `next` if walkable, sliding along walls otherwise.
    fn apply_move(&mut self, i: usize, next: Vec2) {
        let old = self.units[i].pos;
        let d = next - old;
        if d.len_sq() < 1e-8 {
            return;
        }
        let target = if self.map.walkable(next) {
            next
        } else if self.map.walkable(Vec2::new(next.x, old.y)) {
            Vec2::new(next.x, old.y)
        } else if self.map.walkable(Vec2::new(old.x, next.y)) {
            Vec2::new(old.x, next.y)
        } else {
            old
        };
        let u = &mut self.units[i];
        u.pos = target;
        u.facing = d.y.atan2(d.x);
    }

    /// Soft collision: push overlapping units apart (buildings never move).
    pub fn separate_units(&mut self) {
        let n = self.units.len();
        let mut push: Vec<Vec2> = vec![Vec2::ZERO; n];
        for a in 0..n {
            if !self.units[a].alive {
                continue;
            }
            for b in (a + 1)..n {
                if !self.units[b].alive {
                    continue;
                }
                let (ua, ub) = (&self.units[a], &self.units[b]);
                if ua.is_building() && ub.is_building() {
                    continue;
                }
                let dx = ub.pos.x - ua.pos.x;
                if dx.abs() > 80.0 {
                    continue;
                }
                let dy = ub.pos.y - ua.pos.y;
                let min = (ua.radius + ub.radius) * 0.9;
                let d2 = dx * dx + dy * dy;
                if d2 >= min * min {
                    continue;
                }
                let d = d2.sqrt();
                let dir = if d > 1e-3 {
                    Vec2::new(dx / d, dy / d)
                } else {
                    // Exactly stacked: separate along a deterministic pseudo-random axis.
                    let ang = ((ua.id.wrapping_mul(2654435761) ^ ub.id) % 628) as f32 / 100.0;
                    Vec2::new(ang.cos(), ang.sin())
                };
                let overlap = (min - d).min(6.0);
                let (ma, mb) = (mass(ua), mass(ub));
                let total = ma + mb;
                // Heavier units get pushed less; buildings (infinite mass) not at all.
                let (fa, fb) = if ua.is_building() {
                    (0.0, 1.0)
                } else if ub.is_building() {
                    (1.0, 0.0)
                } else {
                    (mb / total, ma / total)
                };
                push[a] -= dir * (overlap * fa);
                push[b] += dir * (overlap * fb);
            }
        }
        for i in 0..n {
            if push[i].len_sq() < 1e-8 {
                continue;
            }
            let np = self.units[i].pos + push[i];
            if self.map.walkable(np) {
                self.units[i].pos = np;
            }
        }
    }

    // =================================================================== damage & death

    /// Apply damage (after resistances). Returns the damage actually dealt.
    pub fn deal_damage(&mut self, src: u32, di: usize, amount: f32, dtype: DmgType) -> f32 {
        if !self.units[di].alive || amount <= 0.0 {
            return 0.0;
        }
        if self.units[di].is_building() && self.building_protected(di) {
            return 0.0;
        }
        let (vid, vteam, vkind, vpos, varmor) = {
            let u = &self.units[di];
            (u.id, u.team, u.kind, u.pos, u.armor)
        };
        let mult = match dtype {
            DmgType::Physical => armor_multiplier(varmor),
            DmgType::Magical => 1.0 - self.content.rules.magic_resist,
            DmgType::Pure => 1.0,
        };
        let dmg = amount * mult;
        {
            let u = &mut self.units[di];
            u.hp -= dmg;
            u.since_dmg = 0.0;
            u.salve_t = 0.0; // damage breaks the healing salve
        }
        self.emit([EV_DAMAGE, vid as f32, src as f32, vpos.x, vpos.y, dmg, dtype as u8 as f32, vteam as f32]);

        let src_idx = self.idx(src);
        let src_info = src_idx.map(|s| (self.units[s].team, self.units[s].is_hero()));
        if let Some((steam, shero)) = src_info {
            if steam != vteam {
                // Hero-on-hero fights: remember for assists and pull enemy towers onto the attacker.
                if shero && vkind == Kind::Hero {
                    let hi = self.units[di].hero as usize;
                    let rec = &mut self.heroes[hi].recent;
                    if let Some(r) = rec.iter_mut().find(|r| r.0 == src) {
                        r.1 = 0.0;
                    } else {
                        rec.push((src, 0.0));
                    }
                    for t in self.units.iter_mut() {
                        if t.alive && t.kind == Kind::Tower && t.team == vteam && t.pos.dist(vpos) <= t.range + 30.0 {
                            t.force_target = src;
                            t.force_t = 3.0;
                        }
                    }
                }
                // Neutrals retaliate, and the whole camp joins in.
                if vkind == Kind::Neutral {
                    let camp = self.units[di].camp;
                    if camp >= 0 {
                        let members = self.camps[camp as usize].members.clone();
                        for m in members {
                            if let Some(mi) = self.live_idx(m) {
                                if self.units[mi].target == 0 || !self.units[mi].returning {
                                    self.units[mi].target = src;
                                    self.units[mi].returning = false;
                                }
                            }
                        }
                    }
                }
            }
        }
        if self.units[di].hp <= 0.0 {
            self.kill_unit(di, src);
        }
        dmg
    }

    /// Damage plus any stun / root / slow carried by an ability or projectile.
    pub fn apply_effects(&mut self, src: u32, di: usize, dmg: f32, dtype: DmgType, fx: u8, dur: f32, pct: f32) {
        if dmg > 0.0 {
            self.deal_damage(src, di, dmg, dtype);
        }
        let u = &mut self.units[di];
        if !u.alive {
            return;
        }
        if fx & FX_STUN != 0 {
            u.stun_t = u.stun_t.max(dur);
            u.swing_t = 0.0;
        }
        if fx & FX_ROOT != 0 {
            u.root_t = u.root_t.max(dur);
        }
        if fx & FX_SLOW != 0 {
            if u.slow_t <= 0.0 {
                u.slow_pct = pct;
            } else {
                u.slow_pct = u.slow_pct.max(pct);
            }
            u.slow_t = u.slow_t.max(dur);
        }
    }

    fn award_xp(&mut self, team: u8, center: Vec2, xp: f32) {
        let mut rec = Vec::new();
        for (i, u) in self.units.iter().enumerate() {
            if u.alive && u.is_hero() && u.team == team && u.pos.dist(center) <= self.content.rules.xp_radius {
                rec.push(i);
            }
        }
        if rec.is_empty() {
            return;
        }
        let share = xp / rec.len() as f32;
        for i in rec {
            self.add_xp(i, share);
        }
    }

    fn award_gold(&mut self, ui: usize, gold: f32, at: Vec2) {
        if gold <= 0.0 {
            return;
        }
        self.add_gold(ui, gold);
        let id = self.units[ui].id;
        self.emit([EV_GOLD, id as f32, 0.0, at.x, at.y, gold, 0.0, 0.0]);
    }

    pub fn kill_unit(&mut self, di: usize, killer_id: u32) {
        if !self.units[di].alive {
            return;
        }
        let (vid, vkind, vteam, vpos, vgold, vxp) = {
            let u = &mut self.units[di];
            u.alive = false;
            u.hp = 0.0;
            u.swing_t = 0.0;
            u.order = Order::Idle;
            (u.id, u.kind, u.team, u.pos, u.bounty_gold, u.bounty_xp)
        };
        let killer = self.idx(killer_id);
        let (kteam, khero) = match killer {
            Some(k) => (self.units[k].team, self.units[k].is_hero()),
            None => (255, false),
        };
        let enemy_team = 1 - vteam.min(1);
        let mut gold_given = 0.0;

        match vkind {
            Kind::Hero => {
                let hi = self.units[di].hero as usize;
                let level = self.heroes[hi].level as f32;
                let rules = &self.content.rules;
                {
                    let h = &mut self.heroes[hi];
                    h.deaths += 1;
                    h.respawn_t = (rules.respawn_base + level * rules.respawn_per_level).min(rules.respawn_max);
                    h.ai_state = 0;
                }
                let bounty = rules.hero_bounty_base + rules.hero_bounty_per_level * level;
                let xp = rules.hero_xp_base + rules.hero_xp_per_level * level;
                let mut assist_ids: Vec<u32> = Vec::new();
                for r in &self.heroes[hi].recent {
                    if r.0 != killer_id && r.1 <= 12.0 {
                        assist_ids.push(r.0);
                    }
                }
                self.heroes[hi].recent.clear();
                if khero && kteam != vteam {
                    let k = killer.unwrap();
                    self.heroes[self.units[k].hero as usize].kills += 1;
                    self.award_gold(k, bounty, vpos);
                    gold_given = bounty;
                    for aid in assist_ids {
                        if let Some(ai) = self.idx(aid) {
                            if self.units[ai].team == kteam {
                                self.heroes[self.units[ai].hero as usize].assists += 1;
                                self.award_gold(ai, bounty * 0.3, vpos);
                                self.emit([EV_ASSIST, aid as f32, vid as f32, vpos.x, vpos.y, 0.0, 0.0, 0.0]);
                            }
                        }
                    }
                }
                self.award_xp(enemy_team, vpos, xp);
            }
            Kind::Melee | Kind::Ranged | Kind::Siege => {
                if khero && kteam == vteam {
                    // Deny: the enemy gets half XP and nobody gets gold.
                    let k = killer.unwrap();
                    self.heroes[self.units[k].hero as usize].denies += 1;
                    self.award_xp(enemy_team, vpos, vxp * 0.5);
                } else {
                    if khero && kteam != vteam {
                        let k = killer.unwrap();
                        self.heroes[self.units[k].hero as usize].last_hits += 1;
                        self.award_gold(k, vgold, vpos);
                        gold_given = vgold;
                    }
                    self.award_xp(enemy_team, vpos, vxp);
                }
            }
            Kind::Neutral => {
                if khero {
                    let k = killer.unwrap();
                    self.heroes[self.units[k].hero as usize].last_hits += 1;
                    self.award_gold(k, vgold, vpos);
                    gold_given = vgold;
                    self.award_xp(kteam, vpos, vxp);
                }
            }
            Kind::Tower | Kind::Ancient => {
                // Everyone on the destroying team shares the bounty.
                if kteam <= 1 && kteam != vteam {
                    let ids: Vec<usize> = self
                        .units
                        .iter()
                        .enumerate()
                        .filter(|(_, u)| u.is_hero() && u.team == kteam)
                        .map(|(i, _)| i)
                        .collect();
                    for ui in ids {
                        self.add_gold(ui, vgold);
                        let id = self.units[ui].id;
                        self.emit([EV_GOLD, id as f32, 0.0, vpos.x, vpos.y, vgold, 0.0, 0.0]);
                    }
                    gold_given = vgold;
                }
                self.emit([EV_BUILDING, vid as f32, vkind as u8 as f32, vpos.x, vpos.y, 0.0, 0.0, vteam as f32]);
                self.recompute_tower_flags();
                if vkind == Kind::Ancient {
                    self.winner = enemy_team as i32;
                    self.emit([EV_GAMEOVER, enemy_team as f32, 0.0, vpos.x, vpos.y, 0.0, 0.0, 0.0]);
                }
            }
        }
        self.emit([EV_KILL, vid as f32, killer_id as f32, vpos.x, vpos.y, gold_given, vkind as u8 as f32, vteam as f32]);
    }

    // =================================================================== projectiles & zones

    pub fn update_projectiles(&mut self) {
        let ps = std::mem::take(&mut self.projs);
        let mut keep: Vec<Proj> = Vec::with_capacity(ps.len());
        for mut p in ps {
            let step = p.speed * DT;
            if p.homing != 0 {
                let ti = match self.live_idx(p.homing) {
                    Some(t) => t,
                    None => continue, // target died: the projectile fizzles
                };
                let (tp, tr) = (self.units[ti].pos, self.units[ti].radius);
                if p.pos.dist(tp) <= step + tr * 0.6 + 4.0 {
                    if p.ability == 255 {
                        self.deal_damage(p.src, ti, p.dmg, p.dtype);
                    } else {
                        self.apply_effects(p.src, ti, p.dmg, p.dtype, p.fx, p.dur, p.pct);
                    }
                    continue;
                }
                p.pos = p.pos.move_toward(tp, step);
                p.dir = (tp - p.pos).norm();
                keep.push(p);
            } else {
                let old = p.pos;
                let adv = step.min(p.remaining);
                p.pos = old + p.dir * adv;
                p.remaining -= adv;
                for ti in 0..self.units.len() {
                    let t = &self.units[ti];
                    if !t.alive || t.team == p.team || t.is_building() || p.hits.contains(&t.id) {
                        continue;
                    }
                    let q = closest_point_on_segment(t.pos, old, p.pos);
                    if q.dist(t.pos) <= p.radius + t.radius {
                        p.hits.push(t.id);
                        self.apply_effects(p.src, ti, p.dmg, p.dtype, p.fx, p.dur, p.pct);
                    }
                }
                if p.remaining > 0.01 {
                    keep.push(p);
                }
            }
        }
        // Anything launched while resolving hits is appended after the survivors.
        let launched = std::mem::take(&mut self.projs);
        self.projs = keep;
        self.projs.extend(launched);
    }

    pub fn update_zones(&mut self) {
        let zs = std::mem::take(&mut self.zones);
        let mut keep = Vec::with_capacity(zs.len());
        for mut z in zs {
            z.t_left -= DT;
            if z.t_left <= 0.0 {
                self.aoe_hit(z.src, z.team, z.pos, z.radius, z.dmg, z.dtype, z.fx, z.dur, z.pct);
            } else {
                keep.push(z);
            }
        }
        let added = std::mem::take(&mut self.zones);
        self.zones = keep;
        self.zones.extend(added);
    }
}

fn mass(u: &Unit) -> f32 {
    match u.kind {
        Kind::Hero => 3.0,
        Kind::Neutral => 2.0,
        _ => 1.0,
    }
}
