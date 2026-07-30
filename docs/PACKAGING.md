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

## Steam Deck

In **Desktop Mode**:

```bash
flatpak install -y flathub org.flatpak.Builder
flatpak install -y flathub org.freedesktop.Platform//23.08 org.freedesktop.Sdk//23.08

gbarecomp/packaging/flatpak/build_flatpak.sh --target BoktaiRecomp \
    --name Boktai --id tech.recomp.BoktaiRecomp \
    --summary "Boktai: The Sun Is in Your Hand, recompiled"
```

Then `flatpak run tech.recomp.BoktaiRecomp`, and to reach it from Gaming Mode:
Steam → Games → Add a Non-Steam Game.

Flatpak is the right target for SteamOS because its root filesystem is immutable —
the runtime supplies SDL2, so nothing is installed into the OS.

> **Untested.** The manifest generation is verified, but the Flatpak build itself
> has never been run — there was no Linux or Deck host available. Expect to fix
> something on first attempt.

The manifest requests `--share=network` for the weather-driven sensor. Drop that
line if you only ever use the manual hotkeys.

## Windows

Mirror MinishCapRecomp's `tools/package_release.ps1`: configure with
`-DGBARECOMP_STATIC_RELEASE=ON` under MSYS2 MinGW64 so SDL2 and the C++ runtime
link statically, build, then `strip`. That option is MSYS2-specific by design —
it pins `libSDL2.a` from the MinGW prefix.

## What is never packaged

The ROM and the BIOS. A recompiled binary already embeds translated ROM code, so
these scripts build **your own** copy from **your own** dump. The launcher prompts
for both on first run.
