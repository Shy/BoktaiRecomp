# Building

## Prerequisites

| | |
|---|---|
| CMake | 3.20+ |
| Compiler | C++20 (Apple Clang 14+, GCC 12+, MSVC 2022) |
| SDL2 | `brew install sdl2` / `apt install libsdl2-dev` / vcpkg |
| Ninja | recommended |

Clone with submodules — `gbarecomp` and `recomp-ui` are both required:

```bash
git clone --recurse-submodules https://github.com/Shy/BoktaiRecomp.git
cd BoktaiRecomp
```

Already cloned without them:

```bash
git submodule update --init --recursive
```

## 1. Supply the assets

See [../baserom.md](../baserom.md). Both are hash-gated and neither is shipped.

## 2. Recompile the BIOS (once)

The framework executes the real BIOS rather than HLE-ing it, so it must be
translated first:

```bash
cmake -B gbarecomp/build -S gbarecomp && cmake --build gbarecomp/build -j
./gbarecomp/build/gba_recompile --bios gbarecomp/bios/gba_bios.bin \
    --config gbarecomp/bios/gba_bios.toml
```

## 3. Recompile the cartridge

```bash
./gbarecomp/build/gba_recompile \
    --rom    variants/boktai1_usa/roms/boktai1_usa.gba \
    --config variants/boktai1_usa/symbols/boktai1_usa_recompile.toml \
    --out    variants/boktai1_usa/generated
```

`generated/` is gitignored: it is derivative of a copyrighted ROM, so it is never
committed and always regenerated locally. CMake fails with a clear message if it
is empty.

## 4. Build

```bash
cmake -B build -S . -G Ninja -DGBAGAME_RECOMP_UI=ON
cmake --build build -j
```

`GBAGAME_RECOMP_UI=ON` (the default) builds the pre-boot launcher and the in-game
settings menu. `OFF` gives a plain SDL2 window — smaller and quicker to iterate
on, but then the solar sensor is only reachable through `--solar-zip` /
`config.ini`, since the menu is what exposes it.

Also available:

| Option | Effect |
|---|---|
| `-DGBARECOMP_RUNTIME_UI_ROOT=<path>` | use a recomp-ui checkout other than the submodule |
| `-DGBARECOMP_ROOT=<path>` | likewise for gbarecomp |

libcurl is optional. Without it the build still succeeds and the weather-driven
sensor reports itself unavailable, leaving the manual hotkeys.

## 5. Run

```bash
cd build && ./BoktaiRecomp ../variants/boktai1_usa/game.toml
```

Run **from the directory holding the executable**. The launcher and the runtime
both resolve `config.ini` / `keybinds.ini` relative to the executable, but
`game.toml` paths resolve relative to the toml itself — so passing the toml
explicitly while sitting in `build/` keeps both halves agreeing. Launching from
the repo root instead makes the launcher write `keybinds.ini` where the runtime
will not read it.

## Tests

The engine's suites are the meaningful ones and live in the submodule:

```bash
ctest --test-dir gbarecomp/build -E "oracle|bios_intro_flawless"
```

## Packaging

See [PACKAGING.md](PACKAGING.md).
