//! Record a bot-vs-bot match through the *same C ABI the browser uses* and write every
//! snapshot to a JSON file. The front-end test (`test/frontend.mjs`) replays this file
//! through a stand-in for `web/js/engine.js`, so `main.js` + `render.js` can be exercised
//! in a real browser with real engine data, without needing the `.wasm` build.
//!
//!   cargo run --release --example dump_replay -- <out.json> [frames] [per_team] [seed] [skip_ticks]

use moba_engine::ffi::*;
use std::fmt::Write as _;

fn f32s(ptr: usize, n: usize) -> &'static [f32] {
    // SAFETY: the pointers come from the bridge's own Vecs and are only read before the next
    // `moba_step` / `moba_snapshot` call; `n` is derived from the global table of the same snapshot.
    unsafe { std::slice::from_raw_parts(ptr as *const f32, n) }
}

/// Compact number: integers without a decimal point, everything else with 2 decimals.
fn put(s: &mut String, v: f32) {
    if !v.is_finite() {
        s.push('0');
    } else if v.fract() == 0.0 && v.abs() < 1e9 {
        let _ = write!(s, "{}", v as i64);
    } else {
        let t = format!("{:.2}", v);
        s.push_str(t.trim_end_matches('0').trim_end_matches('.'));
    }
}

fn put_arr(s: &mut String, v: &[f32]) {
    s.push('[');
    for (i, x) in v.iter().enumerate() {
        if i > 0 {
            s.push(',');
        }
        put(s, *x);
    }
    s.push(']');
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let out = args.get(1).cloned().unwrap_or_else(|| "replay.json".into());
    let frames: usize = args.get(2).and_then(|s| s.parse().ok()).unwrap_or(1800);
    let per: u32 = args.get(3).and_then(|s| s.parse().ok()).unwrap_or(3);
    let seed: u32 = args.get(4).and_then(|s| s.parse().ok()).unwrap_or(7);
    let skip: u32 = args.get(5).and_then(|s| s.parse().ok()).unwrap_or(0);

    // hero 1 (Ranger), fog on, bot drives the player's hero too
    moba_new(seed, 1, per, 1, 1);
    let mut left = skip;
    while left > 0 {
        let n = left.min(600);
        moba_step(n);
        left -= n;
    }
    moba_snapshot();

    let info = unsafe { std::slice::from_raw_parts(moba_info_ptr() as *const u8, moba_info_len()) };
    let info = std::str::from_utf8(info).expect("info is utf-8");
    let grid = GRID_CELLS();
    let terrain = unsafe { std::slice::from_raw_parts(moba_terrain_ptr() as *const u8, grid * grid) };

    let mut s = String::with_capacity(32 << 20);
    s.push_str("{\"info\":");
    s.push_str(info);
    s.push_str(",\"terrain\":[");
    for (i, t) in terrain.iter().enumerate() {
        if i > 0 {
            s.push(',');
        }
        let _ = write!(s, "{}", t);
    }
    s.push_str("],\"frames\":[");

    for fi in 0..frames {
        moba_snapshot();
        // global layout: see ffi::GLOBAL_FIELDS
        let g = f32s(moba_global_ptr(), GLOBAL_FIELDS.len());
        let col = |name: &str| GLOBAL_FIELDS.iter().position(|f| *f == name).unwrap();
        let (nu, nh, np, nz, ne) = (
            g[col("n_units")] as usize,
            g[col("n_heroes")] as usize,
            g[col("n_projs")] as usize,
            g[col("n_zones")] as usize,
            g[col("n_events")] as usize,
        );
        if fi > 0 {
            s.push(',');
        }
        s.push_str("{\"g\":");
        put_arr(&mut s, g);
        s.push_str(",\"u\":");
        put_arr(&mut s, f32s(moba_units_ptr(), nu * UNIT_FIELDS.len()));
        s.push_str(",\"h\":");
        put_arr(&mut s, f32s(moba_heroes_ptr(), nh * HERO_FIELDS.len()));
        s.push_str(",\"p\":");
        put_arr(&mut s, f32s(moba_projs_ptr(), np * PROJ_FIELDS.len()));
        s.push_str(",\"z\":");
        put_arr(&mut s, f32s(moba_zones_ptr(), nz * ZONE_FIELDS.len()));
        s.push_str(",\"e\":");
        put_arr(&mut s, f32s(moba_events_ptr(), ne * EVENT_FIELDS.len()));
        s.push('}');
        moba_step(1);
    }
    s.push_str("]}");
    std::fs::write(&out, &s).expect("write replay");
    println!("wrote {} frames ({:.1} MB) to {}", frames, s.len() as f64 / 1e6, out);
}

#[allow(non_snake_case)]
fn GRID_CELLS() -> usize {
    moba_engine::map::GRID
}
