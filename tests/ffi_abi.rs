//! Drives the engine exactly like the browser does: only through the exported C functions,
//! reading results back as raw f32 tables.

use moba_engine::ffi::*;
use std::sync::Mutex;

// The bridge is a process-wide singleton, so these tests must not interleave.
static LOCK: Mutex<()> = Mutex::new(());

fn col(fields: &[&str], name: &str) -> usize {
    fields.iter().position(|f| *f == name).unwrap_or_else(|| panic!("no column {name}"))
}

fn table(ptr: usize, rows: usize, stride: usize) -> Vec<f32> {
    if rows == 0 {
        return vec![];
    }
    unsafe { std::slice::from_raw_parts(ptr as *const f32, rows * stride) }.to_vec()
}

fn global() -> Vec<f32> {
    table(moba_global_ptr(), 1, GLOBAL_FIELDS.len())
}

fn gl(name: &str) -> f32 {
    global()[col(GLOBAL_FIELDS, name)]
}

fn hero_rows() -> Vec<Vec<f32>> {
    let n = gl("n_heroes") as usize;
    table(moba_heroes_ptr(), n, HERO_FIELDS.len()).chunks(HERO_FIELDS.len()).map(|c| c.to_vec()).collect()
}

fn unit_row(id: f32) -> Option<Vec<f32>> {
    let n = gl("n_units") as usize;
    let t = table(moba_units_ptr(), n, UNIT_FIELDS.len());
    t.chunks(UNIT_FIELDS.len()).find(|r| r[0] == id).map(|r| r.to_vec())
}

fn run(ticks: u32) {
    let mut left = ticks;
    while left > 0 {
        let n = left.min(30);
        moba_step(n);
        left -= n;
    }
    moba_snapshot();
}

#[test]
fn player_can_move_level_buy_and_cast_through_the_c_abi() {
    let _g = LOCK.lock().unwrap_or_else(|e| e.into_inner());
    assert_eq!(moba_abi_version(), ABI_VERSION);
    moba_new(42, 2 /* Archmage */, 1, 1, 0);
    moba_snapshot();

    let player = gl("player");
    let me0 = unit_row(player).expect("player hero is exported");
    let (x0, y0) = (me0[col(UNIT_FIELDS, "x")], me0[col(UNIT_FIELDS, "y")]);

    // static info is reachable and parses as JSON-ish text
    let info = unsafe { std::slice::from_raw_parts(moba_info_ptr() as *const u8, moba_info_len()) };
    let info = std::str::from_utf8(info).unwrap();
    assert!(
        info.starts_with(&format!("{{\"abi\":{}", ABI_VERSION)) && info.contains("\"Frost Nova\"") && info.contains("\"Blink Dagger\"")
    );

    // -- level up Frost Nova (slot 1) and buy Boots of Speed (item 7, 500g of the 625 start gold)
    moba_cmd_level_up(1);
    moba_cmd_buy(7);
    run(2);
    let h = &hero_rows()[gl("player_hero") as usize];
    assert_eq!(h[col(HERO_FIELDS, "ab1")], 1.0, "Frost Nova learned");
    assert_eq!(h[col(HERO_FIELDS, "skill_pts")], 0.0);
    assert_eq!(h[col(HERO_FIELDS, "it0")], 7.0, "boots in the first slot");
    assert!((h[col(HERO_FIELDS, "gold")] - 125.0).abs() < 8.0, "gold spent (plus passive income)");
    assert_eq!(gl("near_shop"), 1.0, "hero spawns inside the shop radius");

    // -- walk toward the middle of the map; the hero must actually move
    moba_cmd_move(1200.0, 1200.0);
    run(90);
    let me1 = unit_row(player).unwrap();
    let moved = ((me1[col(UNIT_FIELDS, "x")] - x0).powi(2) + (me1[col(UNIT_FIELDS, "y")] - y0).powi(2)).sqrt();
    assert!(moved > 300.0, "hero moved {moved:.0} units in 3 s with boots");

    // -- cast Frost Nova at a point: mana is spent and the cooldown starts
    let mana_before = me1[col(UNIT_FIELDS, "mana")];
    let p = (me1[col(UNIT_FIELDS, "x")], me1[col(UNIT_FIELDS, "y")]);
    moba_cmd_cast(1, 0, p.0 + 100.0, p.1);
    run(3);
    let h = &hero_rows()[gl("player_hero") as usize];
    assert!(h[col(HERO_FIELDS, "cd1")] > 5.0, "cooldown running");
    assert!(unit_row(player).unwrap()[col(UNIT_FIELDS, "mana")] < mana_before, "mana spent");

    // -- selling is refused away from the shop, so the boots stay
    moba_cmd_sell(0);
    run(2);
    assert_eq!(hero_rows()[gl("player_hero") as usize][col(HERO_FIELDS, "it0")], 7.0);
}

#[test]
fn identical_runs_have_identical_hashes_and_restart_resets() {
    let _g = LOCK.lock().unwrap_or_else(|e| e.into_inner());
    let play = || {
        moba_new(7, 0, 2, 1, 1);
        run(1800);
        (moba_hash_hi() as u64) << 32 | moba_hash_lo() as u64
    };
    let (a, b) = (play(), play());
    assert_eq!(a, b, "same seed + same inputs -> same state");
    moba_new(8, 0, 2, 1, 1);
    run(1800);
    assert_ne!((moba_hash_hi() as u64) << 32 | moba_hash_lo() as u64, a, "different seed -> different game");
    moba_new(7, 0, 2, 1, 1);
    assert_eq!(gl("time"), 0.0, "restart gives a fresh clock");
}

#[test]
fn bot_toggle_and_watch_mode_play_by_themselves() {
    let _g = LOCK.lock().unwrap_or_else(|e| e.into_inner());
    moba_new(3, 1, 1, 1, 1); // player hero driven by the bot
    run(30 * 180);
    let me = &hero_rows()[gl("player_hero") as usize];
    assert!(me[col(HERO_FIELDS, "level")] >= 2.0, "bot-driven hero levels up on its own");
    assert!(me[col(HERO_FIELDS, "last_hits")] >= 1.0, "and last-hits creeps");
    let u = unit_row(gl("player")).unwrap();
    assert!(u[col(UNIT_FIELDS, "x")] > 700.0, "and walks out of its base into the lane");
    // taking control back stops the AI from issuing orders
    moba_set_bot(0);
    moba_snapshot();
    assert_eq!(hero_rows()[gl("player_hero") as usize][col(HERO_FIELDS, "bot")], 0.0);
}
