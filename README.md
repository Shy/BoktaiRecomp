# BoktaiRecomp — Boktai: The Sun Is in Your Hand, Recompiled

Static recompilation of **Boktai: The Sun Is in Your Hand** (Game Boy Advance) to
native PC, built on the [`gbarecomp`](https://github.com/mstan/gbarecomp)
framework.

Boktai's cartridge carried a **photodiode**: the Gun del Sol charged from real
sunlight, and the undead could only be sealed while you had light. That sensor is
emulated here and driven by **the actual weather where you are** — a clear
afternoon fills the gauge, a rainy night leaves you fighting in the dark.

> ### Status — playable, early, self-improving
>
> A **static-recompilation base + runner**, not a finished port. It boots through
> the real recompiled BIOS into gameplay, saves to EEPROM, and the solar sensor
> works end to end.
>
> Coverage is **not yet fully static**. Uncovered code paths run through the
> interpreter on first hit, then JIT to native and cache to disk, so the game
> gets faster the more you play. Open items: [ISSUES.md](ISSUES.md).

---

## What "static recompilation" means here

The ROM's **ARM7TDMI machine code is statically translated to native C** — every
function the game runs becomes a real generated C function. As across the
`gbarecomp` ecosystem, **the GBA BIOS is recompiled and executed too** (never
HLE'd or stubbed), so boot, IRQ and SWI handling run as real recompiled code. The
PPU, APU, DMA, timers, the EEPROM save chip, the S-3511A RTC and the solar sensor
are modeled by the runtime.

**The ROM is never redistributed.** You supply your own legally-dumped copy, and
the runner refuses to launch on anything else.

## ROM

| Target         | Game                            | Region | SHA-1                                      | Debug port |
|----------------|---------------------------------|--------|--------------------------------------------|------------|
| `BoktaiRecomp` | Boktai: The Sun Is in Your Hand | USA    | `7164326283df46a3941ec7b6ceca889cbc40e660` | 19893      |

Save chip: **EEPROM** (8 KB). Cartridge extras: **S-3511A RTC** and the **solar
sensor**, sharing four GPIO pins.

> **Use a clean dump.** Many Boktai ROMs in circulation are intro-patched or
> sensor-patched, and a sensor patch removes the very hardware reads this project
> emulates. See [baserom.md](baserom.md).

## Quick start

```bash
git clone --recurse-submodules https://github.com/Shy/BoktaiRecomp.git
```

1. Supply a GBA BIOS dump and your own ROM — [baserom.md](baserom.md).
2. Recompile and build — [docs/BUILDING.md](docs/BUILDING.md).
3. Run it. The launcher opens first: pick your ROM, set your postal code under
   **Settings → Solar sensor**, press **PLAY**.

Prebuilt binaries are not distributed: a recompiled binary embeds translated ROM
code, so everyone builds their own. Packaging scripts for macOS, Windows and
Steam Deck are in [docs/PACKAGING.md](docs/PACKAGING.md).

## The solar sensor

Three light sources, in precedence order:

| Source | How |
|---|---|
| Hotkey override | `SolarBrighter` / `SolarDimmer` step a manual level; `SolarLive` releases it. Unbound by default — bind them in the launcher's Hotkeys panel. |
| Live weather | Postal code → coordinates → current irradiance. Set it in the launcher, or in the in-game menu (**Esc**). |
| Nothing configured | The sensor reads dark. No network request is ever made. |

Live weather uses **global horizontal irradiance** (W/m²) — the power a flat
upward-facing surface actually receives — mapped onto the gauge's measured
response:

| Irradiance | Conditions | Gauge |
|---|---|---|
| 0 | night | empty |
| 50–150 | heavy overcast | 1–3 bars |
| 200–400 | light overcast, low sun | 4–6 bars |
| 800–1000 | clear midday | full |

**Full sun** is adjustable, because clear-sky midday is ~900 W/m² at mid
latitudes but much less in winter or at high latitude — a fixed 900 would mean
the gauge could never fill on a genuinely sunny day.

**Privacy.** Nothing leaves the machine unless you set a postal code. When you
do, that code goes to `api.zippopotam.us` to resolve coordinates, and those
coordinates go to `api.open-meteo.com` for the current reading. No account, no
API key, nothing else sent. Polling is every 10 minutes, never per frame.
Details: [docs/SOLAR-SENSOR.md](docs/SOLAR-SENSOR.md).

## Controls

Defaults match the launcher's rebind page, so the two never disagree:

| GBA | Keyboard |
|---|---|
| A / B | X / Z |
| L / R | C / V |
| Start / Select | Return / Right Shift |
| D-pad | Arrow keys |

| Function | Key |
|---|---|
| In-game settings menu | **Esc** |
| Load / save state (slots 1–9) | **F1–F9** / **Shift+F1–F9** |
| Fullscreen | **Alt+Return** |
| Turbo | **Tab** |

All rebindable — inputs on the launcher's Controls page, hotkeys in its Hotkeys
panel. Both write the same `keybinds.ini` / `config.ini` the runtime reads.

## Layout

| Path | What |
|---|---|
| `variants/boktai1_usa/game.toml` | ROM identity, save chip, video/audio policy |
| `variants/boktai1_usa/symbols/` | recompiler input: entry seeds, data ranges, code copies |
| `variants/boktai1_usa/generated/` | translated C — **gitignored**, regenerate locally |
| `src/solar_weather.*` | postal code → coordinates → irradiance → sensor brightness |
| `src/game_ui.*` | this game's section of the in-game settings menu |
| `tools/` | ROM hash verifier, release packaging |
| `packaging/flatpak/` | Steam Deck build |

The `variants/` layout follows
[EmeraldRecomp](https://github.com/mstan/EmeraldRecomp) so the JP release and the
Boktai sequels can drop in as sibling directories.

## Upstream

Bringing this game up produced fixes that belonged in the framework rather than
here — PPU bitmap modes 3/4/5, a shared GPIO port so a cart can carry more than
one device, save-chip override, the solar sensor itself, game-owned in-game menu
items, and self-healing on non-Windows hosts. All merged.
See [docs/UPSTREAM.md](docs/UPSTREAM.md).

## License

[PolyForm Noncommercial 1.0.0](LICENSE), matching `gbarecomp`.
