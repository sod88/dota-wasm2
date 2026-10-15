//! Content / mod tests: the built-in game is unchanged, mods load strictly and really change play.

use moba_engine::content::{apply_mod, builtin, Content, MAX_HEROES, MAX_ITEMS};
use moba_engine::data::*;
use moba_engine::json::{self, Value};
use moba_engine::{Config, Game};

fn leak(c: Content) -> &'static Content {
    Box::leak(Box::new(c))
}

fn load(base: &Content, text: &str) -> Result<Content, Vec<String>> {
    apply_mod(base, text.as_bytes())
}

fn play(content: &'static Content, cfg: Config, minutes: f32) -> Game {
    let mut g = Game::with_content(cfg, content);
    while g.winner < 0 && g.time < minutes * 60.0 {
        g.step();
    }
    g
}

fn bots(per_team: u8, seed: u32, hero: u8) -> Config {
    Config { seed, player_hero: hero, heroes_per_team: per_team, fog: true, player_is_bot: true }
}

/// Hashes recorded from the engine *before* it became data-driven: the refactor changed nothing.
#[test]
fn builtin_content_still_plays_exactly_like_the_original_engine() {
    for (per, seed, mins, hash) in [(1, 3, 8.0, 0x8c09611f2d45f96f_u64), (2, 5, 10.0, 0x334ad9fd7ef384d4), (3, 7, 12.0, 0x413a7ddc794dadc7), (5, 11, 6.0, 0x6a6146b71105abb8)] {
        let g = play(builtin(), bots(per, seed, 0), mins);
        assert_eq!(g.state_hash(), hash, "{}v{} seed {}", per, per, seed);
    }
}

#[test]
fn shared_mod_cases_give_the_expected_errors() {
    let text = std::fs::read_to_string(concat!(env!("CARGO_MANIFEST_DIR"), "/tests/mod_cases.json")).unwrap();
    let cases = match json::parse(&text).unwrap() {
        Value::Arr(a) => a,
        _ => panic!("cases must be an array"),
    };
    assert!(cases.len() >= 40);
    for case in cases {
        let Value::Obj(o) = case else { panic!() };
        let get = |k: &str| o.iter().find(|(n, _)| n == k).map(|(_, v)| v.clone());
        let Some(Value::Str(name)) = get("name") else { panic!() };
        let input = match (get("mod_text"), get("mod")) {
            (Some(Value::Str(t)), _) => t,
            (_, Some(v)) => json::to_pretty(&v),
            _ => panic!("{}: no input", name),
        };
        let expected: Vec<String> = match get("errors") {
            Some(Value::Arr(a)) => a.into_iter().map(|v| if let Value::Str(s) = v { s } else { panic!() }).collect(),
            _ => panic!(),
        };
        let got = match load(builtin(), &input) {
            Ok(_) => vec![],
            Err(e) => e,
        };
        assert_eq!(got, expected, "case: {}", name);
    }
}

#[test]
fn invalid_utf8_and_oversized_files_are_rejected() {
    assert_eq!(apply_mod(builtin(), &[b'{', 0xff, b'}']).unwrap_err(), vec!["mod file is not valid UTF-8 text".to_string()]);
    let big = vec![b' '; (1 << 20) + 1];
    assert_eq!(apply_mod(builtin(), &big).unwrap_err(), vec!["mod file is larger than 1 MiB".to_string()]);
}

#[test]
fn exported_builtin_is_a_valid_no_op_mod() {
    let exported = builtin().to_mod_json();
    let back = load(builtin(), &exported).expect("the exported content must load");
    let mut expect = builtin().clone();
    expect.mods = back.mods.clone();
    assert_eq!(back, expect);
    // and exporting what was loaded gives the same text again
    assert_eq!(back.to_mod_json().replace("\"name\": \"Built-in content\"", ""), exported.replace("\"name\": \"Built-in content\"", ""));
    let g = play(leak(back), bots(2, 5, 0), 10.0);
    assert_eq!(g.state_hash(), 0x334ad9fd7ef384d4);
}

#[test]
fn limits_on_hero_and_item_counts() {
    let ab = r#"{"name":"A","cast":"target"}"#;
    let heroes: Vec<String> = (0..MAX_HEROES + 1).map(|i| format!(r#"{{"name":"H{}","abilities":[{},{},{},{}]}}"#, i, ab, ab, ab, ab)).collect();
    let err = load(builtin(), &format!(r#"{{"heroes":[{}]}}"#, heroes.join(","))).unwrap_err();
    assert_eq!(err, vec![format!("$.heroes: too many heroes (at most {})", MAX_HEROES)]);
    let items: Vec<String> = (0..MAX_ITEMS).map(|i| format!(r#"{{"name":"I{}","cost":1}}"#, i)).collect();
    let err = load(builtin(), &format!(r#"{{"items":[{}]}}"#, items.join(","))).unwrap_err();
    assert_eq!(err, vec![format!("$.items: too many items (at most {})", MAX_ITEMS - 1)]);
}

const NIGHTBLADE: &str = r##"{
  "name": "Nightblade pack", "version": "1.0", "author": "test",
  "items": [{"name": "Shadow Cloak", "cost": 600, "agi": 8, "armor": 2, "ms": 10}],
  "heroes": [{
    "name": "Nightblade", "title": "Agility - assassin", "primary": "agi",
    "str": 18, "agi": 28, "int": 14, "gain_str": 2, "gain_agi": 3.2, "gain_int": 1.2,
    "dmg_min": 30, "dmg_max": 36, "range": 110, "bat": 1.6, "ms": 185, "radius": 15,
    "glyph": "bolt", "color": "#6a5acd",
    "build": ["Boots of Speed", "Shadow Cloak", "Blink Dagger"], "skill_order": [1, 0, 2],
    "abilities": [
      {"name": "Shadow Dart", "desc": "A fast dart.", "cast": "line", "range": 600, "radius": 25, "mana": 50, "cd": [8, 7, 6, 5], "dmg": [70, 110, 150, 190], "speed": 900},
      {"name": "Venom Strike", "desc": "Slows.", "cast": "target", "fx": ["slow"], "range": 300, "mana": 60, "cd": 9, "dmg": [50, 90, 130, 170], "dur": 3, "pct": [30, 40, 50, 60], "speed": 800},
      {"name": "Fleet Step", "desc": "Haste.", "cast": "self_buff", "fx": ["haste", "aspd"], "mana": 40, "cd": 18, "dur": 5, "pct": [30, 40, 50, 60], "pct2": [15, 20, 25, 30]},
      {"name": "Death Mark", "desc": "Leap and strike.", "cast": "blink", "dtype": "pure", "fx": ["stun"], "max_level": 3, "req_level": [6, 12, 18], "range": 450, "radius": 200, "mana": [150, 200, 250], "cd": [60, 50, 40], "dmg": [200, 320, 440], "dur": [1, 1.5, 2], "aoe_on_land": true}
    ]
  }]
}"##;

#[test]
fn a_new_hero_is_playable_and_deterministic() {
    let c = leak(load(builtin(), NIGHTBLADE).expect("valid mod"));
    assert_eq!(c.heroes.len(), 4);
    assert_eq!(c.abilities.len(), 16);
    assert_eq!(c.heroes[3].name, "Nightblade");
    assert_eq!(c.heroes[3].build.len(), 3);
    assert_eq!(c.abilities[3 * 4 + 3].name, "Death Mark");
    assert_eq!(c.mods[0].name, "Nightblade pack");

    let cfg = bots(2, 9, 3); // our hero is the player (as a bot) and the first on its team
    let a = play(c, cfg, 12.0);
    let b = play(c, cfg, 12.0);
    assert_eq!(a.state_hash(), b.state_hash(), "same mod + seed => same match");
    let nb = a.heroes.iter().find(|h| h.def == 3).expect("Nightblade was spawned");
    assert!(nb.items.iter().any(|s| s.id != 0), "its bot bought items from the mod's build");
    assert!(nb.level > 1, "and levelled up");
    assert!(nb.ab_level.iter().sum::<u8>() > 1, "and learned abilities");
    assert!(nb.last_hits > 0, "and farmed");
    // a different mod state is a different game
    let plain = play(builtin(), bots(2, 9, 0), 12.0);
    assert_ne!(plain.state_hash(), a.state_hash());
}

#[test]
fn patching_changes_only_the_named_fields() {
    let c = load(builtin(), r#"{"heroes":[{"name":"Warden","ms":210,"abilities":[null,{"dmg":[100,150,200,250]}]}]}"#).unwrap();
    let (old, new) = (&builtin().heroes[0], &c.heroes[0]);
    assert_eq!(new.ms, 210.0);
    assert_eq!(HeroDef { ms: old.ms, ..*new }, *old, "everything else is untouched");
    assert_eq!(c.abilities[0], builtin().abilities[0]);
    assert_eq!(c.abilities[1].dmg, [100.0, 150.0, 200.0, 250.0]);
    assert_eq!(c.abilities[1].mana, builtin().abilities[1].mana);
    assert_eq!(c.heroes.len(), 3);
}

#[test]
fn mods_stack_in_order() {
    let first = load(builtin(), r#"{"name":"one","rules":{"start_gold":700,"wave_melee":2}}"#).unwrap();
    let second = load(&first, r#"{"name":"two","rules":{"start_gold":900}}"#).unwrap();
    assert_eq!(second.rules.start_gold, 900.0);
    assert_eq!(second.rules.wave_melee, 2, "the first mod's other changes survive");
    assert_eq!(second.mods.iter().map(|m| m.name.as_str()).collect::<Vec<_>>(), ["one", "two"]);
}

#[test]
fn rules_really_drive_the_simulation() {
    let c = leak(load(builtin(), r#"{"rules":{"start_gold":1234,"wave_melee":0,"wave_ranged":0,"siege_every":0,"first_wave_at":1}}"#).unwrap());
    let mut g = Game::with_content(Config::default(), c);
    assert_eq!(g.heroes[0].gold, 1234.0);
    for _ in 0..(40 * 30) {
        g.step();
    }
    assert!(g.units.iter().all(|u| !u.kind.is_lane_creep()), "waves are empty");
    // the default game does spawn creeps in the same time
    let mut d = Game::new(Config::default());
    for _ in 0..(40 * 30) {
        d.step();
    }
    assert!(d.units.iter().any(|u| u.kind.is_lane_creep()));
}

#[test]
fn xp_table_sets_the_level_cap() {
    let c = leak(load(builtin(), r#"{"xp_table":[0,50,120]}"#).unwrap());
    let mut g = Game::with_content(Config::default(), c);
    let ui = g.player_idx().unwrap();
    g.add_xp(ui, 100000.0);
    assert_eq!(g.heroes[0].level, 3);
}

#[test]
fn replacing_the_roster_removes_the_builtin_heroes() {
    let c = load(builtin(), r#"{"replace":["heroes"],"heroes":[{"name":"Solo","abilities":[{"name":"a","cast":"target"},{"name":"b","cast":"target"},{"name":"c","cast":"target"},{"name":"d","cast":"target"}]}]}"#).unwrap();
    assert_eq!(c.heroes.len(), 1);
    assert_eq!(c.abilities.len(), 4);
    let g = play(leak(c), bots(3, 2, 0), 5.0);
    assert!(g.heroes.iter().all(|h| h.def == 0), "everybody plays the only hero");
}

#[test]
fn empty_build_and_missing_salve_do_not_break_bots() {
    // no salve item and a hero without a build: the bot AI must cope
    let c = leak(load(builtin(), r#"{"replace":["items","heroes"],"items":[{"name":"Stick","cost":10,"dmg":1}],"heroes":[{"name":"Plain","abilities":[{"name":"a","cast":"none"},{"name":"b","cast":"none"},{"name":"c","cast":"none"},{"name":"d","cast":"none"}]}]}"#).unwrap());
    let g = play(c, bots(2, 4, 0), 6.0);
    assert!(g.time >= 6.0 * 60.0 - 1.0);
}
