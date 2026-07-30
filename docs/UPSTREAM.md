# Upstream contributions

Bringing this game up surfaced framework problems rather than game-specific ones.
Everything below landed in `gbarecomp` or `recomp-ui`, not here — a game repo is
the wrong home for an engine fix.

| PR | What | Why Boktai found it |
|---|---|---|
| gbarecomp #1 | `GBARECOMP_SAVE_TYPE` save-chip override | Boktai's `EEPROM_V` signature read as a dead library string |
| gbarecomp #2 | PPU bitmap modes 3, 4, 5 | genuinely unimplemented; narrow impact here |
| gbarecomp #3 | shared `GpioPort` so a cart can carry >1 device | the RTC and the solar sensor share four pins |
| gbarecomp #4 | `codegen_tests` no longer collides with its generated dir | first non-Windows build of that suite |
| gbarecomp #5 | mGBA oracle builds on macOS and Linux | CoreFoundation link + Windows-only patch gating |
| gbarecomp #6 | `GbaSolarSensor`, cart-aware oracle diffing, oracle `.exe` portability | the sensor itself |
| gbarecomp #7 | self-heal works off Windows | the heal compiler path was a hardcoded MSYS2 absolute, the linker flag was PE-only, and the overlay loader was a stub |
| gbarecomp #8 | games can contribute in-game menu items; save/load state in the menu | the solar settings needed a home that was not a keystroke |
| gbarecomp #9 | launcher carries solar settings | set your location before booting |
| gbarecomp #10 | portable macOS + Steam Deck packaging | there was no way to build for either |
| recomp-ui #10 | `RECOMP_RUNTIME_UI_TEXT` item type | a postal code is not a number or a fixed choice |
| recomp-ui #11 | launcher Solar sensor panel | same setting, before boot |

Two of these were bugs in earlier versions of this project's own work, caught by
upstream review conventions: unguarded access to newer `recomp-ui` fields (fixed
with the SFINAE detection pattern the framework already used for gyro), and a
single-owner assignment to the in-game menu's `extra_items` that would have
silently dropped rows once a third contributor existed.

`gbarecomp` also took the solar work further after merge, replacing the
number-row debug keys with rebindable `SolarBrighter` / `SolarDimmer` /
`SolarLive` hotkeys that are unbound by default and surfaced only for cartridges
that declare a sensor.
