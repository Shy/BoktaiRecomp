# Known issues

## Coverage is not fully static

The exit banner reports `self_heal_coverage=NOT_STATIC`. Two IWRAM program
counters — `0x03001D40` and `0x03001E4C` — are bridged tens of thousands of times
per session and never heal to native.

They are RAM-resident code reached through a `[[code_copy]]` region rather than
ROM functions, which is a different problem from ordinary missing coverage: the
overlay compiler works from ROM bytes, and there are none for a routine the game
copied into IWRAM at runtime. Proposals are recorded in
`recomp_master_misses_U3IE.toml.frag` for review; per the framework's rules
nothing is auto-merged into `game.toml`.

Everything else heals: a recent session compiled and loaded 103 overlays and
called them 27,223 times.

## Shadows

Shadow rendering next to walls looked wrong during play-testing and was never
run to ground. An early guess ("more light means bigger shadows") was checked
against two screenshots and was simply wrong — the shadow size did not change.
Unexplained.

## mGBA differs from the recomp in one area

A side-by-side comparison showed a different palette (tan vs purple) and enemies
present in mGBA but absent in the recomp. That comparison was **not controlled**:
different boot, different input, and the cartridge RTC was free-running on both
sides, so day/night state alone could explain it.

A controlled run is the open work — fresh boot on both sides, matched synthesized
input, RTC pinned on *both*, in one process pair, diffing PAL/VRAM/OAM and the
blend/window registers at a matched VBlank. `gbarecomp/oracle/diff_cart.py` exists
for exactly this and its docstring warns about the RTC trap.

## Audio

Not systematically compared against hardware or the oracle.

## `GBARECOMP_DEMO_INPUT=campaign` segfaults

A headless run with `GBARECOMP_DEMO_INPUT=campaign` dies with SIGSEGV (exit 139)
somewhere before frame 300. Reproduced at both `--view-width 240` and `256`, so it
is unrelated to the extended view.

The demo tracks are written for Mega Man Zero — the mode list mentions an
"action-platformer stress track", a "saber handoff" and an opening "Golem" — so
they drive Boktai somewhere it was never going to survive. Not obviously a bug in
the tracks so much as them being applied to the wrong game.

Practical effect: there is no scripted way to reach Boktai gameplay headlessly,
which blocks automated characterization of anything that only shows up in-game.
The alternatives are driving `KEYINPUT` over the debug server's `run_frames`, or
committing a save state.
