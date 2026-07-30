# Bring-up log

State as of 2026-07-27. Everything here was measured on an M4 Max, macOS 26.5.2,
AppleClang 21, SDL2 via Homebrew `sdl2-compat`.

## Where it got to

Working:

- Native arm64 macOS build of engine + game (6.7 MB `Mach-O` executable).
- BIOS and ROM both hash-verify and gate startup.
- BIOS recompiled to 770 functions; cart recompiled to 5,087 functions,
  `undefined=0`, ~600k lines of C++ in 2 shards, in 0.27 s.
- **The recompiled GBA BIOS intro renders on screen** (confirmed visually:
  the Nintendo logo boot animation plays).
- Cart code executes; the game writes a full palette (1024/1024 bytes),
  VRAM, and OAM.
- `unmapped=0`, `io_unhandled=0` throughout — no missing memory or IO handlers.
- Performance: 1200 frames of static execution in 2.7 s (~7× realtime).

Reached the timezone and character-creation screens; both report a save failure
("Failed to save settings." / "Could not create data."). Root-caused to the
runtime configuring the **wrong save chip** — the game drives the 0x0E
SRAM/FLASH window and never the 0x0D EEPROM window, while the ROM's only
signature is `EEPROM_V122`. See `../PR-save-type-override.md`. **Diagnosed, not
fixed:** forcing SRAM gets closer but still never sets the dirty flag.

Still open:

- Static recompilation wedges before the render loop (see below); play currently
  needs `GBARECOMP_FORCE_INTERP=1`.
- Audio is wrong (`m4a` probe reports `SoundInfo ptr 0x00000000`; audio loudness
  is also an open item in the engine's own accuracy burndown).

## HARDWARE CHECK: the mode-4 swirl is NOT real Boktai behaviour

Verified against a real cartridge: Boktai 1 goes straight from the GBA BIOS to
the "Licensed by Nintendo" screen. There is **no** animated swirl. Our build
draws one, which means we diverge at or shortly after the BIOS-to-cartridge
handoff — the engine's own SYNC RULES put that at debugging priority 1.

Implementing bitmap modes did not cause this; it made an existing divergence
visible instead of blank. **Next lead: find why the game enters a mode-4
sequence at all.** Compare the first cartridge instructions and the first
DISPCNT writes against mGBA via `oracle/diff_*.py`.

## The PPU had no bitmap-mode renderer (fixed, but narrower than first thought)

**Status: fixed.** `render_bitmap_bg` was added to both scanline renderers in
`gbarecomp/src/gba/gba_ppu.cpp` (+128 lines, one file). Boktai now renders its
animated intro under `GBARECOMP_FORCE_INTERP=1`, and all 14 engine test suites
still pass. Write-up for upstream: `../PR-ppu-bitmap-modes.md`.

Caveat: the **static** build still wedges before reaching the render loop, so
the fix is only visible in interpreter mode today. That is the separate
static-coverage bug below, not a PPU issue.

### The original diagnosis

**It is not a hang.** Boktai renders in **BG mode 4** and gbarecomp's PPU only
implements modes 0, 1 and 2 — [`gba_ppu.cpp:668`](../gbarecomp/src/gba/gba_ppu.cpp:668):

```
668:    if (bg_mode == 0) {
673:    } else if (bg_mode == 1) {
677:    } else if (bg_mode == 2) {
```

There is no branch for modes 3/4/5 (the bitmap modes). The only mention of them
in the whole file is an OBJ-tile-base comment at `:1391`. So the PPU composites
nothing and emits only the backdrop — palette entry 0 — which is exactly the
flat colour on screen.

Evidence chain:

1. Full MMIO capture from reset shows the game reaching video init:
   `cycle=78335710  pc=0x03003134  DISPCNT=0x9444` → **mode 4**, forced blank
   off, BG2 + OBJ enabled. (The 212 earlier DISPCNT writes are all from BIOS
   `pc=0x1bc2` animating the Nintendo intro.)
2. The mode-4 framebuffer is genuinely populated:
   `mode4_frame0[0x00000..0x09600]=7938` non-zero bytes,
   `mode4_frame1[0x0A000..0x13600]=0` (correct — DISPCNT bit 4 = 0 selects
   frame 0), `objtiles[0x14000..0x18000]=7377`.
3. VRAM occupancy **oscillates** across long runs — 15,315 (3600f) → 16,199
   (8000f) → 13,055 (16000f) — i.e. an active clear-and-redraw loop, not a
   stalled one.
4. The 67,491 SWI 6 (Div) calls are consistent with a **software** rasteriser,
   which is what mode 4 implies.
5. The "guest has not HALTed" hang report is a false positive here: a
   compute-bound software renderer legitimately doesn't idle.

So the game has been running correctly and invisibly the whole time.

**Fix: implement BG modes 3/4/5 in `gba_ppu.cpp`.** Mode 4 is the simplest —
240×160 8-bit palette indices at `0x06000000` (or `0x0600A000` when DISPCNT
bit 4 is set), drawn through BG2's affine transform, then composited with OBJ
and the existing window/blend paths.

## CORRECTED: the solar sensor and RTC are NOT ruled out

Instrumenting every access to the cartridge GPIO window before any
`rtc_.active()` gating gave, identically for static, interp, and a 3× longer
interp run:

```
[cart-probe] gpio_reads=3 gpio_writes=0 eeprom_reads=0 eeprom_writes=0
```

That measurement was taken **too early in boot** and the conclusion drawn from
it was wrong. Re-measured on a run driven to the character-creation screen with
`GBARECOMP_DEMO_INPUT=campaign`:

```
[cart-probe] gpio_rd=337 gpio_wr=120929 eeprom_region_rd=0 eeprom_region_wr=0 sram_region_rw=7784
```

**Over 120,000 GPIO writes.** The cartridge GPIO port *is* heavily clocked once
the game is actually running, so the RTC and/or the solar sensor are very much
in play — the earlier zero simply meant the game had not reached that code yet.
Do not treat the sensor as irrelevant.

What the same measurement *does* establish is the save bug: the game performs
**zero** accesses to the 0x0D EEPROM window and 2,768+ to the 0x0E SRAM/FLASH
window, while the ROM's only save signature is `EEPROM_V122`. The runtime
therefore configures the wrong chip. See `../PR-save-type-override.md`.

(The probes were reverted; to reproduce, add counters at the six
`case Region::Rom` sites in `gba_bus.cpp` and a VRAM-region counter next to the
`GBARECOMP_IWRAM_DUMP` handler in `runtime.cpp`.)

## Secondary bug: static stalls on a bridged SWI + IntrWait — LOCALIZED

Full write-up: `../ISSUE-static-bridge-swi-intrwait.md`.

Progress this pass: added `resume = true` entries for the 7 interior misses
inside the DMA-copied loader (accepted as `midfn_aliases: 7 entries -> 4 hosts`),
taking dispatch misses **23 → 16**. That did not clear the stall.

Root cause narrowed by diffing SWI logs between backends: **the first 2,305 SWIs
are byte-identical**, then static simply stops — a liveness bug, not corruption.
The stall correlates exactly with the logged `iwflags` column
(`bus_read_u32(0x03007FF8)`, the BIOS IntrWait flag):

| backend | `iwflags != 0` first seen | records after |
|---|---|---|
| static | SWI #2304 | 1 — then dead |
| interp | SWI #2304 | 9,577 — fine |

So it is the **first SWI taken while the IntrWait flag is set, from inside a
Stage-1 interpreter bridge**. Static parks at `final_pc=0x00000008` (BIOS SWI
vector) forever. Not attempted: the fix, which is IRQ-nesting semantics in the
bridge.

## Original framing of the static gap

The engine's differential oracle (`GBARECOMP_FORCE_INTERP=1`) separates
recompiler bugs from runtime/hardware bugs. Both backends stall, but not at the
same place:

| Run | `final_pc` | VRAM non-zero @1200f | @3600f | dispatch misses |
|---|---|---|---|---|
| static | `0x00000008` (BIOS SWI vector) | 7,377 | 7,377 (frozen) | 13 |
| interp | `0x030040F4` (IWRAM) | 13,079 | 15,315 (growing) | 1 |

Read: **static is genuinely wedged** (byte-identical stats at 1200 and 3600
frames), while the interpreter keeps making forward progress. So there are two
distinct problems — a static-coverage problem *and* a deeper one that stops
both backends from rendering.

## Ruled out

- **Missing IO / memory handlers.** `unmapped=0 io_unhandled=0`.
- **Decoder gaps.** `undefined=0` across the whole 16 MiB recompile.
- **The RTC, *as the cause of the early render stall only*.** `RECOMP_RTC_OFF=1`
  gave byte-identical results there. That says nothing about the RTC later in
  boot, where it is clocked heavily — see the correction above.

## What the game is actually doing

`GBARECOMP_SWI_LOG` over 600 frames: **67,491 SWI calls, all of them SWI 0x06
(Div)**. 609 distinct numerators, divisors ranging 1..2055.

Call sites:

| `lr` | count |
|---|---|
| `0x03003748` | 32,594 |
| `0x03003758` | 32,593 |
| `0x03004D9C` | 2,048 |
| `0x03004DD0` | 256 |

The exact powers of two (2048, 256) look like *completed* fixed-count table
builds rather than a spin loop, so this is probably legitimate math work
(divide-heavy table generation) rather than the hang itself.

All four call sites are in **IWRAM**, which is the crux of the static problem.

## The hang, as the engine itself classifies it

The runtime's own hang detector wrote `hang_dump.log`:

```
reason=guest has not HALTed for several seconds — likely a busy-spin freeze (MC-HP-002 class)
pc=0x03003288
cpsr=0x8000001F        (ARM state, System mode, N set)
cycles=153006223
vblank_starts=545
r0=0x000000F9  r13=0x03007ED4  r14=0x02002200  r15=0x03003288
obj@r0=0x000000F9  *(r0+0x5c)=0xE55EC002
obj[0x00..0x60]: E55EC002 E55EC002 E55EC002 ... (repeated)
m4a={"ok":true,"live":false,"reason":"SoundInfo ptr 0x00000000 out of RAM"}
```

Three things fall out of this:

1. **The engine classes this as `MC-HP-002`** — the same busy-spin freeze class as
   the *already-open* Minish Cap hang named in gbarecomp's own `ROADMAP.md`. This
   is not a Boktai-specific misconfiguration; it is an existing engine-level
   defect class that Boktai also trips.
2. **`r0 = 0x000000F9` is a corrupt object pointer.** It is not a RAM address at
   all — `0xF9` lands in the BIOS region, which is why the "object" reads back as
   BIOS code bytes (`0xE55EC002`) repeated. The game is dereferencing garbage, so
   guest state was already corrupted before the spin.
3. **VBlank IRQs are firing** (`vblank_starts=545`), so the IRQ path and PPU
   cadence are alive. The audio engine never initialized (`SoundInfo ptr 0`),
   consistent with an init path that bailed early.

A corrupt pointer plus 13 interpreter-bridged IWRAM PCs points the same
direction: the RAM-resident code is not executing faithfully. Fixing coverage
there is the first thing to try, before chasing the display separately.

## Coverage telemetry exists

`recomp_coverage_U3IE.json` is written automatically and reports
`coverage`, `distinct_misses`, `interpreted_insns`, `healed_native`,
`native_calls`, and `failed`. This is the built-in "interp vs static"
measurement — no need to build one.

## The static-coverage problem: IWRAM-resident code

All 13 bridged PCs are in IWRAM:

```
0x03003024 0x0300303C 0x03003054 0x03003070 0x03003088 0x030030A0   (x1 each)
0x03003D30 (x3)  0x03004D44 (x3)  0x03004D9C (x1579)  0x03004DD0 (x236)
0x08EDA918 0x08EDA920 0x08EDA9B0                                    (x1 each)
```

Boktai's crt0 (at `0x08EDA900` — Konami placed it at the very end of a 16 MiB
ROM) copies code into IWRAM at `0x03003000` and runs it there. `0x03003000`
appears verbatim in crt0's literal pool at `0x08EDA9A4`, confirming the
destination.

The engine auto-generated `recomp_master_misses_U3IE.toml.frag` proposing
`[[extra_func]]` entries for all 13, and flagged two of the runs as jump-table
candidates. **Do not merge it as-is.** The right fix for RAM-resident code is a
`[[code_copy]]` entry mapping the IWRAM range back to its ROM source, so the
recompiler translates it once with the correct bias:

```toml
[[code_copy]]
runtime_start = 0x03003000
source_start  = 0x08??????   # UNKNOWN — must be found
size          = 0x????
```

### SOLVED: the first copy is a DMA of a loader

Decoded from the copier at `0x08EDA9B0` (reachable only after seeding it, since
crt0 arrives via `mov r15,r0`):

```
08EDA9B0  add r0,r15,#0x14           -> r0 = 0x08EDA9CC   (source)
08EDA9B4  mov r2,#0xD2               -> 210 words
08EDA9B8  orr r2,r2,#0x84000000      -> DMA enable | 32-bit
08EDA9BC  mov r3,#0x04000000
08EDA9C0  add r3,r3,#0xD4            -> 0x040000D4 = DMA3SAD
08EDA9C4  stmia r3,{r0,r1,r2}        -> SAD=0x08EDA9CC DAD=0x03000000 CNT=210w
08EDA9C8  bx lr
```

`r1` was set to `0x03000000` by `mov r1,#0x3000000` at `0x08EDA90C`. 210 words =
840 = `0x348` bytes. `0x08EDA9CC` starts `0xE90D4FF0` (`stmdb sp,{r4-r11,lr}`)
then `0xE24DDD1F` (`sub sp,sp,#0x7C0`) — a normal ARM prologue, so it is code.

```toml
[[code_copy]]
runtime_start = 0x03000000
source_start  = 0x08EDA9CC
size          = 0x00000348
```

Effect of adding it, measured:

| | before | after |
|---|---|---|
| functions emitted | 5,090 | **5,113** (arm 41 → 64) |
| interpreted insns | 1,353,627 | **365,658** (−73%) |
| dispatch misses | 12 | 23 |

The 23 new functions are real IWRAM translations — `gf_afunc_03000094`,
`gf_afunc_030000C0`, `gf_afunc_03000228`, `gf_afunc_030002DC`,
`gf_afunc_03000318`. Misses went *up* only because coverage now reaches deeper:
most of the new ones are interior addresses of already-translated functions
("near gf_afunc_…"), which is what `resume = true` / `[[resume_range]]` exists
for. That is the next iteration.

### Why `0x03003000+` can NOT be a `[[code_copy]]`

After the DMA, crt0 at `0x08EDA920` loads `r1 = 0x03003000` (literal at
`0x08EDA9A4`) and jumps to `0x03000000`. So that 840-byte blob is a
**loader/decompressor** that unpacks the game's main section into IWRAM at
`0x03003000+`.

Those bytes are **produced at runtime** — they exist nowhere verbatim in the
16 MiB image, which is exactly why a 64-byte probe from every bridged IWRAM PC
matched nothing. Per gbarecomp's own `docs/ARCHITECTURE.md` this is the "true
self-modifying / streamed code" case that "can't be translated ahead of time at
all," so it must run on the interpreter tier or be healed at runtime.

**On macOS there is no Stage-2 healing** (`load_and_resolve` is Windows-only),
so the hot decompressed routine at `0x03004D9C` (x1579) is permanently
interpreted here. That is a plausible contributor to the freeze and is worth
testing on Windows, where Stage-2 can actually compile it.

### Historical note: why `source_start` looked unknown

`GBARECOMP_IWRAM_DUMP` at frame 600 does **not** contain the code any more —
the surviving non-zero IWRAM regions start at odd addresses (`0x03003009`,
`0x030039B5`), i.e. data/stack, not 4-byte-aligned ARM code. A 64-byte probe
from each bridged PC finds no match anywhere in the ROM.

So the dump is taken too late. Next attempt should either dump IWRAM much
earlier (immediately after crt0's copy), or disassemble crt0 at `0x08EDA900`
to read the copy's source and length directly out of the setup code. Note
crt0's nearby literals include the strings `"main"`, `"nano"`, `"retp"` —
Konami's "nano" runtime library — so the copy loop's operands should be
recoverable from that function.

## Next steps

1. Recover the `[[code_copy]]` source/size by disassembling crt0 at
   `0x08EDA900`, or by dumping IWRAM immediately after the copy.
2. Add the `[[code_copy]]`, recompile, and confirm `dispatch_misses` drops.
3. Separately, find why neither backend enables display: check DISPCNT /
   forced-blank state and the first VBlank IRQ handshake. `oracle/diff_*.py`
   against mGBA or NanoBoyAdvance is the intended tool.
4. Only then resume the solar sensor work in [../PLAN.md](../PLAN.md).

## Upstream macOS bugs found

1. `codegen_tests` is both a generated directory and an executable target;
   collides on any non-Windows platform (`errno=21`). One-line fix.
2. Self-heal's compiler path is hardcoded to `C:/msys64/mingw64/bin/g++.exe`
   ([overlay_compile.cpp:61](../gbarecomp/src/runtime/overlay_compile.cpp:61)).
   It is overridable via `GBARECOMP_HEAL_CXX`, but the default should be
   platform-aware. Stage-2 overlay *loading* is also Windows-only
   (`load_and_resolve` returns "overlay loading unimplemented on this
   platform"), so macOS gets the Stage-1 interpreter bridge only. Not a
   blocker for static recompilation.
3. `RELEASE_NOTES.md` quotes the BIOS CRC32 as `0x21A2AE0A`; the authoritative
   constant in `gba_bios.h` is `0x81977335`, which is what a genuine dump
   hashes to.
