# Lane Wars — a moddable Dota-style MOBA engine in C++ and Rust

A deterministic 30 Hz MOBA simulation with **two interchangeable engines**: one in C++ (`cpp/`) and
one in Rust (`src/`). Both compile to WebAssembly and share one canvas front end in plain JavaScript.

The two engines are the same game:

- they export the same C functions;
- they read the same JSON mod files and give the same error messages;
- given the same seed, commands and mods, they produce bit-identical matches.

`tools/parity.py` checks this tick by tick across many full matches.

## Run it (macOS)

```bash
# one time: Rust, and Emscripten for the C++ engine
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
brew install emscripten cmake

./build.sh        # Rust engine -> web/moba_engine.wasm
./build-cpp.sh    # C++ engine  -> web/moba_engine_cpp.wasm
./serve.sh        # http://localhost:8080
```

Pick the engine in the menu (**Engine: Rust | C++**), or open `index.html?engine=cpp`. You only need
one of the two builds; the menu offers whichever exists. Don't open `index.html` straight from disk:
browsers refuse to load `.wasm` from `file://` URLs.

`build-cpp.sh` also works with wasi-sdk (`WASI_SDK_PATH=...`) or zig instead of Emscripten.

## Mods

Mods are JSON files that add heroes and items, rebalance anything, or change the rules. They load
from the **Mods** panel in the menu: tick the shipped ones, add your own, or drag files onto the menu.
**Export content** downloads the whole current game as a mod file to start from. Three examples ship
in `web/mods/`.

The full format is in **[MODDING.md](MODDING.md)**.

## Tests

```bash
cargo test --release                                            # Rust: 26 tests
cmake -S cpp -B cpp/build && cmake --build cpp/build -j && cpp/build/engine_tests   # C++: 68 checks
python3 tools/parity.py           # Rust vs C++, tick by tick (add --full to play every match out)
```

`web/parity.html` (open it through `./serve.sh`) runs the same comparison between the two `.wasm`
builds in the browser.

Headless bot matches, with the same arguments in both languages:

```bash
cargo run --release --example headless -- 3 7 45                # heroes per team, seed, max minutes
cpp/build/headless 3 7 45 web/mods/nightblade.json              # ... plus optional mod files
```

## Controls

| Input | Action |
|---|---|
| Right-click | Move / attack (also denies hurt allied creeps) |
| `A` + click | Attack-move |
| `Q` `W` `E` `R` | Cast (`Shift`+key or the **+** badge learns it) |
| `1`–`6` | Use inventory item (right-click a slot at the fountain to sell) |
| `S` / `H` | Stop / hold |
| `B` | Shop (buying needs your hero at the fountain) |
| `Tab` | Scoreboard (hold) |
| `Space` / `Y` | Centre on hero / lock camera |
| Arrow keys, screen edge, minimap | Pan camera (unlocks it) |
| `T` | Autopilot: the bot plays your hero |
| `P` / `Esc` | Pause / cancel targeting |
| Mouse wheel | Zoom |

Menu options: engine, 1v1 to 5v5, fog of war, quick-cast, "watch bots play", mods. URL parameters
for quick launches: `?autostart=1&engine=cpp&hero=2&size=2&fog=0&watch=1&speed=4&mods=nightblade.json`.

## Layout

```
cpp/src/          C++ engine            src/              Rust engine (same modules)
  math.hpp          vectors, PRNG         math.rs
  data.hpp/.cpp     types, built-in data  data.rs
  content.*         moddable content      content.rs        + mod loader and validation
  json.*            JSON read/write       json.rs
  map.*, path.*     terrain, A*           map.rs, path.rs
  game.*            world, tick loop      game.rs
  combat.cpp        attacks, damage       combat.rs
  abilities.cpp     spells, items         abilities.rs
  ai.cpp            creeps, towers, bots  ai.rs
  ffi.cpp           the C ABI             ffi.rs
cpp/tests, cpp/examples                   tests/, examples/
web/              index.html, js/ (engine.js loader, render.js, icons.js, main.js), mods/, parity.html
tools/            enginelib.py (drives either engine from Python), parity.py, gen_cpp_builtin.py
tests/mod_cases.json   mod error cases both engines must reproduce exactly
```

### How JavaScript talks to the engines

The page calls `moba_new / moba_step / moba_snapshot`, the `moba_cmd_*` commands and the mod
functions (`moba_alloc`, `moba_mod_load`, `moba_mod_clear`, `moba_content_export`). After a snapshot,
units, heroes, projectiles, zones and events are flat `f32` tables in wasm memory. Their column names
are exported as JSON (`unit_fields`, `hero_fields`, …), so `web/js/engine.js` looks columns up by
name.

Fog of war is enforced inside the engine: the player's team is never sent units it can't see.

### Changing the engine

Content and balance belong in mods. Changes to behaviour (new cast types, new AI) go into the engine
code. To keep both engines in lockstep:

1. Make the change in both `cpp/src/` and `src/`.
2. Run `tools/parity.py`. It names the first tick and table where they differ.

Built-in content is defined once, in `src/data.rs`. After editing it, regenerate the C++ copy with
`python3 tools/gen_cpp_builtin.py > cpp/src/data.cpp` (it reads the Rust build's export).

Floating point is the delicate part. The C++ build uses `-ffp-contract=off` and single-precision
literals (`0.5f`), and reproduces Rust semantics where C++ differs: saturating float-to-int casts,
NaN-aware min/max, stable sorting, and the exact pop order of Rust's `BinaryHeap`.

## Known gaps

- **3v3 and 5v5 bot matches rarely finish.** Bots only push towers when no enemy hero is within 900
  units (see `think_bot` in `ai.rs` / `ai.cpp`). 1v1 and 2v2 finish in about 20–40 minutes. Playing
  yourself is unaffected.
- Bot-only matches are won by Dire more often than by Radiant; the cause hasn't been looked into yet.
- Bots never farm the jungle and don't coordinate pushes or rotations.
- Mods change data, not behaviour: new cast types or item actives need engine code.
- One map, no sound.
