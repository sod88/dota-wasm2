use moba_engine::map::*;
use moba_engine::path::PathFinder;

#[test]
fn map_is_point_symmetric() {
    let m = Map::generate();
    let mut diff = 0;
    for cy in 0..GRID {
        for cx in 0..GRID {
            let a = m.terrain[cy * GRID + cx] != T_BLOCKED;
            let b = m.terrain[(GRID - 1 - cy) * GRID + (GRID - 1 - cx)] != T_BLOCKED;
            if a != b {
                diff += 1;
            }
        }
    }
    assert_eq!(diff, 0, "walkability must be symmetric so both teams are fair");
}

#[test]
fn bases_lanes_and_camps_are_connected() {
    let m = Map::generate();
    let mut pf = PathFinder::new();
    for lane in Lane::ALL {
        let p = pf.find(&m, BASE_RADIANT, BASE_DIRE).expect("route between bases");
        assert!(!p.is_empty());
        // walking the lane polyline itself must always be possible
        for f in 0..=20 {
            assert!(m.walkable(m.lane_point(lane, f as f32 / 20.0)), "lane {:?} blocked at {}", lane, f);
        }
    }
    for c in &m.camps {
        assert!(m.walkable(c.pos), "camp spawn must be walkable");
        assert!(pf.find(&m, BASE_RADIANT, c.pos).is_some(), "camp reachable");
    }
}

#[test]
fn paths_avoid_obstacles() {
    let m = Map::generate();
    let mut pf = PathFinder::new();
    // jungle-to-jungle across the map should never cut through blocked cells
    let a = m.camps[0].pos;
    let b = m.camps[m.camps.len() - 1].pos;
    let path = pf.find(&m, a, b).unwrap();
    let mut prev = a;
    for p in path {
        assert!(m.los(prev, p), "every path leg must be walkable");
        prev = p;
    }
    assert!(prev.dist(b) < CELL);
}
