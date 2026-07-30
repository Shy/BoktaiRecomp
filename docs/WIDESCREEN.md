# 16:10 extended view (EXPERIMENTAL — not ready)

## Why 256×160

Not an arbitrary number. `256/160` is exactly **16:10**, and:

| View | Integer scale on a 1280×800 Deck | Result |
|---|---|---|
| native 240×160 | ×5 → 1200×800 | 40px bars each side |
| **16:10 256×160** | **×5 → 1280×800** | **pixel-perfect, no bars** |

The Steam Deck (LCD and OLED) is 1280×800. So 256×160 is the one width where the
GBA framebuffer tiles the Deck panel exactly at integer scale — no filtering, no
letterbox, every guest pixel exactly 5×5 device pixels.

The engine already does the right thing here without changes:
`compute_presentation_layout` works in reduced-aspect units, so 256×160 into
1280×800 yields `integer_scale = 5` and a full-bleed destination rect.

16:9 would need 284.4 → 288px (+48 columns). 16:10 needs **+16 columns, 8 per
side** — a third of the exposure, which is the main reason this is worth
attempting at all for a game with no decompilation.

## What is done

`RunOptions::max_view_width = 256` and `widescreen_view_width = 256` in
`src/main.cpp`. That authorizes the PPU to render 8 extra columns on each side.

`launcher_expose_widescreen` is deliberately **false**: the width is reachable
with `--view-width 256` for testing, but not advertised in the launcher, because
authorizing the width does not make the *game* aware of it.

## What is not done, and why it is hard here

Widening the canvas is the easy half. The game still believes the screen is 240
wide, so the margins expose whatever the game never intended to be seen:

| Artifact | Cause |
|---|---|
| Sprites popping in at the edges | Many GBA games "hide" an OBJ by parking it at X ≥ 240. Those become visible in the margin. |
| HUD misplacement | HUD sprites and BG layers are positioned in screen coordinates, so they sit 8px off-centre or leave a gap. |
| Wrong window/blend edges | `WININ`/`WINOUT` and the blend regions are screen-space; Boktai leans on them for its lighting. |
| Stale or garbage BG columns | The game only uploads the tiles the 240px window needs. |

That last one is the deep one, and it is where MinishCap's approach does **not
transfer**.

### How MinishCap solves it, and why we cannot copy it

`MinishCapRecomp/src/minish_extended_view.cpp` is ~666 lines that read the
game's *complete* room layers straight out of EWRAM:

> The two Special buffers are not hardware tilemaps: they are the game's complete
> rendered room layers (128×128 8px tiles). The original game copies only the
> visible 32×32 ring from these buffers to VRAM as the camera moves.

It can do that because it has **pinned data symbols from the
[zeldaret/tmc](https://github.com/zeldaret/tmc) decompilation** — `kMapTop`,
`kMapDataTopSpecial`, `kHud`, `kEntities`, `kPlayerEntity`, camera and room
control blocks, all at known addresses with known layouts.

**Boktai has no decompilation.** `variants/boktai1_usa/symbols/imported_symbols.tsv`
is empty for exactly this reason. Every one of those structures would have to be
reverse-engineered from scratch: the room/tilemap master buffer, the camera, the
HUD element table, the entity array and its stride.

That is the real cost, and it is not a small one.

### The reason 16:10 might still work without any of that

GBA background tilemaps are **256 or 512 pixels wide in hardware**, while the
visible window is 240. For a 256-wide BG map, the tiles covering x = 240…255
*already exist in VRAM* — the hardware wraps at 256, so the game has been
maintaining them all along whether or not it draws them.

So for exactly +16 columns there is a real chance the BG layers fill correctly
with no game-side work at all, which is not true at 288 and is why this is worth
measuring before writing any RE.

Sprites, HUD and window regions would still need handling.

## Measured so far

**The margins are purely additive.** Rendering the same frame at 240 and at 256
and comparing the wide frame's centre 240 columns against the native frame:
`0 / 38400 pixels differ`. So authorizing the wider view does not perturb what
the game itself draws — the extension is strictly extra columns, and the faithful
view is byte-for-byte recoverable by asking for 240.

That is the property that makes this safe to leave in the tree: a player who does
not opt in gets exactly the original image.

The engine reports the split as expected:

```
extended view ON: requested=256x160 effective=256x160 margins=8/8
```

**The margins do NOT fill themselves.** On a logo/title screen with real content
(51 distinct colours) the 8-column margins render as flat near-black while the
adjacent interior is the scene:

| Band | Colour |
|---|---|
| left margin `x=0..7` | `(24,24,24)` |
| left interior `x=8..15` | `(0,148,222)` |
| right interior `x=240..247` | `(0,148,222)` |
| right margin `x=248..255` | `(24,24,24)` |

So the hopeful theory above — that a 256-wide hardware BG map would already have
valid tiles at x = 240…255 — **does not hold for this screen**. The margins fall
back to the backdrop instead of continuing the picture.

The one consolation is that they are *empty*, not *corrupt*: the result reads as a
slightly wider letterbox rather than garbage, so nothing looks broken. But 16:10
is not free, and getting real content into those columns needs game-side work.

**Still unmeasured: gameplay.** A scrolling gameplay tilemap may behave differently
from this screen, which looks like a windowed backdrop. Reaching gameplay headlessly
is currently blocked — see the demo-input crash in [ISSUES.md](../ISSUES.md) — and
no save state exists to jump straight in.

## Status

Experimental and off by default. Nothing is claimed to work yet. If the artifacts
turn out to need the full MinishCap treatment — reverse-engineering Boktai's room
buffer, camera, HUD table and entity array with no decompilation to lean on —
this stays behind `--view-width 256` rather than shipping something that looks
broken.

## Testing it

```bash
./BoktaiRecomp ../variants/boktai1_usa/game.toml --view-width 256 --scale 5
```

On a Deck, `--view-width 256` plus fullscreen is the pixel-perfect configuration.
