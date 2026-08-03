// builder_core.h — turn a player's cartridge dump into a playable binary.
//
// This is the engine behind the builder app. It is deliberately UI-free: the
// GUI (builder_main.cpp) and the headless `--build` path drive the same code, so
// what CI exercises is what a player runs.
//
// ---------------------------------------------------------------------------
// Why a builder exists at all
//
// A recompiled binary is translated cartridge code, so it cannot be
// distributed. What CAN be distributed is the machinery that produces one. So
// the player gets a ROM-free builder, supplies their own dump once, and every
// launch after that goes straight into the game. See docs/BUILDER.md.
//
// ---------------------------------------------------------------------------
// Two properties worth stating, because they are the whole point
//
// 1. The player's ROM and BIOS are read in place and never copied into our
//    tree. Their absolute paths are handed to the recompiler; nothing
//    copyrighted is written anywhere except the build output, which stays on
//    the player's machine.
//
// 2. The build runs against a COPY of the sources in a writable directory, not
//    the shipped ones. A Flatpak's /app is read-only at runtime, and the build
//    writes into the source tree in at least two places (the recompiled
//    cartridge under variants/<name>/generated/, and the recompiled BIOS under
//    gbarecomp/src/runtime/generated_bios/). Copying ~15 MB once is cheaper
//    than teaching every one of those paths to relocate, and it is robust to a
//    third such path existing that we have not found.

#pragma once

#include <functional>
#include <string>
#include <vector>

namespace boktai::builder {

// ---- where things live ------------------------------------------------------
struct Paths {
    // Read-only sources as shipped (or a developer's checkout).
    std::string src_root;
    // Writable per-user directory: the source copy, the build trees, the built
    // game, the stamp and the log all live under here.
    std::string data_root;

    // data_root/src — the source copy the build actually runs in.
    std::string work_src() const;
    // The built game binary, once there is one.
    std::string game_binary() const;
    std::string stamp_file() const;
    std::string log_file() const;
};

// Resolve the standard locations from the running executable's directory.
// data_root follows the platform convention ($XDG_DATA_HOME, ~/Library/
// Application Support, %LOCALAPPDATA%), which is also what a Flatpak
// transparently redirects into ~/.var/app/<id>/.
Paths default_paths(const char* argv0);

// ---- the player's assets ----------------------------------------------------
struct Assets {
    std::string rom;    // absolute path to their cartridge dump
    std::string bios;   // absolute path to their GBA BIOS dump
};

// ---- what the game is (parameters, not hardcoded facts) ---------------------
// Kept as data so lifting this into gbarecomp for another title is a file move
// rather than a rewrite — the same way gbarecomp/packaging/ already works.
struct GameSpec {
    const char* variant;        // "boktai1_usa"
    const char* target;         // CMake target / binary name
    const char* rom_sha1;       // expected 40-hex cartridge fingerprint
    const char* bios_sha1;      // expected 40-hex BIOS fingerprint
    const char* recompile_toml; // relative to src_root
    const char* app_version;    // bumped whenever a rebuild is required
};

const GameSpec& game_spec();

// ---- progress ---------------------------------------------------------------
enum class Step {
    CopySources,
    Recompiler,
    Bios,
    Cartridge,
    Configure,
    Compile,
    Done,
};

struct Progress {
    Step        step     = Step::CopySources;
    float       fraction = 0.0f;   // 0..1 overall, monotonic
    std::string headline;          // "Compiling the game"
    std::string detail;            // "412/1180   recompiled_07.cpp"
};

// Called from the build thread. The GUI copies out of it under its own lock.
using ProgressFn = std::function<void(const Progress&)>;

// ---- pre-flight ------------------------------------------------------------
// Everything cheap that can fail, checked BEFORE the multi-minute wait: asset
// fingerprints, free disk, and a working toolchain. On macOS without the
// Command Line Tools this is the check that fails, with `remedy` set to the
// command that fixes it — which is the whole macOS story (see docs/BUILDER.md).
struct Preflight {
    bool        ok = false;
    std::string error;    // one user-facing sentence
    std::string remedy;   // a command to run, or empty
};

Preflight preflight(const Paths& p, const Assets& a);

// Verify one file against an expected SHA-1. `actual_hex` is always filled when
// the file could be read, so a caller can show the real digest on mismatch.
bool verify_sha1(const std::string& path, const char* want_hex,
                 std::string& actual_hex);

// ---- the build -------------------------------------------------------------
// Runs to completion (minutes). Returns false and fills `err` with a
// user-facing sentence on failure; the full transcript is always in
// p.log_file(). Safe to re-run: it is incremental.
bool build(const Paths& p, const Assets& a, const ProgressFn& on_progress,
           std::string& err);

// ---- staleness -------------------------------------------------------------
// The stamp records the app version plus both asset fingerprints. A mismatch
// means "build (or rebuild) before launching"; the build tree is kept, so a
// post-update rebuild is usually seconds.
bool stamp_ok(const Paths& p, const Assets& a);
void stamp_write(const Paths& p, const Assets& a);

// Read back the ROM/BIOS a previous build used, so a returning player does not
// have to pick their files again. Returns false when there is no usable stamp.
bool stamp_read_assets(const Paths& p, Assets& out);

// True when a built game exists AND the stamp matches — i.e. launch it.
bool ready_to_play(const Paths& p, const Assets& a);

// ---- exposed for tests ------------------------------------------------------
// Parse a ninja progress line ("[412/1180] Building CXX object …"). Returns
// false for any other line. `file` is the last path-like token, or empty.
bool parse_ninja_line(const std::string& line, int& done, int& total,
                      std::string& file);

}  // namespace boktai::builder
