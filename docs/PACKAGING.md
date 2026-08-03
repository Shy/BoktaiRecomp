# Packaging

The scripts live in the `gbarecomp` submodule (`gbarecomp/packaging/`) because
every game in the ecosystem needs the same thing; see
[`gbarecomp/packaging/README.md`](../gbarecomp/packaging/README.md).

Recompile and build first — packaging needs `generated/`, which is not committed.

## macOS

```bash
gbarecomp/packaging/package_macos.sh --target BoktaiRecomp --name Boktai \
    -DGBAGAME_RECOMP_UI=ON
```

Produces `release-stage/Boktai-macos-<arch>/` and a `.tar.gz`: the executable,
every non-system dylib beside it, the launcher assets, and an ad-hoc signature.
Verified booting the real ROM from a freshly extracted tarball.

If you see **"failed to load SDL3"**, your Homebrew `sdl2` is `sdl2-compat`, whose
`libSDL2` is a shim that dlopens SDL3. `otool -L` cannot see that dependency, so
a naive packager misses it; the script scans for the `@loader_path/libSDL3.dylib`
probe and copies SDL3 in. If it warns that it could not find SDL3, install it
(`brew install sdl3`) and re-run.

It produces a plain directory rather than a `.app` on purpose — a bundle is
SIGKILLed at launch because the launcher needs `assets/` inside `Contents/MacOS`,
where `codesign` treats it as unsigned code. The reasoning is in the
`gbarecomp/packaging` README.

## Steam Deck builder

Players download `BoktaiBuilder.flatpak`; they do not clone this repository or
install `flatpak-builder`. In **Desktop Mode**:

```bash
flatpak install --user ~/Downloads/BoktaiBuilder.flatpak
flatpak run tech.recomp.BoktaiBuilder
```

On first launch, choose a legally dumped Boktai ROM and GBA BIOS. The builder
copies its bundled clean source tree to writable per-user storage, runs the
bundled `gba_recompile`, and compiles the game using `org.freedesktop.Sdk 23.08`.
The shared SDK runtime supplies C++, CMake and Ninja without changing SteamOS's
immutable root. Add “BoktaiBuilder” through Steam → Games → Add a Non-Steam Game
to launch it in Gaming Mode.

Maintainers build the distributable bundle on Linux with:

```bash
tools/package_builder_flatpak.sh
```

This writes `flatpak-build/BoktaiBuilder.flatpak`. Use `--generate-only` to
regenerate and validate the manifest/support files without running Flatpak.
Manifest generation is verified in CI; a complete first-launch build on real
Steam Deck hardware remains unverified.

The manifest requests `--share=network` for the weather-driven sensor. Drop that
line if you only ever use the manual hotkeys.

## Windows

Mirror MinishCapRecomp's `tools/package_release.ps1`: configure with
`-DGBARECOMP_STATIC_RELEASE=ON` under MSYS2 MinGW64 so SDL2 and the C++ runtime
link statically, build, then `strip`. That option is MSYS2-specific by design —
it pins `libSDL2.a` from the MinGW prefix.

## Automated builds

`.github/workflows/release.yml` builds only the ROM-free builder: macOS and Linux
tarballs plus `BoktaiBuilder.flatpak`. It needs no ROM/BIOS secrets and opens a
**draft** GitHub Release after both builder jobs succeed.

Triggered by pushing a `v*` tag, or manually via *Actions → Release → Run
workflow*.

The packaging scripts exclude ROMs, BIOS files, generated translations, VCS data
and build output. The Flatpak build exports to a dedicated repository before
creating the single-file bundle. The Release remains a draft so its assets and
instructions can be reviewed before publication.

Windows is not in the matrix yet: its packaging path is MSYS2-specific
(`GBARECOMP_STATIC_RELEASE` pins `libSDL2.a` from the MinGW prefix), so it needs a
runner set up for that rather than the stock image.

## What is never packaged

The ROM and the BIOS. A recompiled binary already embeds translated ROM code, so
these scripts build **your own** copy from **your own** dump. The launcher prompts
for both on first run.
