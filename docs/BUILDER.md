# The builder — how a player gets a playable game

## The idea

This repository is **not** a build. It is the thing that *produces* a build on the
player's own machine, from the player's own cartridge dump.

Nothing derived from the ROM is ever distributed. What gets published is a
**builder**, which contains no cartridge-derived code at all and is therefore free
to hand out. The player runs it once, it compiles their copy, and every launch
after that goes straight into the game.

That distinction is what makes automated releases possible: CI can build and
publish the builder without ever seeing a ROM.

## Why the build can't live in the game's own launcher

The obvious idea — "put the build in the launcher" — is circular. recomp-ui is
compiled *into* the game binary, so the launcher does not exist until after the
build. The way out is that a **launcher-only** executable carries no ROM-derived
code, so it can be prebuilt and shipped:

| Component | Derived from ROM? | Publishable? |
|---|---|---|
| builder GUI (links recomp-ui) | no | **yes** |
| `gba_recompile` (the recompiler) | no | **yes** |
| engine + game sources | no | **yes** |
| `variants/*/generated/*.cpp` | **yes** | never |
| `BoktaiRecomp` (the game) | **yes** | never |

recomp-ui already builds exactly such a binary for its own self-test
(`recomp-ui-launcher`, from `src/proto_main.c`), so this is a known-good shape
rather than a new one.

## Status

Implemented in `tools/builder/` (target `BoktaiBuilder`), and verified on macOS:

- configures and links with **no ROM present** (`-DGBAGAME_BUILDER_ONLY=ON`) —
  the property the automated release depends on;
- a full `--build` run took the real cartridge from picked file to a 13 MB native
  binary, which then booted that cartridge and rendered 60 frames;
- pre-flight rejects a wrong dump by SHA-1 before the build starts, and the stamp
  makes a second run a no-op;
- the window renders (verified by screenshot via `--shot`).

The Flatpak manifest and release packaging are implemented. Manifest generation
is tested without a ROM; the complete bundle and first-launch compile still need
verification on a Steam Deck. Windows toolchain packaging is not implemented.

## Shape

One app, self-bootstrapping. Its entry point is a wrapper, not the game:

```
launch:
  read stamp (app version + ROM sha1)
  if built game exists and stamp matches -> exec it
  else                                   -> run builder GUI, then exec it
```

So the player adds **one** thing to Steam, once. First launch builds with a
progress bar; every later launch boots straight in. Gaming Mode never sees a
missing binary, and there is no second icon or re-adding after a build.

### Staleness

The stamp records the app version and the ROM sha1. After an app update the stamp
mismatches and the builder runs again, showing "updated, rebuilding once". The
build tree is kept (see below), so this is usually seconds rather than minutes.

## What is shipped

| Piece | Why |
|---|---|
| prebuilt builder GUI | the wizard; ROM-free |
| prebuilt `gba_recompile` | saves building the recompiler on the user's machine |
| bundled sources (game + gbarecomp + recomp-ui) | ~15 MB; makes the build offline and pins the exact revision the app was tested against |
| a toolchain | see per-platform below |

Sources are bundled rather than fetched so the build works offline, the version
stamp is simply the app version, and there is no "works on my machine" drift
between what was tested and what a user compiles.

## Toolchain, per platform

Building needs a C++ compiler and CMake. A Steam Deck has **neither** and cannot
install them system-wide (immutable root — verified: both absent). A typical
Windows player has neither either. So the builder brings its own.

| Platform | How | Notes |
|---|---|---|
| **Linux / Steam Deck** | Flatpak whose `runtime` is `org.freedesktop.Sdk` | The SDK *is* gcc + CMake. A standard shared Flathub runtime, so the toolchain is a one-time download shared with other apps rather than bundled per-app. |
| **Windows** | bundled `w64devkit` | ~80 MB portable gcc + make, no installer, redistributable. |
| **macOS** | **not possible** — errors with instructions | You can ship clang, but compiling needs Apple's macOS SDK headers and `libSystem.tbd`, which are not redistributable. The builder detects their absence, says `xcode-select --install`, and exits so the user can come back. |

## The window

The builder draws its **own** small ImGui window rather than reusing recomp-ui's
first-run wizard. An earlier draft of this document claimed the wizard's
`setup_preparing` / `setup_status` / `setup_prepare_pulse` job was "directly
reusable as recompile-and-build". Reading the implementation, it is not, for two
concrete reasons:

- **It adopts its output as the ROM.** `launcher_model_poll_prepare_disc()` calls
  `launcher_model_set_rom(m, out_path)` on success. The right output for a build
  is an executable, not a ROM path.
- **There is no progress channel.** `prepare_disc_cb` is
  `int(const char* src, char* out, size_t, char* err, size_t)` — the worker
  thread can report a result and an error, and nothing in between. `setup_status`
  is written once at the start and once at the end; `setup_prepare_pulse` is an
  indeterminate animation. A real `[N/M]` percentage cannot flow through it
  without changing recomp-ui's ABI.

So the builder owns its window. It still reuses the parts of recomp-ui that fit
without modification — the vendored Dear ImGui, `tinyfiledialogs` for native
file pickers, `sha1.c` for asset fingerprints, and `launcher_capture_png()` for
the render self-test — via one `recomp_target_launcher_ui()` call.

It deliberately draws with ImGui's **built-in font** and loads no asset files, so
a missing or broken `assets/` directory cannot stop a player from building. That
font is ASCII-only: an em-dash or ellipsis renders as `?`, so every string drawn
in the window stays ASCII.

Progress is a **real percentage**: `ninja` prints `[N/M] Building …` per edge, so a
piped subprocess gives an exact figure and the current file. On a handheld with no
terminal, a five-minute indeterminate spinner reads as a hang; if ninja's format
ever changes the fallback is an indeterminate bar, not a failure.

Only the recompiler's own `==>` markers reach the screen. Echoing every line put
things like `auto_jt 0x081AE608 count=5 stride=4 site=0x081AE5EE MOVpc bounded`
in front of a player, which means nothing to them and reads as a fault.
Everything still goes to the log.

### The related recomp-ui fix

recomp-ui's wizard hardcoded **PlayStation** text in its BIOS step
(`launcher_imgui.cpp:5113`), even though its own comment says the step serves
`PSX / GBA`. A GBA user was told a *required* 16 KB BIOS is an *optional* 512 KB
`SCPH1001.BIN` with a bundled OpenBIOS that does not exist.

Fixed upstream in [recomp-ui#12](https://github.com/mstan/recomp-ui/pull/12) by
driving those labels off `SystemProfile`, the same pattern that repo already uses
for panel composition. The builder does not depend on it — it does not use that
wizard — but the **game** the builder produces does show it.

## Why the build runs in a copy of the sources

The build writes into the source tree in at least two places: the recompiled
cartridge lands in `variants/<name>/generated/`, and the recompiled BIOS in
`gbarecomp/src/runtime/generated_bios/` (hardcoded in gbarecomp's own
`CMakeLists.txt`, so `--out` alone does not relocate it). A Flatpak's `/app` is
read-only at runtime, so none of that can happen in place.

Two ways out: teach every such path to relocate, or copy the sources once into a
writable directory and build there. This takes the second. It is ~17 MB, it needs
no upstream CMake changes, and — the deciding reason — it is robust to a *third*
such write path existing that grepping did not find.

The copy goes over the top of whatever is already there rather than wiping, so the
build trees inside it survive and a post-update rebuild stays incremental. The
player's ROM and BIOS are **not** copied: their absolute paths are handed to the
recompiler, so nothing copyrighted is ever duplicated by us.

## Failure handling

Pre-flight the cheap things *before* the long wait:

- verify both asset sha1s **at pick time** — a JP or bad dump is rejected in the
  wizard, not after three minutes of compiling;
- check free disk (needs ~200 MB);
- check the toolchain (this is where macOS exits with instructions).

If the build still fails: show the last ~20 lines in the wizard, write the full log
to a known path, and tell the user to send that file. The difference between "it
didn't work" and something diagnosable.

## Disk and rebuild cost

Measured on this project:

| | |
|---|---|
| one generated shard (206 K lines, 7.6 MB) | **2.7 s** at `-O1` |
| full clean build | ~2–3 min desktop, ~3–5 min Deck |
| build tree | 105 MB |
| generated C | 64 MB |
| final binary | 15 MB |
| **incremental relink** | **2.4 s** |

The build tree is **kept**. 170 MB is 0.27 % of a 64 GB Deck, and keeping it turns
the post-update rebuild from minutes into seconds.

## Distribution

CI builds and publishes the builder, because it is ROM-free:

- **Steam Deck / Flatpak hosts**: one `.flatpak` bundle (`BoktaiBuilder.flatpak`),
  attached to a Release. The user installs it in Desktop Mode with one command and
  optionally adds it to Steam:
  ```bash
  flatpak install --user ./BoktaiBuilder.flatpak
  ```
  Flatpak downloads the shared runtime (`org.freedesktop.Sdk 23.08`), which
  includes gcc, g++, CMake and Ninja — the toolchain needed on first launch.
  Because SteamOS has an immutable root, this is the only way: a player cannot
  install build tools system-wide, so the builder carries them in the Flatpak layer.
- **macOS / Linux (desktop)**: a `.tar.gz` archive with the builder, launcher assets,
  a prebuilt gba_recompile and the bundled sources. Extract and run `./BoktaiBuilder`.
  Requires a local C++ compiler and CMake; the builder checks for them and reports
  what to install if they are missing.
- **Windows**: a `.zip` with the builder, `w64devkit` (portable gcc + make) and the
  sources.

No secrets, nothing copyrighted, fully automatable.

## Installation on Steam Deck

1. **Download** `BoktaiBuilder.flatpak` from a Release.

2. **Install** it in Desktop Mode. Open a terminal and run:
   ```bash
   flatpak install --user ~/Downloads/BoktaiBuilder.flatpak
   ```
   (Adjust the path if the file is elsewhere.) Steam Deck already has Flathub
   configured; Flatpak downloads the shared `org.freedesktop.Sdk 23.08` runtime
   during installation if it is not present.

3. **Run it** from Desktop Mode:
   ```bash
   flatpak run tech.recomp.BoktaiBuilder
   ```
   Or find it in the Deck's application menu.

4. **First launch**: the builder window opens. Point it at your ROM and BIOS.
   Both are checked by SHA-1 before anything is built. The build takes 2–5 minutes
   on a Deck (desktop builds are faster, but Deck performance is typical).

5. **After the build**: the game launches into the intro. Subsequent launches skip
   the builder entirely and boot straight to the game.

6. **Add to Steam** (optional, so you can play in Gaming Mode): In Desktop Mode,
   Steam → Games → Add a Non-Steam Game, search for "Boktai" and pick the builder.
   Switch to Gaming Mode and it shows up in your library. Each time you update the
   Flatpak, the builder runs once on next launch (the build is incremental because
   the tree is kept, so this is usually seconds).

## Scope

Built in this repository first, but with the game facts (name, app id, ROM sha1,
symbols path) as **parameters** rather than hardcoded — the same way
`gbarecomp/packaging/` already works. Lifting it into gbarecomp so MinishCap and
Emerald can use it is then mostly a file move. MinishCap has exactly this problem
today: its only distribution is a hand-built Windows zip, and it has no CI at all.

## What this replaces

- `.github/workflows/release.yml` — fetched the ROM from a secret URL to build on
  a runner. That is the thing this design exists to avoid; it should be deleted
  and replaced with a workflow that builds the **builder**.
- `tools/make_release.sh` — still useful as a maintainer tool for packaging a
  local build, but it is no longer the distribution path.
