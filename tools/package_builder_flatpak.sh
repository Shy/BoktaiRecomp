#!/usr/bin/env bash
# Build the ROM-free BoktaiBuilder Flatpak. The application compiles the game
# from the player's own dump on first launch, so its runtime is the full SDK.
set -euo pipefail

GENERATE_ONLY=0
case "${1:-}" in
    --generate-only) GENERATE_ONLY=1; shift ;;
    "") ;;
    *) echo "usage: $0 [--generate-only]" >&2; exit 2 ;;
esac
[ "$#" -eq 0 ] || { echo "usage: $0 [--generate-only]" >&2; exit 2; }

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
APP_ID="${FLATPAK_ID:-tech.recomp.BoktaiBuilder}"
NAME="${NAME:-BoktaiBuilder}"
SUMMARY="Boktai builder (no game data included)"
OUT="flatpak-build"
OUT_ABS="$ROOT/$OUT"
TEMPLATE="gbarecomp/packaging/flatpak/manifest.builder.template.yml"
MANIFEST="$OUT/$APP_ID.yml"

[ -f "$TEMPLATE" ] || { echo "missing $TEMPLATE" >&2; exit 1; }
mkdir -p "$OUT"

cat > "$OUT/$APP_ID.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=$NAME
Comment=$SUMMARY
Exec=BoktaiBuilder
Icon=$APP_ID
Categories=Game;Emulator;
Terminal=false
EOF

cat > "$OUT/$APP_ID.metainfo.xml" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<component type="desktop-application">
  <id>$APP_ID</id>
  <name>$NAME</name>
  <summary>$SUMMARY</summary>
  <metadata_license>CC0-1.0</metadata_license>
  <project_license>LicenseRef-proprietary</project_license>
  <description><p>Builds Boktai locally from the player's legally dumped ROM and BIOS.</p></description>
  <launchable type="desktop-id">$APP_ID.desktop</launchable>
  <content_rating type="oars-1.1"/>
</component>
EOF

if [ -f packaging/icon.png ]; then
    cp packaging/icon.png "$OUT/$APP_ID.png"
else
    printf '\211PNG\r\n\032\n\0\0\0\rIHDR\0\0\0\1\0\0\0\1\10\6\0\0\0\37\25\304\211\0\0\0\nIDATx\234c\0\1\0\0\5\0\1\r\n\055\264\0\0\0\0IEND\256B`\202' > "$OUT/$APP_ID.png"
fi

SKIP_FILE="$(mktemp)"
SENSITIVE_FILE="$(mktemp)"
trap 'rm -f "$SKIP_FILE" "$SENSITIVE_FILE"' EXIT

# Basename and recursive globs protect future files; exact paths gathered below
# protect unusual nesting and make the generated manifest auditable in CI.
cat > "$SKIP_FILE" <<'EOF'
.git
.git/**
.flatpak-builder
.flatpak-builder/**
flatpak-build
flatpak-build/**
build
build-*
**/build
**/build-*
release-stage
release-stage/**
recomp_cache
**/recomp_cache
roms
**/roms
*.gba
*.GBA
*.agb
*.AGB
**/*.gba
**/*.GBA
**/*.agb
**/*.AGB
gba_bios.bin
**/gba_bios.bin
generated
**/generated
recompiled_*.c
recompiled_*.cpp
bios_recompiled.c
bios_recompiled.cpp
bios_recompiled.h
bios_symbol_map.cpp
**/recompiled_*.c
**/recompiled_*.cpp
**/bios_recompiled.c
**/bios_recompiled.cpp
**/bios_recompiled.h
**/bios_symbol_map.cpp
EOF

find . -mindepth 1 \
    \( -path './.git' -o -path './flatpak-build' \) -prune -o \
    \( -iname '*.gba' -o -iname '*.agb' -o -name 'gba_bios.bin' \
       -o -name roms -o -name generated \
       -o -name 'recompiled_*.c' -o -name 'recompiled_*.cpp' \
       -o -name bios_recompiled.c -o -name bios_recompiled.cpp \
       -o -name bios_recompiled.h -o -name bios_symbol_map.cpp \) \
    -print | while IFS= read -r path; do printf '%s\n' "${path#./}"; done \
    > "$SENSITIVE_FILE"
cat "$SENSITIVE_FILE" >> "$SKIP_FILE"
LC_ALL=C sort -u "$SKIP_FILE" -o "$SKIP_FILE"

SKIP_YAML="$(mktemp)"
trap 'rm -f "$SKIP_FILE" "$SENSITIVE_FILE" "$SKIP_YAML"' EXIT
while IFS= read -r path; do
    path=${path//\'/\'\'}
    printf "          - '%s'\n" "$path" >> "$SKIP_YAML"
done < "$SKIP_FILE"

sed -e "s|@APP_ID@|$APP_ID|g" \
    -e "s|@APP_NAME@|$NAME|g" \
    -e 's|@TARGET@|BoktaiBuilder|g' \
    -e "s|@SUMMARY@|$SUMMARY|g" \
    -e "s|@SOURCE_DIR@|$ROOT|g" \
    -e "s|@SUPPORT_DIR@|$OUT_ABS|g" "$TEMPLATE" |
awk -v skipfile="$SKIP_YAML" '
    /^@SKIP@$/ {
        print "        skip:"
        while ((getline line < skipfile) > 0) print line
        close(skipfile)
        next
    }
    { print }
' > "$MANIFEST"

# Every sensitive object currently in the checkout must have an exact exclusion,
# in addition to the wildcard rules. This also exercises ignored/untracked decoys.
while IFS= read -r path; do
    if ! LC_ALL=C grep -Fx -- "$path" "$SKIP_FILE" >/dev/null; then
        echo "source payload would contain protected path: $path" >&2
        exit 1
    fi
done < "$SENSITIVE_FILE"
echo "generated $MANIFEST (clean source payload validated)"

if [ "$GENERATE_ONLY" -eq 1 ]; then
    echo "generated desktop, metainfo and icon; --generate-only stopped before build"
    exit 0
fi

if command -v flatpak-builder >/dev/null 2>&1; then
    BUILDER=(flatpak-builder)
elif command -v flatpak >/dev/null 2>&1 && flatpak info org.flatpak.Builder >/dev/null 2>&1; then
    BUILDER=(flatpak run --filesystem=host --share=network org.flatpak.Builder)
else
    echo "flatpak-builder or org.flatpak.Builder is required" >&2
    exit 1
fi

BUILD_DIR="$OUT/build"
REPO="$OUT/repo"
rm -rf "$BUILD_DIR" "$REPO"
mkdir -p "$BUILD_DIR" "$REPO"
"${BUILDER[@]}" --force-clean --repo="$REPO" "$BUILD_DIR" "$MANIFEST"

BAD="$(find "$BUILD_DIR/files" \
    \( -iname '*.gba' -o -iname '*.agb' -o -name gba_bios.bin \
       -o -name roms -o -name generated \
       -o -name 'recompiled_*.c' -o -name 'recompiled_*.cpp' \
       -o -name bios_recompiled.c -o -name bios_recompiled.cpp \
       -o -name bios_recompiled.h -o -name bios_symbol_map.cpp \) \
    -print -quit)"
[ -z "$BAD" ] || { echo "protected data reached Flatpak payload: $BAD" >&2; exit 1; }
[ -x "$BUILD_DIR/files/bin/BoktaiBuilder" ] || { echo "BoktaiBuilder was not installed" >&2; exit 1; }
[ -x "$BUILD_DIR/files/bin/sources/bin/gba_recompile" ] || { echo "gba_recompile was not installed" >&2; exit 1; }

flatpak build-bundle "$REPO" "$OUT/BoktaiBuilder.flatpak" "$APP_ID"
echo "built $OUT/BoktaiBuilder.flatpak"
