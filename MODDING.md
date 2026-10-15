# Modding Lane Wars

A mod is one JSON file. It can add heroes and items, rebalance anything, replace the whole roster, and
change the rules of the game: gold, wave sizes, respawn times, the XP curve. Nothing needs compiling,
and the same file works in both the Rust and the C++ engine with identical results.

## Using mods

- **In the game:** the **Mods** panel in the menu lists the mods in `web/mods/`. Tick the ones you
  want. Use **Add mod file…** or drop `.json` files onto the menu to load your own; they are
  remembered in this browser. Mods load top to bottom, so a later mod can change what an earlier
  one added.
- **Shipping a mod with the game:** put the file in `web/mods/` and add its name to
  `web/mods/index.json`.
- **Headless:** `cpp/build/headless 2 7 30 my-mod.json` plays a bot match with the mod (C++ engine).
- **Starting point:** **Export content** in the menu downloads every hero, item and rule currently
  in play as a mod file. Copy the parts you want to change into a new file.

If a mod has a problem, it is not loaded at all and the menu lists up to 20 errors, each with the
exact location, for example `$.heroes[0].abilities[2].cd: must be between 0 and 10000`.

## The smallest mods

```json
{ "name": "Rich start", "rules": { "start_gold": 3000 } }
```

```json
{
  "name": "Faster Warden",
  "heroes": [{ "name": "Warden", "ms": 200, "abilities": [null, { "dmg": [100, 150, 200, 250] }] }]
}
```

Lists (`heroes`, `items`, `neutrals`) are matched **by name**. A name that already exists patches that
entry: only the keys you write change. A new name adds a new entry. In a hero's `abilities`, `null`
means "leave this slot alone".

See `web/mods/` for three complete examples: a new hero with a new item (`nightblade.json`), a rules
rebalance (`gold-rush.json`), and a jungle overhaul (`wild-jungle.json`).

## File layout

```text
{
  "format": 1,                         optional; only format 1 exists
  "name": "...", "version": "...", "author": "...", "description": "...",
  "replace": ["heroes", "items", "neutrals"],   start these lists empty instead of patching them
  "rules":    { ... },                 game rules
  "xp_table": [ ... ],                 XP to reach each level (sets the level cap)
  "items":    [ { ... } ],
  "creeps":   { "melee": {...}, "ranged": {...}, "siege": {...} },
  "neutrals": [ { ... } ],
  "camps":    [ [names], [names], [names] ],     small, medium, large jungle camps
  "towers":   [ {tier 1} | null, {tier 2} | null, {tier 3} | null ],
  "ancient":  { ... },
  "heroes":   [ { ..., "abilities": [ {Q}, {W}, {E}, {R} ] } ]
}
```

Every key is optional. Unknown keys are errors, so typos are caught. Any object may also contain a
`"comment"` key, which is ignored. Numbers outside their range are rejected.

`replace` rules:

- Replacing `items` also requires replacing `heroes`, because bot item builds refer to items by name.
- Replacing `neutrals` also requires a `camps` list.
- The game always needs at least 1 hero and 1 neutral.

Limits: 24 heroes, 99 items, 16 neutral types, 2–60 XP-table entries.

## Rules

| key | default | range | meaning |
|---|---|---|---|
| `start_gold` | 625 | 0 – 1000000 | gold each hero starts with |
| `passive_gold` | 1.6 | 0 – 10000 | gold per second for every living hero |
| `wave_interval` | 30 | 1 – 600 | seconds between creep waves |
| `first_wave_at` | 8 | 0 – 600 | seconds until the first wave |
| `wave_melee` / `wave_ranged` | 3 / 1 | 0 – 20 | creeps per lane per wave (whole numbers) |
| `wave_siege` / `siege_every` | 1 / 5 | 0 – 10 / 0 – 1000 | siege creeps every Nth wave (0 = never) |
| `camp_respawn` | 60 | 1 – 3600 | seconds before a cleared jungle camp returns |
| `xp_radius` | 700 | 0 – 5000 | heroes this close share the XP of a kill |
| `shop_radius` | 260 | 0 – 5000 | distance from the fountain where you can shop |
| `magic_resist` | 0.25 | 0 – 0.95 | fraction of magical damage blocked |
| `blink_range` | 340 | 0 – 5000 | Blink Dagger distance (any item with `"active": "blink"`) |
| `salve_heal` / `salve_time` | 300 / 10 | 0 – 100000 / 0.1 – 300 | healing salve total and duration |
| `mana_potion_restore` / `mana_potion_time` | 150 / 8 | 0 – 100000 / 0.1 – 300 | mana potion total and duration |
| `hero_vision` | 420 | 0 – 5000 | how far heroes see |
| `hp_base` / `hp_per_str` | 200 / 20 | 1 – 100000 / 0 – 10000 | hero max HP = base + per_str × strength |
| `mana_base` / `mana_per_int` | 50 / 12 | 0 – 100000 / 0 – 10000 | hero max mana |
| `armor_base` / `armor_per_agi` | 1 / 0.15 | -50 – 100 / -5 – 5 | hero armor |
| `hp_regen_base` / `hp_regen_per_str` | 1 / 0.12 | 0 – 1000 / 0 – 100 | HP regeneration per second |
| `mana_regen_base` / `mana_regen_per_int` | 0.5 / 0.05 | 0 – 1000 / 0 – 100 | mana regeneration per second |
| `respawn_base` / `respawn_per_level` / `respawn_max` | 5 / 2.2 / 60 | 0 – 600 / 0 – 100 / 0 – 600 | hero respawn seconds |
| `hero_bounty_base` / `hero_bounty_per_level` | 120 / 14 | 0 – 100000 / 0 – 10000 | gold for a hero kill (assists get 30%) |
| `hero_xp_base` / `hero_xp_per_level` | 60 / 22 | 0 – 100000 / 0 – 10000 | XP for a hero kill |

On top of these, every point of agility adds 1% attack speed, and every point of the hero's primary
attribute adds 1 attack damage.

`xp_table` lists the total XP needed to reach level 1, 2, 3 and so on. It must start at 0 and never
decrease, and its length is the level cap (the default has 25 levels).

## Heroes

| key | range | notes |
|---|---|---|
| `name` | 1–40 chars | **required**; the hero's identity |
| `title` | ≤ 80 chars | subtitle on the hero card |
| `primary` | `"str"`, `"agi"`, `"int"` | primary attribute: +1 damage per point |
| `str`, `agi`, `int` | 0 – 1000 | attributes at level 1 |
| `gain_str`, `gain_agi`, `gain_int` | 0 – 100 | attribute gain per level |
| `dmg_min`, `dmg_max` | 0 – 100000 | base attack damage (`dmg_min` ≤ `dmg_max`) |
| `range` | 0 – 5000 | attack range (above ~200 counts as ranged) |
| `bat` | 0.1 – 20 | base attack time in seconds |
| `ms` | 0 – 2000 | movement speed |
| `proj_speed` | 0 – 10000 | attack projectile speed; 0 = melee (instant hit) |
| `radius` | 1 – 500 | collision size |
| `build` | ≤ 24 item names | items the bot buys, in order |
| `skill_order` | 3 of `0`, `1`, `2` | bot's priority for Q/W/E (it always takes R when it can) |
| `glyph` | name | emblem: `shield`, `bow`, `star`, `sword`, `bolt`, `moon`, `flame`, `crown`, `leaf`, `skull` (default: from primary attribute) |
| `color` | `"#rrggbb"` | emblem tint |
| `abilities` | 4 objects | Q, W, E, R. **Required** (exactly 4) for a new hero |

Defaults for a new hero: primary `str`, attributes 20 (+2/level), damage 30–36, range 100, bat 1.7,
ms 300, radius 16, no build, skill order 0, 1, 2.

## Abilities

Every ability is built from one **cast type** plus optional **effects**:

| `cast` | what it does | uses |
|---|---|---|
| `"target"` | hits one enemy unit (a homing projectile if `speed` > 0) | `range`, `dmg`, `speed` |
| `"line"` | a projectile flying to max range that hits everything it passes | `range`, `radius` (width), `speed`, `dmg` |
| `"point"` | an area at a point; with `delay` it lands after a telegraph | `range`, `radius`, `delay`, `dmg` |
| `"self_aoe"` | an area around the caster | `radius`, `dmg` |
| `"self_buff"` | a buff on the caster: `aspd` (attack speed `pct` %) and/or `haste` (move speed `pct2` %) | `dur`, `pct`, `pct2` |
| `"blink"` | teleports towards the cursor; with `aoe_on_land` it also hits an area on arrival | `range`, `radius`, `dmg` |
| `"none"` | does nothing (a placeholder slot) | |

`fx` is a list of effects applied to enemies hit for `dur` seconds: `"stun"`, `"root"`, `"slow"` (by
`pct` %), plus `"aspd"` and `"haste"` for `self_buff`.

| key | range | notes |
|---|---|---|
| `name` | 1–40 chars | **required** for a new ability |
| `cast` | see above | **required** for a new ability |
| `desc` | ≤ 300 chars | tooltip text |
| `dtype` | `"physical"`, `"magical"`, `"pure"` | physical uses armor; magical uses `magic_resist`; pure ignores both (default magical) |
| `fx` | list | see above |
| `max_level` | 1 – 4 | |
| `req_level` | 1 – 99 | hero level needed for each ability level (default 1, 3, 5, 7) |
| `range` | 0 – 5000 | per level |
| `radius` | 0 – 2000 | per level |
| `mana` | 0 – 100000 | per level |
| `cd` | 0 – 10000 | cooldown seconds, per level |
| `dmg` | 0 – 1000000 | per level |
| `dur` | 0 – 300 | effect duration, per level |
| `pct`, `pct2` | 0 – 1000 | percentages, per level |
| `speed` | 0 – 10000 | projectile speed |
| `delay` | 0 – 30 | seconds before a `point` ability lands |
| `aoe_on_land` | true / false | `blink` only |
| `icon` | name | `hammer`, `net`, `flame`, `arrow`, `rain`, `snow`, `meteor`, `waves`, `chevrons`, `blink`, `fist` (default: picked from cast type and effects) |
| `color` | `"#rrggbb"` | icon and spell-effect colour |

"Per level" values take one number for every level (`"cd": 9`) or a list of 1–4 numbers
(`"dmg": [50, 90, 130, 170]`). In a short list the last value repeats.

## Items

| key | range | notes |
|---|---|---|
| `name` | 1–40 chars | **required** |
| `cost` | 0 – 1000000 | **required** for a new item |
| `str`, `agi`, `int` | -1000 – 1000 | |
| `dmg` | -10000 – 10000 | |
| `armor` | -100 – 100 | |
| `hp`, `mana` | -100000 – 100000 | |
| `aspd` | -1000 – 1000 | attack speed % |
| `ms` | -1000 – 1000 | movement speed |
| `hpreg`, `mpreg` | -1000 – 1000 | regeneration per second |
| `active` | `"none"`, `"salve"`, `"mana"`, `"blink"` | what using the item does (see rules for the numbers) |
| `cd` | 0 – 10000 | cooldown of the active |
| `stack` | true / false | consumable: stacks up to 5 per slot, used up on use |
| `color` | `"#rrggbb"` | icon colour |

Bots open with the first item whose `active` is `"salve"`, if there is one.

## Creeps, neutrals, camps, towers, Ancient

`creeps.melee` / `ranged` / `siege` and every entry of `neutrals` accept:

- `name`
- `hp` (1 – 10000000)
- `dmg_min`, `dmg_max` (0 – 100000)
- `armor` (-100 – 100)
- `range` (0 – 5000)
- `bat` (0.1 – 20)
- `ms` (0 – 2000)
- `proj_speed` (0 – 10000, 0 = melee)
- `radius` (1 – 500)
- `vision` (0 – 5000)
- `gold`, `xp` (0 – 1000000)
- `bldg_mult` (0 – 100: damage multiplier against buildings)

A new neutral needs `hp`, `dmg_min` and `dmg_max`. Lane creeps and neutrals grow a little stronger
every minute.

`camps` lists three tiers (small, medium, large), each 1–8 neutral names. The map has two camps of
each tier per side.

`towers` patches tier 1–3 towers in every lane. Keys:

- `hp`
- `dmg_min`, `dmg_max`
- `armor`
- `range`
- `bat`
- `gold` (paid to every hero on the destroying team)
- `lane_frac` (0 – 1, position along the lane from the owner's base)

`ancient` accepts `hp`, `armor`, `radius` and `vision`.

## Good to know

- Mods are deterministic: the same mods and the same seed always produce the same match, in both
  engines. Lockstep multiplayer and replays depend on this.
- A mod can't add new kinds of behaviour, such as a new cast type or new item actives; those live
  in the engine code (`src/abilities.rs` and `cpp/src/abilities.cpp`). Everything else is data.
- Errors are identical in both engines: the shared test file `tests/mod_cases.json` checks 45 cases
  against both.
