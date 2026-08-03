#!/usr/bin/env bash
# package_builder.sh — assemble the distributable BUILDER payload.
#
#   tools/package_builder.sh
#
# ---------------------------------------------------------------------------
# What this produces, and why it is publishable
#
# The game binary is translated cartridge code and cannot be distributed. The
# BUILDER can: it contains no cartridge-derived code at all, which is why this
# script needs no ROM, no BIOS and no secrets, and can therefore run on a hosted
# CI runner. See docs/BUILDER.md.
#
# Layout:
#   <name>-<platform>-<arch>/
#     BoktaiBuilder          the wrapper + first-run builder GUI
#     assets/                launcher chrome, for the game it produces
#     lib*/…                 bundled libraries (from the platform script)
#     sources/               the sources the build runs against, minus anything
#                            copyrighted, .git, and prebuilt trees
#     sources/bin/gba_recompile   prebuilt, so the player does not compile the
#                            compiler (this is where the builder looks for it)
# ---------------------------------------------------------------------------

set -euo pipefail

NAME="${NAME:-BoktaiBuilder}"
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

case "$(uname -s)" in
    Darwin) PLATFORM_SCRIPT=package_macos.sh; PLATFORM=macos; EXE_SUFFIX="" ;;
    Linux)  PLATFORM_SCRIPT=package_linux.sh; PLATFORM=linux; EXE_SUFFIX="" ;;
    MINGW*|MSYS*) PLATFORM_SCRIPT=package_windows.sh; PLATFORM=windows; EXE_SUFFIX=".exe" ;;
    *) echo "unsupported host $(uname -s)" >&2; exit 1 ;;
esac

if [ ! -x "gbarecomp/packaging/$PLATFORM_SCRIPT" ]; then
    echo "gbarecomp/packaging/$PLATFORM_SCRIPT is missing." >&2
    echo "The submodule predates the packaging scripts (upstream PR #10)." >&2
    exit 1
fi

# ---- 1. the builder binary + its libraries + launcher assets ----
# GBAGAME_BUILDER_ONLY skips the game target, so this configures and links with
# no ROM present. That is the property the whole release model rests on: if it
# ever stopped holding, this step would fail rather than quietly need a ROM.
echo "==> build and stage the builder"
"gbarecomp/packaging/$PLATFORM_SCRIPT" \
    --target BoktaiBuilder --name "$NAME" \
    --build-dir build-builder-release \
    -DGBAGAME_BUILDER_ONLY=ON

OUT="$(echo release-stage/"$NAME"-"$PLATFORM"-*/ | head -1)"
OUT="${OUT%/}"
[ -d "$OUT" ] || { echo "the platform script produced no directory" >&2; exit 1; }
echo "    staged in $OUT"

# ---- 2. the prebuilt recompiler ----
# Shipping it means a player's first build does not start by compiling the
# recompiler (and does not need network for the toml++ FetchContent that step
# would otherwise trigger). The builder falls back to building it from the
# bundled sources if this is absent, so this is an optimisation, not a
# requirement.
echo "==> build and stage gba_recompile"
cmake -B build-recompiler -S gbarecomp -DCMAKE_BUILD_TYPE=Release \
      > /dev/null
cmake --build build-recompiler --target gba_recompile --parallel
mkdir -p "$OUT/sources/bin"
cp "build-recompiler/gba_recompile$EXE_SUFFIX" "$OUT/sources/bin/"

# ---- 3. the sources the build runs against ----
# Bundled rather than fetched so the build works offline, so the version stamp is
# just the app version, and so there is no drift between what was tested and what
# a player compiles.
#
# The exclusions mirror tools/builder/builder_core.cpp's own copy filter AND the
# flatpak skip list: never a ROM, never a BIOS, never a prebuilt tree. -type f is
# deliberately NOT used when looking for cartridges — this project's own ROM is a
# symlink, and -type f would walk straight past it.
echo "==> stage sources"
tar -cf - \
    --exclude='.git' \
    --exclude='.github' \
    --exclude='build' \
    --exclude='build-*' \
    --exclude='release-stage' \
    --exclude='flatpak-build' \
    --exclude='roms' \
    --exclude='*.gba' \
    --exclude='*.agb' \
    --exclude='gba_bios.bin' \
    --exclude='generated' \
    --exclude='bios_recompiled.*' \
    --exclude='bios_symbol_map.cpp' \
    . | (cd "$OUT/sources" && tar -xf -)

# ---- 4. refuse to ship anything copyrighted ----
# The exclusions above are supposed to guarantee this. Assert it anyway: the
# consequence of a regression here is publishing a cartridge dump.
echo "==> assert the payload carries no ROM or BIOS"
if find "$OUT" \( -name '*.gba' -o -name '*.agb' -o -name 'gba_bios.bin' \) \
     | grep .; then
    echo "the payload contains cartridge or BIOS data — refusing" >&2
    exit 1
fi
# Recompiled output is derived from the cartridge, so it must not be here either.
if find "$OUT" -name 'recompiled_*.cpp' -o -name 'bios_recompiled.cpp' | grep .; then
    echo "the payload contains recompiled sources — refusing" >&2
    exit 1
fi
echo "    clean"

# ---- 4b. a README that describes the BUILDER ----
# The platform script writes a README for a GAME payload ("the launcher prompts
# for both on first run and caches the paths beside this file"), which is not what
# this is: the builder caches in the per-user data directory, not next to the
# executable, and the first run compiles rather than plays. Overwrite it, keeping
# the platform-specific quarantine note, because this is the first thing a player
# reads.
echo "==> write a builder README"
{
cat <<EOF
$NAME — $PLATFORM

This archive contains NO GAME DATA. It contains a builder: it compiles Boktai on
this machine from your own legally-dumped cartridge, which never leaves it.

  1. Run ./BoktaiBuilder$EXE_SUFFIX from this directory.
  2. Point it at your Boktai (USA) cartridge dump and your Game Boy Advance
     BIOS. Both are checked by SHA-1 before anything is built, so a bad dump is
     rejected in seconds rather than after a long compile.
  3. It compiles for a few minutes, once. Every launch after that goes straight
     into the game, so keep using this same shortcut.

Your ROM and BIOS are read where they are and never copied. The build output and
a log go in your user data directory; run with --paths to see exactly where.

Building needs a C++ compiler and CMake. The builder checks for them before it
starts and tells you what to install if they are missing.
EOF
if [ "$PLATFORM" = macos ]; then
    cat <<EOF

If macOS refuses to open it after a download:
    xattr -dr com.apple.quarantine "$(basename "$OUT")"
EOF
fi
} > "$OUT/README.txt"

# ---- 5. prove the builder runs from the payload ----
# --paths exercises argv0 resolution and the sources/ discovery, which is exactly
# what differs between a developer checkout and a shipped layout. Getting this
# wrong is how a payload ships that cannot find its own sources.
echo "==> smoke-test the staged builder"
FOUND="$("$OUT/BoktaiBuilder$EXE_SUFFIX" --paths | awk '/^sources/ {print $3}')"
case "$FOUND" in
    */sources) echo "    resolved its bundled sources: $FOUND" ;;
    *) echo "the staged builder resolved sources to '$FOUND', not its own"\
            "sources/ directory" >&2; exit 1 ;;
esac

# ---- 6. repack ----
echo "==> repack"
ARCHIVE="$(basename "$OUT").tar.gz"
( cd release-stage && rm -f "$ARCHIVE" && tar -czf "$ARCHIVE" "$(basename "$OUT")" )
echo
echo "release-stage/$ARCHIVE"
du -sh "$OUT" | awk '{print "    unpacked: "$1}'
du -h "release-stage/$ARCHIVE" | awk '{print "    archive:  "$1}'
