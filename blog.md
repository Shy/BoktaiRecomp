# Engineering Journal — BoktaiRecomp

## 2026-07-31: Repair Steam Deck builder packaging

- Changed the builder manifest runtime to `org.freedesktop.Sdk 23.08`; the SDK is
  required at player runtime because first launch runs C++, CMake and Ninja.
- The Flatpak build now compiles the ROM-free `BoktaiBuilder` configuration and
  `gba_recompile`, then installs the builder, filtered source tree and recompiler
  in the layout expected by the builder's source resolver.
- Reworked Flatpak packaging around a generated, dynamically augmented skip list
  and a dedicated OSTree repository. Release automation now publishes only the
  ROM-free builder tarballs and Flatpak to a draft release.
- Verification performed for this change: shell syntax, generate-only against
  locally present ignored ROM/BIOS/generated paths, YAML parsing and structural
  assertions. CI also plants explicit decoys. A full Flatpak build and first
  launch on Steam Deck were not run locally.
