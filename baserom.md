# Base ROM (and BIOS)

Neither the ROM nor the BIOS ships here. You provide your own dumps of both, and
the runner refuses to launch unless **both** verify — the BIOS is not optional,
because every gbarecomp game boots through the real recompiled BIOS
(`gbarecomp/PRINCIPLES.md`, "BIOS is sacred").

## 1. BIOS

See [`gbarecomp/bios/README.md`](gbarecomp/bios/README.md). Expected at
`gbarecomp/bios/gba_bios.bin`.

| SHA-1 |
|---|
| `300c20df6731a33952ded8c436f7f186d25d3492` |

## 2. Cartridge ROM

Your own dump of Boktai: The Sun Is in Your Hand (USA), at
`variants/boktai1_usa/roms/boktai1_usa.gba`.

| Region | SHA-1 | CRC32 | Size |
|---|---|---|---|
| USA | `7164326283df46a3941ec7b6ceca889cbc40e660` | `0xE715AC45` | 16 MiB |

Verify before building:

```bash
./build/verify_rom_hash variants/boktai1_usa/roms/boktai1_usa.gba
```

### Use a clean dump — this matters more for Boktai than for most games

Two kinds of patched Boktai ROM circulate widely, and both break this project:

- **Sensor patches** replace the cartridge photodiode reads with a constant, so
  the game no longer talks to the hardware this project emulates. The solar
  sensor becomes inert — the whole point of the port, gone.
- **Intro patches** (cracktros) change the header/logo region. One of these cost
  real debugging time here: an oracle screenshot showed a scene group's
  "GREETINGS '10" splash on a 2004 game, which is how the dump was identified as
  patched at all.

Either will also fail the hash gate above, which is the intended behaviour.

## Pointing the build at them

Paths live in `variants/boktai1_usa/game.toml` (`[rom].path`, `[bios].path`),
resolved relative to that file. `--rom` / `--bios` override them, and the
launcher's pickers write a cached sidecar so a first run is not blank.
