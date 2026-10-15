//! Play a full bot-vs-bot match without any graphics and print a timeline.
//!
//!   cargo run --release --example headless -- [heroes_per_team] [seed] [max_minutes]

use moba_engine::data::*;
use moba_engine::{Config, Game};

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let per: u8 = args.get(1).and_then(|s| s.parse().ok()).unwrap_or(3);
    let seed: u32 = args.get(2).and_then(|s| s.parse().ok()).unwrap_or(7);
    let max_min: f32 = args.get(3).and_then(|s| s.parse().ok()).unwrap_or(45.0);

    let mut g = Game::new(Config { seed, player_hero: 0, heroes_per_team: per, fog: true, player_is_bot: true });
    let started = std::time::Instant::now();
    let mut next_report = 0.0;
    while g.winner < 0 && g.time < max_min * 60.0 {
        g.step();
        if g.time >= next_report {
            next_report += 120.0;
            report(&g);
        }
    }
    report(&g);
    match g.winner {
        0 => println!("\nWINNER: Radiant at {:.1} min", g.time / 60.0),
        1 => println!("\nWINNER: Dire at {:.1} min", g.time / 60.0),
        _ => println!("\nNo winner after {:.1} min", g.time / 60.0),
    }
    let ms = started.elapsed().as_secs_f64() * 1000.0;
    println!("simulated {} ticks in {:.0} ms ({:.2} ms/tick, state hash {:016x})", g.tick, ms, ms / g.tick as f64, g.state_hash());
}

fn report(g: &Game) {
    println!("--- t = {:>5.1} min   wave {:>2}   units alive {:>3}", g.time / 60.0, g.wave_no, g.units.iter().filter(|u| u.alive).count());
    for team in 0..2u8 {
        let towers = g.count_kind(team, Kind::Tower);
        println!("  {} towers {:>2}/9   ancient hp {:>5.0}", if team == 0 { "Radiant" } else { "Dire   " }, towers, g.units.iter().find(|u| u.kind == Kind::Ancient && u.team == team).map(|u| u.hp).unwrap_or(0.0));
    }
    for h in &g.heroes {
        let u = g.units.iter().find(|u| u.id == h.unit).unwrap();
        let items: Vec<&str> = h.items.iter().filter(|s| s.id != 0).map(|s| ITEMS[s.id as usize].name).collect();
        println!(
            "  {} {:<9} L{:<2} {:>4.0}g  K/D/A {}/{}/{}  LH {:<3} DN {:<2} hp {:>4.0}/{:<4.0} {}  [{}]",
            if h.team == 0 { "R" } else { "D" },
            HEROES[h.def as usize].name,
            h.level,
            h.gold,
            h.kills,
            h.deaths,
            h.assists,
            h.last_hits,
            h.denies,
            u.hp.max(0.0),
            u.max_hp,
            if u.alive { "alive" } else { "dead " },
            items.join(", ")
        );
    }
}
