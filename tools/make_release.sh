#!/usr/bin/env bash
# make_release.sh — build locally, then attach the artifacts to a draft GitHub
# Release. Run from the repository root:
#
#   tools/make_release.sh v0.0.1
#
# ---------------------------------------------------------------------------
# Why this is a local script and not a CI job
#
# Building requires the cartridge dump and the GBA BIOS. Doing that on a hosted
# runner would mean putting them somewhere GitHub can fetch — a URL in a secret,
# an artifact, a private release asset — and they would pass through
# infrastructure you do not control on every build.
#
# So the split is: the ROM and BIOS never leave this machine, and the only thing
# uploaded is the packaged binary. Note that the binary is still produced FROM the
# ROM (that is what static recompilation is), so publishing it distributes
# translated cartridge code — which is why the release is created as a DRAFT and
# publishing stays a deliberate act.
#
# CI (.github/workflows/ci.yml) keeps verifying everything that does not need a
# ROM: the engine, its suites, and this repo's own sources on three platforms.
# ---------------------------------------------------------------------------

set -euo pipefail

TAG="${1:-}"
REPO="${RELEASE_REPO:-Shy/BoktaiRecomp}"
TARGET=BoktaiRecomp
NAME=Boktai

if [ -z "$TAG" ]; then
    cat >&2 <<'USAGE'
usage: tools/make_release.sh <tag>            e.g. tools/make_release.sh v0.0.1

  RELEASE_REPO=owner/repo   override the target repository
  SKIP_UPLOAD=1             build and package only, do not touch GitHub
USAGE
    exit 2
fi

case "$TAG" in
    v[0-9]*) ;;
    *) echo "tag should look like v0.1.0, got '$TAG'" >&2; exit 2 ;;
esac

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

ROM="variants/boktai1_usa/roms/boktai1_usa.gba"
BIOS="gbarecomp/bios/gba_bios.bin"
ROM_SHA1=7164326283df46a3941ec7b6ceca889cbc40e660
BIOS_SHA1=300c20df6731a33952ded8c436f7f186d25d3492

sha1_of() {
    if command -v shasum >/dev/null 2>&1; then shasum -a 1 "$1" | cut -d' ' -f1
    else sha1sum "$1" | cut -d' ' -f1; fi
}

echo "==> check assets"
for pair in "$ROM:$ROM_SHA1:cartridge dump" "$BIOS:$BIOS_SHA1:GBA BIOS"; do
    path="${pair%%:*}"; rest="${pair#*:}"; want="${rest%%:*}"; what="${rest#*:}"
    if [ ! -f "$path" ]; then
        echo "missing $what at $path" >&2
        echo "see baserom.md — neither is distributed with this repository" >&2
        exit 1
    fi
    got="$(sha1_of "$path")"
    if [ "$got" != "$want" ]; then
        echo "$what sha1 mismatch:" >&2
        echo "  got      $got" >&2
        echo "  expected $want" >&2
        exit 1
    fi
    echo "    $what ok ($want)"
done

# Recompiled sources are derivative of the ROM and are never committed, so they
# have to exist locally before anything can be packaged.
echo "==> recompile"
if [ ! -x gbarecomp/build/gba_recompile ]; then
    cmake -B gbarecomp/build -S gbarecomp -DCMAKE_BUILD_TYPE=Release
    cmake --build gbarecomp/build --target gba_recompile --parallel
fi
# The BIOS is executed, not HLE'd, so it must be translated before the game links.
if [ ! -s gbarecomp/src/runtime/generated_bios/bios_recompiled.cpp ]; then
    ./gbarecomp/build/gba_recompile --bios "$BIOS" \
        --config gbarecomp/bios/gba_bios.toml
fi
./gbarecomp/build/gba_recompile \
    --rom    "$ROM" \
    --config variants/boktai1_usa/symbols/boktai1_usa_recompile.toml \
    --out    variants/boktai1_usa/generated

echo "==> package"
case "$(uname -s)" in
    Darwin) SCRIPT=package_macos.sh ;;
    Linux)  SCRIPT=package_linux.sh ;;
    *) echo "unsupported host $(uname -s); on Windows use tools/package_release.ps1" >&2
       exit 1 ;;
esac
if [ ! -x "gbarecomp/packaging/$SCRIPT" ]; then
    echo "gbarecomp/packaging/$SCRIPT is missing." >&2
    echo "The submodule predates the packaging scripts (upstream PR #10)." >&2
    exit 1
fi
"gbarecomp/packaging/$SCRIPT" --target "$TARGET" --name "$NAME" \
    -DGBAGAME_RECOMP_UI=ON

# The packaging scripts already smoke-test --help. This proves the packaged
# binary boots the real cartridge, which --help cannot tell you.
echo "==> verify the package boots the ROM"
DIR="$(echo release-stage/"$NAME"-*/ | head -1)"
( cd "$DIR" && GBARECOMP_NO_LAUNCHER=1 ./"$TARGET" --frames 60 --no-window \
    --bios "$ROOT/$BIOS" --rom "$ROOT/$ROM" > /tmp/mkrel-boot.log 2>&1 )
grep -q 'rom_loaded' /tmp/mkrel-boot.log
grep -q 'ppu_frames=60' /tmp/mkrel-boot.log
echo "    booted the ROM and rendered 60 frames"

# Belt and braces: the scripts are supposed to exclude these, so fail loudly if a
# regression ever put them back.
echo "==> assert no cartridge data in the artifact"
if find release-stage -type f \
     \( -name '*.gba' -o -name '*.agb' -o -name 'gba_bios.bin' \) | grep .; then
    echo "the package contains cartridge or BIOS data — refusing to upload" >&2
    exit 1
fi
echo "    artifact carries no ROM or BIOS"

ASSETS="$(find release-stage -maxdepth 1 -name '*.tar.gz')"
[ -n "$ASSETS" ] || { echo "no tarball produced" >&2; exit 1; }
echo "==> artifacts"
printf '    %s\n' $ASSETS

if [ "${SKIP_UPLOAD:-0}" = "1" ]; then
    echo "(SKIP_UPLOAD=1: stopping before touching GitHub)"
    exit 0
fi

command -v gh >/dev/null 2>&1 || { echo "gh CLI not found" >&2; exit 1; }

NOTES="$(mktemp)"
cat > "$NOTES" <<'NOTES_BODY'
**You need your own legally-dumped ROM and GBA BIOS.** The launcher prompts for
both on first run; neither is included in these archives, and neither is uploaded
anywhere — these builds are made locally and only the binary is published.

Each archive is self-contained: the executable, its bundled libraries and the
launcher assets. Extract it and run the executable from inside the directory.

On a Steam Deck, prefer building the Flatpak — SteamOS has an immutable root, so a
package that brings its own runtime fits better than a tarball. See
`docs/PACKAGING.md`.
NOTES_BODY
printf '\nBuilt locally from `%s` on %s.\n' \
    "$(git rev-parse --short HEAD)" "$(uname -sm)" >> "$NOTES"

# Draft: the binary embeds translated cartridge code, so publishing is a decision.
# Re-running for the same tag adds to the existing draft rather than failing, so a
# second platform can be appended from another machine.
if gh release view "$TAG" --repo "$REPO" >/dev/null 2>&1; then
    echo "==> release $TAG exists; uploading (clobbering same-named assets)"
    gh release upload "$TAG" --repo "$REPO" --clobber $ASSETS
else
    echo "==> creating draft release $TAG"
    gh release create "$TAG" --repo "$REPO" --draft \
        --title "$NAME $TAG" --notes-file "$NOTES" $ASSETS
fi
rm -f "$NOTES"

echo
echo "Draft release $TAG updated: https://github.com/$REPO/releases"
echo "Review it and publish manually. Run this on another machine with the same"
echo "tag to append that platform's build."
