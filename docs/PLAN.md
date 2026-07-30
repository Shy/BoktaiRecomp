# Boktai 1 Recomp — Plan

Static recompilation of *Boktai: The Sun Is in Your Hand* (USA) using
[`mstan/gbarecomp`](https://github.com/mstan/gbarecomp), with the cartridge solar
sensor driven by a real webcam.

**Status:** planning complete, Milestone 0 verified on hardware. 2026-07-27.

---

## Goal

A playable native Boktai 1 on macOS (Apple Silicon) where the in-game solar sensor
reads **actual light** from a USB webcam, so charging the Gun del Sol requires real
sunlight the way the original cartridge did.

---

## Verified facts (measured, not assumed)

Everything below was run on this machine (M4 Max, macOS 26.5.2) before planning.

| Claim | Evidence |
|---|---|
| Engine builds on macOS arm64 | 108/109 targets; `cmake_minimum_required(3.20)` is cmake-4 safe |
| Engine is healthy on macOS | **14/14 test suites pass** (decoder, thumb, interpreter, bus, DMA, timers, IRQ, PPU, function_finder, codegen_shards, heal_gate, selfheal, presentation) |
| SDL2 backend available | Found via Homebrew `sdl2-compat` → real window, not the stub |
| BIOS is authentic | SHA-1 `300c20df…3492` + MD5 `a860e8c0…4f6` match `bios/gba_bios.toml` exactly |
| ROM is an unmodified retail dump | 16 MiB exactly (power of two), title `BOKTAI`, code `U3IE`, maker `A4` (Konami), fixed byte `0x96`, header checksum `0x0C` computed = stored, Nintendo logo byte-identical to the BIOS's internal copy |
| Recompiler handles Konami's toolchain | Cold run, **0 seed symbols**, 0.32 s → **5,081 functions** (arm=32, thumb=5,049), **`undefined=0`**, 597,693 lines of C++ across 2 shards, **compiles clean** |
| Boktai's RTC works for free | `SIIRTC_V` found at `0x58458C`; engine auto-detects the Seiko signature and is already bus-routed |
| Boktai's saves work | `EEPROM_V` at `0x584508`; `GbaSave` implements EEPROM 512 B / 8 KB incl. savestate serialization |
| GPIO plumbing already exists | Bus routes `0xC4–0xC9` to the RTC at [`gba_bus.cpp:126`](gbarecomp/src/gba/gba_bus.cpp:126) (8/16/32-bit paths + writes) |

**ROM identity for the config TOML:**
```toml
[identity]
sha1 = "92bda1b64b84c995a79790f518cfa292ed62f9c7"
md5  = "bc6a2eeeec34ccc66c4eca0dce4d7630"
```

### Notable ROM properties
- Entry branch `0xEA3B6A3E` → **entry target `0x08EDA900`**, i.e. ~14.5 MB into a
  16 MB ROM. Unusual (most games start near `0x08000000`); discovery seeds from there.
- Overwhelmingly **THUMB** (5,049 vs 32 ARM) — size-optimized Konami build.
- 2,003 indirect dispatch sites — these are the future `runtime_dispatch_miss` sources.

---

## Locked decisions

| # | Decision | Rationale |
|---|---|---|
| 1 | **Finder-first symbols**, not a decomp | No Boktai decomp exists. Finder produced 5,081 functions cold with `undefined=0`. Ghidra only if the engine's own "Ghidra mandate" trigger fires |
| 2 | **USA ROM only**, as `variants/boktai1_usa/` | English, matches the `*_usa` convention of every existing target; JP stays possible later |
| 3 | **Never target a sensor-patched ROM** | The "Sensor hack L+R" builds *remove* the real sensor reads, defeating the entire goal |
| 4 | Solar sensor lives **upstream in `src/gba/`** | Repo doctrine: real hardware behavior goes in the core with a primary-source citation |
| 5 | Sensor behind a **`SensorProvider` interface** | Debug slider first, webcam later, fixed value for tests — one seam |
| 6 | **Sensor value joins the deterministic input stream**, 1 sample/frame | Cosim demands deterministic lockstep; a live camera would destroy it. Follows the existing `RECOMP_RTC_EPOCH` precedent |
| 7 | **Webcam, not ambient light sensor** | macOS ALS hardware exists (`AppleSPUALSDriver`) but has **no public API**, faces the user, and isn't portable. A detachable `C922 Pro Stream Webcam` can be aimed at the sky |
| 8 | **Locked exposure + mean luma** | Auto-exposure normalizes brightness away — a naive mean-luma provider would read near-constant regardless of real light |
| 9 | Calibration: **happy medium**, tunable in `game.toml` | Faithful-vs-forgiving deferred until the sensor is actually driving gameplay |
| 10 | **macOS native dev**, no VM | 14/14 tests pass |
| 11 | **Upstream-first for engine changes**; `BoktaiRecomp` independent | Sensor/GPIO/macOS fixes are genuinely general. Rule: *engine changes upstream, game changes ours* |

---

## Repository layout

Mirrors `EmeraldRecomp` (the most evolved existing target).

```
BoktaiRecomp/
├── CMakeLists.txt
├── README.md
├── CLAUDE.md
├── .gitmodules                     # gbarecomp, recomp-ui
├── src/
│   ├── main.cpp
│   ├── game_launcher_boot.{cpp,h}
│   └── boktai_solar_hud.{cpp,h}    # on-screen light meter (game-side, not engine)
├── tools/
│   ├── verify_rom_hash/main.cpp
│   └── import_symbols/import_symbols.py
└── variants/boktai1_usa/
    ├── game.toml                   # sensor calibration curve, view options
    ├── config/boktai1_usa.toml     # identity, data_range, code_copy, jump_table, extra_func
    ├── symbols/
    │   ├── function_boundaries.tsv
    │   └── imported_symbols.tsv
    ├── generated/README.md         # generated C++ is gitignored
    └── launcher/boxart.tga
```

Keeping `variants/` (rather than Minish's flat layout) leaves the JP release as a
drop-in second directory.

---

## The interp-vs-static loop

This is the project's spine, and it is the engine's **native** workflow — not
something bolted on.

1. **Recompile** with the current manifest → static C++ shards.
2. **Run.** Uncovered code does not break anything: unknown targets hit
   `runtime_dispatch`, which falls back to the interpreter (self-heal tier) and
   appends the exact PC to `dispatch_misses.log`.
3. **Measure two things:**
   - *Correctness* — differential cosim, recomp vs `GBARECOMP_FORCE_INTERP=1`,
     halting at the first full-state divergence.
   - *Coverage / speed* — how many instructions were served statically vs
     interpreted, and wall-clock FPS.
4. **Ingest the manifest.** Turn `dispatch_misses.log` entries into
   `[[extra_func]]` / `[[jump_table]]` / `[[data_range]]` in
   `config/boktai1_usa.toml`, then recompile. Repeat.

Misses are cheap and self-reporting; **false positives (data decoded as code) are
catastrophic** and bypass the oracle. So the finder stays conservative and the TOML
closes the gap deliberately — never by widening heuristics.

### Already-known manifest entries

The very first cold recompile translated **the GPIO register window as code** — the
generated start vector falls through into `runtime_dispatch(0x080000C4)`. On a normal
cart that region is ordinary ROM; on Boktai it is the RTC/solar-sensor port. So:

```toml
[[data_range]]
start = 0x080000C4
end   = 0x080000CA        # exclusive
note  = "Cartridge GPIO window (RTC + solar sensor), not code"
```

---

## Solar sensor design

### Hardware model (upstream, `src/gba/gba_solar.{h,cpp}`)

The Boktai cartridge carries a photodiode behind an integrating ADC on the same
4-bit cartridge GPIO port as the RTC. The game pulses a reset/clock line, a counter
ramps, and an input bit flips after N clocks — **N is inversely proportional to
brightness**. The game counts clocks to the flip to obtain its light value.

**Open item, must not be guessed:** the exact pin assignment shared between the RTC
(SCK/SIO/CS on bits 0–2) and the sensor. This requires a primary reference
(GBATEK, mGBA's solar sensor implementation, CowBite) both for correctness and
because repo policy forbids behavior without a cited hardware basis.

### Required refactor

The bus currently routes `0xC4–0xC9` to the RTC **exclusively**, gated on
`rtc_.active()` — and on Boktai the RTC *is* active, so sensor pin reads would be
swallowed. Extract a small shared `GpioPort` that owns the data/direction/control
registers and merges pin state from both attached devices.

### Determinism

Follows the established `RECOMP_RTC_EPOCH` pattern:

- `RECOMP_SOLAR_FIXED=<0-255>` pins the value for cosim and tests.
- `RECOMP_SOLAR_OFF` disables the device entirely.
- Sensor state **is serialized into savestates** and included in the cosim state
  hash. (The RTC currently is *not* serialized — a pre-existing gap worth fixing
  in the same PR series rather than inheriting.)

### `SensorProvider` backends

| Backend | Purpose |
|---|---|
| `FixedProvider` | Tests, cosim, CI |
| `DebugSliderProvider` | Keyboard/UI control — proves the game reacts before any camera work |
| `ReplayProvider` | Reads the recorded input stream |
| `CameraProvider` | AVFoundation (macOS) / Media Foundation (Windows), behind `GBARECOMP_HAVE_*` guards so the author's Windows build stays green |

**Camera pipeline:** lock exposure/ISO/white balance → mean luma over a center-weighted
region → calibration curve from `game.toml` → quantize to one byte → write into the
deterministic input stream once per frame.

**Free test rig:** the installed OBS / Elgato / Anamorphic *virtual* cameras are a
deterministic synthetic light source — feed a known brightness ramp and assert the
sensor tracks it.

---

## Milestones

Each gate must be *measurably* met, per the engine's own roadmap discipline.

| # | Milestone | Gate | Est. |
|---|---|---|---|
| **M0** | Toolchain proven | ✅ **DONE** — builds, 14/14 tests, Boktai recompiles clean | — |
| **M1** | `BoktaiRecomp` scaffold boots | Real BIOS intro plays, then Boktai's first cartridge instruction executes; ROM hash gate refuses anything else | 1–2 days |
| **M2** | Title screen → playable | Menu navigation works; dispatch-miss grind through the early game; EEPROM save round-trips | 1–3 weeks |
| **M3** | Solar sensor in the engine | Debug slider changes in-game sun; upstream PR series landed or in review | 3–5 days |
| **M4** | Cosim gates green | recomp-vs-recomp determinism = 0 divergence with RTC + sensor pinned; then recomp vs `FORCE_INTERP` | 1–2 weeks |
| **M5** | Real light | C922 with locked exposure drives the sensor; virtual-camera ramp test passes | 2–4 days |
| **M6** | Playable & tuned | Full playthrough viable; calibration curve settled; on-screen light meter | ongoing |

M4's estimate is deliberately loose: Boktai **has an RTC**, and `COSIM_ORACLE.md`
notes MinishCap's *lack* of one is what made it the cleanest first fixture. Expect
real Gate-1 determinism work here.

---

## Upstream PR series (`mstan/gbarecomp`)

Ordered smallest-first to build review credibility before the substantial change.

1. **macOS/Unix build fix.** `codegen_tests` is both a generated *directory*
   ([`CMakeLists.txt:621`](gbarecomp/CMakeLists.txt:621)) and an *executable*
   ([`:637`](gbarecomp/CMakeLists.txt:637)). On Windows the exe carries `.exe` so they
   coexist; on Unix the linker fails with `errno=21 (Is a directory)`. One-line fix
   (rename the generated dir or set `OUTPUT_NAME`). **This is the only failure on macOS.**
2. **Docs fix.** `RELEASE_NOTES.md` states the BIOS CRC32 is `0x21A2AE0A`; the
   authoritative constant in [`gba_bios.h:33`](gbarecomp/src/gba/gba_bios.h:33) is
   `0x81977335`, which is what a genuine BIOS actually hashes to. (Harmless today —
   the gate is SHA-1 alone — but misleading.)
3. **Shared `GpioPort`** extracted from `GbaRtc`, behavior-preserving, tests included.
4. **`GbaSolarSensor`** with GBATEK/mGBA citations, `RECOMP_SOLAR_*` pinning, savestate
   serialization, unit tests.
5. **`SensorProvider`** interface + guarded platform camera backends.
6. **Bonus:** RTC savestate serialization gap.

---

## Questions for the author (Discord)

1. **Pin assignment** for RTC + solar sensor sharing the cartridge GPIO port — does he
   have a preferred reference, and would he take the sensor upstream?
2. **Coverage telemetry** — is there an existing report of *% instructions served
   statically vs interpreted*? That is the literal "measure interp vs static" number.
3. **`FORCE_INTERP` maturity** — `COSIM_ORACLE.md` describes cosim as *"DESIGN — plan-first,
   no engine code yet."* `g_force_interp` exists in
   [`runtime_arm.h:145`](gbarecomp/src/armv4t/runtime_arm.h:145); how usable is it today?
4. **macOS interest** — is he open to macOS as a supported target, or tolerated?
5. **Manifest workflow** — canonical loop for feeding `dispatch_misses.log` back into
   the config TOML. Any tooling, or hand-edit?
6. **Low cold coverage** — 5,081 functions from a 16 MB ROM feels low. Expected for a
   cold no-symbol run, or a sign discovery is stalling on Konami's indirect dispatch?

---

## Risks

| Risk | Severity | Mitigation |
|---|---|---|
| Sensor/RTC pin sharing undocumented | **High** | Primary sources (GBATEK, mGBA); ask the author. Blocks M3 only |
| Long dispatch-miss grind (16 MB ROM, 2,003 indirect sites) | **Medium** | Hybrid fallback means the game *runs* throughout; coverage is incremental, never a gate |
| Cosim immature + Boktai has an RTC | **Medium** | Pin RTC + sensor; start Gate 1 on recomp-vs-recomp, which needs no second backend |
| AVFoundation code the author can't compile | **Medium** | Hard `GBARECOMP_HAVE_*` guards; keep camera code out of the shared path |
| Locked exposure can't span indoor → direct sun | **Low** | Fall back to deriving luminance from exposure metadata (EV/APEX) |
| macOS support drifts (only 1 known break so far) | **Low** | Upstream the fix; run the suite on every bump |

---

## Immediate next actions

1. Scaffold `BoktaiRecomp/` per the layout above, with the verified `[identity]` block.
2. Add the `0x080000C4` `data_range` exclusion before the first real recompile.
3. Wire `src/main.cpp` + CMake against the `gbarecomp` submodule (crib from `EmeraldRecomp`).
4. Boot to the BIOS intro → first cartridge instruction (**M1**).
5. Open upstream PR #1 (the one-line macOS fix) — also the Discord icebreaker.
