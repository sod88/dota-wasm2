//! A* pathfinding on the walkability grid (8-way, no corner cutting) with
//! line-of-sight "string pulling" so units walk smooth, direct-looking routes.

use crate::map::{Map, CELL, GRID};
use crate::math::Vec2;
use std::cmp::Ordering;
use std::collections::BinaryHeap;

#[derive(Copy, Clone, Eq, PartialEq)]
struct Node {
    f: i32,
    g: i32,
    idx: u32,
}

// BinaryHeap is a max-heap, so invert the comparison to pop the lowest f first
// (ties: prefer the node that has travelled further, i.e. is closer to the goal).
impl Ord for Node {
    fn cmp(&self, o: &Self) -> Ordering {
        o.f.cmp(&self.f).then_with(|| self.g.cmp(&o.g))
    }
}
impl PartialOrd for Node {
    fn partial_cmp(&self, o: &Self) -> Option<Ordering> {
        Some(self.cmp(o))
    }
}

const DIRS: [(i32, i32, i32); 8] = [
    (1, 0, 10),
    (-1, 0, 10),
    (0, 1, 10),
    (0, -1, 10),
    (1, 1, 14),
    (1, -1, 14),
    (-1, 1, 14),
    (-1, -1, 14),
];

/// Reusable A* scratch space. Generation stamps avoid clearing the arrays per query.
pub struct PathFinder {
    g: Vec<i32>,
    parent: Vec<i32>,
    open_stamp: Vec<u32>,
    closed_stamp: Vec<u32>,
    stamp: u32,
    heap: BinaryHeap<Node>,
}

impl Default for PathFinder {
    fn default() -> Self {
        Self::new()
    }
}

impl PathFinder {
    pub fn new() -> Self {
        let n = GRID * GRID;
        PathFinder {
            g: vec![0; n],
            parent: vec![-1; n],
            open_stamp: vec![0; n],
            closed_stamp: vec![0; n],
            stamp: 0,
            heap: BinaryHeap::new(),
        }
    }

    fn heuristic(x: i32, y: i32, goal: (i32, i32)) -> i32 {
        let dx = (x - goal.0).abs();
        let dy = (y - goal.1).abs();
        10 * (dx + dy) - 6 * dx.min(dy) // octile distance
    }

    /// Returns waypoints from `from` (exclusive) to `to` (inclusive, or the nearest
    /// walkable point if `to` itself is blocked). `None` if no route exists.
    pub fn find(&mut self, map: &Map, from: Vec2, to: Vec2) -> Option<Vec<Vec2>> {
        let start = map.nearest_walkable_cell(from)?;
        let goal = map.nearest_walkable_cell(to)?;

        self.stamp = self.stamp.wrapping_add(1);
        if self.stamp == 0 {
            self.open_stamp.iter_mut().for_each(|s| *s = 0);
            self.closed_stamp.iter_mut().for_each(|s| *s = 0);
            self.stamp = 1;
        }
        let cell_idx = |c: (i32, i32)| c.1 as usize * GRID + c.0 as usize;
        let sidx = cell_idx(start);
        let gidx = cell_idx(goal);

        self.heap.clear();
        self.g[sidx] = 0;
        self.parent[sidx] = -1;
        self.open_stamp[sidx] = self.stamp;
        self.heap.push(Node {
            f: Self::heuristic(start.0, start.1, goal),
            g: 0,
            idx: sidx as u32,
        });

        let mut found = false;
        while let Some(Node { g, idx, .. }) = self.heap.pop() {
            let i = idx as usize;
            if self.closed_stamp[i] == self.stamp {
                continue;
            }
            self.closed_stamp[i] = self.stamp;
            if i == gidx {
                found = true;
                break;
            }
            let (cx, cy) = ((i % GRID) as i32, (i / GRID) as i32);
            for &(dx, dy, cost) in &DIRS {
                let (nx, ny) = (cx + dx, cy + dy);
                if !map.cell_walkable(nx, ny) {
                    continue;
                }
                // No squeezing diagonally between two blocked cells.
                if dx != 0 && dy != 0 && !(map.cell_walkable(cx + dx, cy) && map.cell_walkable(cx, cy + dy)) {
                    continue;
                }
                let ni = ny as usize * GRID + nx as usize;
                if self.closed_stamp[ni] == self.stamp {
                    continue;
                }
                let ng = g + cost;
                if self.open_stamp[ni] != self.stamp || ng < self.g[ni] {
                    self.open_stamp[ni] = self.stamp;
                    self.g[ni] = ng;
                    self.parent[ni] = i as i32;
                    self.heap.push(Node {
                        f: ng + Self::heuristic(nx, ny, goal),
                        g: ng,
                        idx: ni as u32,
                    });
                }
            }
        }
        if !found {
            return None;
        }

        // Walk the parent chain back from the goal.
        let mut pts: Vec<Vec2> = Vec::new();
        let mut cur = gidx as i32;
        while cur >= 0 && cur as usize != sidx {
            let c = cur as usize;
            pts.push(Map::cell_center((c % GRID) as i32, (c / GRID) as i32));
            cur = self.parent[c];
        }
        pts.reverse();
        match pts.last_mut() {
            Some(last) => {
                if map.walkable(to) {
                    *last = to;
                }
            }
            None => pts.push(if map.walkable(to) { to } else { Map::cell_center(goal.0, goal.1) }),
        }
        Some(smooth(map, from, pts))
    }
}

/// Greedy string pulling: keep only the waypoints needed to avoid obstacles.
fn smooth(map: &Map, from: Vec2, pts: Vec<Vec2>) -> Vec<Vec2> {
    let mut out = Vec::new();
    let mut anchor = from;
    let mut i = 0;
    while i < pts.len() {
        let mut best = i;
        for k in (i..pts.len()).rev() {
            if map.los(anchor, pts[k]) {
                best = k;
                break;
            }
        }
        out.push(pts[best]);
        anchor = pts[best];
        i = best + 1;
    }
    // Drop a leading waypoint we are already standing on.
    if out.len() > 1 && out[0].dist(from) < CELL * 0.25 {
        out.remove(0);
    }
    out
}
