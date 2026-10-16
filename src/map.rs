//! The battlefield: a walkability grid, three lanes, a river and jungle camps.
//!
//! Layout (y grows downwards, like the canvas): Radiant base bottom-left, Dire base
//! top-right. Lanes are polylines running Radiant -> Dire. The map is point-symmetric
//! about its centre, so both sides are perfectly fair.

use crate::math::{closest_point_on_segment, Vec2};

pub const CELL: f32 = 24.0;
pub const GRID: usize = 100;
pub const WORLD: f32 = CELL * GRID as f32; // 2400 world units

pub const BASE_RADIANT: Vec2 = Vec2::new(280.0, 2120.0);
pub const BASE_DIRE: Vec2 = Vec2::new(2120.0, 280.0);

/// Terrain codes. Higher value wins when shapes overlap while carving.
pub const T_BLOCKED: u8 = 0;
pub const T_GROUND: u8 = 1;
pub const T_LANE: u8 = 2;
pub const T_RIVER: u8 = 3;

const LANE_HALF_WIDTH: f32 = 72.0;
const RIVER_HALF_WIDTH: f32 = 50.0;
const BASE_RADIUS: f32 = 250.0;

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Lane {
    Top,
    Mid,
    Bot,
}

impl Lane {
    pub const ALL: [Lane; 3] = [Lane::Top, Lane::Mid, Lane::Bot];
    pub fn idx(self) -> usize {
        match self {
            Lane::Top => 0,
            Lane::Mid => 1,
            Lane::Bot => 2,
        }
    }
}

#[derive(Clone, Copy, Debug)]
pub struct CampSpawn {
    pub pos: Vec2,
    /// 0 = small, 1 = medium, 2 = large
    pub tier: u8,
}

pub struct Map {
    pub terrain: Vec<u8>,
    /// Lane polylines, ordered Radiant base -> Dire base.
    pub lanes: [Vec<Vec2>; 3],
    pub camps: Vec<CampSpawn>,
}

impl Map {
    pub fn generate() -> Map {
        let mut m = Map { terrain: vec![T_BLOCKED; GRID * GRID], lanes: [Vec::new(), Vec::new(), Vec::new()], camps: Vec::new() };
        let corner_tl = Vec2::new(280.0, 280.0);
        let corner_br = Vec2::new(2120.0, 2120.0);
        m.lanes[Lane::Top.idx()] = vec![BASE_RADIANT, corner_tl, BASE_DIRE];
        m.lanes[Lane::Mid.idx()] = vec![BASE_RADIANT, BASE_DIRE];
        m.lanes[Lane::Bot.idx()] = vec![BASE_RADIANT, corner_br, BASE_DIRE];

        // Segments jungle corridors may connect to: every lane segment plus the river.
        let mut connectors: Vec<(Vec2, Vec2)> = vec![(corner_tl, corner_br)];
        for lane in &m.lanes {
            for w in lane.windows(2) {
                connectors.push((w[0], w[1]));
            }
        }

        m.carve_circle(BASE_RADIANT, BASE_RADIUS, T_GROUND);
        m.carve_circle(BASE_DIRE, BASE_RADIUS, T_GROUND);

        // Radiant-half camps; the Dire half is the point-mirror image.
        let radiant_camps: [(f32, f32, u8); 6] =
            [(600.0, 1000.0, 0), (520.0, 1450.0, 1), (900.0, 1250.0, 2), (1250.0, 1750.0, 1), (1650.0, 1900.0, 0), (1350.0, 1550.0, 2)];
        for &(x, y, tier) in &radiant_camps {
            for pos in [Vec2::new(x, y), Vec2::new(WORLD - x, WORLD - y)] {
                m.carve_circle(pos, 70.0, T_GROUND);
                let mut best = (f32::MAX, pos);
                for &(a, b) in &connectors {
                    let q = closest_point_on_segment(pos, a, b);
                    let d = pos.dist(q);
                    if d < best.0 {
                        best = (d, q);
                    }
                }
                m.carve_capsule(pos, best.1, 40.0, T_GROUND);
                m.camps.push(CampSpawn { pos, tier });
            }
        }

        for lane in m.lanes.clone().iter() {
            for w in lane.windows(2) {
                m.carve_capsule(w[0], w[1], LANE_HALF_WIDTH, T_LANE);
            }
        }
        m.carve_capsule(corner_tl, corner_br, RIVER_HALF_WIDTH, T_RIVER);

        // Rocks: a pair in the river (forces detours) and one in each base.
        for &(x, y, r) in &[(800.0, 800.0, 34.0), (1600.0, 1600.0, 34.0), (150.0, 1950.0, 34.0), (2250.0, 450.0, 34.0)] {
            m.block_circle(Vec2::new(x, y), r);
        }
        m
    }

    fn carve_capsule(&mut self, a: Vec2, b: Vec2, half_width: f32, value: u8) {
        let r2 = half_width * half_width;
        for cy in 0..GRID {
            for cx in 0..GRID {
                let c = Self::cell_center(cx as i32, cy as i32);
                let q = closest_point_on_segment(c, a, b);
                if c.dist_sq(q) <= r2 {
                    let t = &mut self.terrain[cy * GRID + cx];
                    if value > *t {
                        *t = value;
                    }
                }
            }
        }
    }

    fn carve_circle(&mut self, c: Vec2, r: f32, value: u8) {
        self.carve_capsule(c, c, r, value);
    }

    fn block_circle(&mut self, c: Vec2, r: f32) {
        let r2 = r * r;
        for cy in 0..GRID {
            for cx in 0..GRID {
                if Self::cell_center(cx as i32, cy as i32).dist_sq(c) <= r2 {
                    self.terrain[cy * GRID + cx] = T_BLOCKED;
                }
            }
        }
    }

    #[inline]
    pub fn cell_of(p: Vec2) -> (i32, i32) {
        ((p.x / CELL).floor() as i32, (p.y / CELL).floor() as i32)
    }

    #[inline]
    pub fn cell_center(cx: i32, cy: i32) -> Vec2 {
        Vec2::new((cx as f32 + 0.5) * CELL, (cy as f32 + 0.5) * CELL)
    }

    #[inline]
    pub fn cell_walkable(&self, cx: i32, cy: i32) -> bool {
        cx >= 0 && cy >= 0 && (cx as usize) < GRID && (cy as usize) < GRID && self.terrain[cy as usize * GRID + cx as usize] != T_BLOCKED
    }

    #[inline]
    pub fn walkable(&self, p: Vec2) -> bool {
        let (cx, cy) = Self::cell_of(p);
        self.cell_walkable(cx, cy)
    }

    /// True if the straight segment a-b stays on walkable ground.
    pub fn los(&self, a: Vec2, b: Vec2) -> bool {
        let d = a.dist(b);
        let steps = (d / (CELL * 0.5)).ceil().max(1.0) as i32;
        for i in 0..=steps {
            let t = i as f32 / steps as f32;
            if !self.walkable(a + (b - a) * t) {
                return false;
            }
        }
        true
    }

    /// Closest walkable cell to `p` (searching outwards in rings), or None if the map is empty.
    pub fn nearest_walkable_cell(&self, p: Vec2) -> Option<(i32, i32)> {
        let (cx, cy) = Self::cell_of(p);
        let (cx, cy) = (cx.clamp(0, GRID as i32 - 1), cy.clamp(0, GRID as i32 - 1));
        if self.cell_walkable(cx, cy) {
            return Some((cx, cy));
        }
        for r in 1..=20i32 {
            let mut best: Option<(f32, (i32, i32))> = None;
            for dy in -r..=r {
                for dx in -r..=r {
                    if dx.abs().max(dy.abs()) != r || !self.cell_walkable(cx + dx, cy + dy) {
                        continue;
                    }
                    let d = Self::cell_center(cx + dx, cy + dy).dist_sq(p);
                    if best.is_none_or(|(bd, _)| d < bd) {
                        best = Some((d, (cx + dx, cy + dy)));
                    }
                }
            }
            if let Some((_, c)) = best {
                return Some(c);
            }
        }
        None
    }

    /// Closest walkable world position to `p`.
    pub fn nearest_walkable(&self, p: Vec2) -> Vec2 {
        if self.walkable(p) {
            return p;
        }
        self.nearest_walkable_cell(p).map(|(x, y)| Self::cell_center(x, y)).unwrap_or(p)
    }

    pub fn lane_length(&self, lane: Lane) -> f32 {
        self.lanes[lane.idx()].windows(2).map(|w| w[0].dist(w[1])).sum()
    }

    /// Point at arc-length fraction `frac` (0 = Radiant base, 1 = Dire base) along a lane.
    pub fn lane_point(&self, lane: Lane, frac: f32) -> Vec2 {
        let poly = &self.lanes[lane.idx()];
        let mut left = self.lane_length(lane) * frac.clamp(0.0, 1.0);
        for w in poly.windows(2) {
            let seg = w[0].dist(w[1]);
            if left <= seg {
                return w[0].move_toward(w[1], left);
            }
            left -= seg;
        }
        *poly.last().unwrap()
    }
}
