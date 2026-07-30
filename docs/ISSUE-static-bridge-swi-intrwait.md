# Static build stalls on the first SWI taken inside a Stage-1 bridge while the BIOS IntrWait flag is set

Boktai 1 (USA). The recompiled (static) build stops making progress while
`GBARECOMP_FORCE_INTERP=1` runs the same game into gameplay indefinitely.

**This is a liveness bug, not a correctness bug.** State is provably identical up
to the failure point.

## Reproduction

```bash
# static — stalls
GBARECOMP_SAVE_TYPE=sram GBARECOMP_DEMO_INPUT=campaign \
  ./BoktaiRecomp --bios gba_bios.bin --rom boktai1_usa.gba --frames 3000

# interp — fine
GBARECOMP_FORCE_INTERP=1 GBARECOMP_SAVE_TYPE=sram GBARECOMP_DEMO_INPUT=campaign \
  ./BoktaiRecomp --bios gba_bios.bin --rom boktai1_usa.gba --frames 3000
```

Static ends `final_pc=0x00000008` (the BIOS SWI vector) with `vram_nonzero`
frozen at 7,377 and `interpreted_insns` pinned at 365,563 no matter how many
frames are requested. Interp reaches gameplay.

## Localization — SWI-sequence diff

`GBARECOMP_SWI_LOG` on both backends, then diffed field-by-field
(`imm, ret, r0, r1`):

```
static = 2305 records
interp = 11881 records
identical for the first 2305 SWIs; divergence is only in LENGTH
```

**No value ever differs.** Static executes correctly and then simply stops.

The stall correlates perfectly with the logged `iwflags` column, which is
`bus_read_u32(0x03007FF8)` — the BIOS IntrWait flag:

| backend | `iwflags != 0` first seen | records after it |
|---|---|---|
| static | SWI **#2304** | **1** — then dead |
| interp | SWI **#2304** | **9,577** — continues fine |

Both backends reach the same state at the same SWI. It is the **first SWI taken
while the IntrWait flag is set**, and it kills the static build.

Last common record:

```
seq=2304  cycles=78859543  imm=393216 (SWI 6, Div)  ret=0x0300356C
r0=0x0000023C r1=0x0000007F  lr=0x03003748  iwflags=0x00000001
```

`ret`/`lr` are in Boktai's **runtime-decompressed IWRAM region**, so this SWI is
taken from inside a Stage-1 interpreter bridge.

## Why this game hits it

Boktai's crt0 (`0x08EDA900`) DMAs an 840-byte loader from ROM `0x08EDA9CC` to
IWRAM `0x03000000`, and that loader **decompresses the main section to
`0x03003000+` at runtime**. Those bytes exist nowhere in the ROM image, so no
`[[code_copy]]` or `[[extra_func]]` can cover them — per `docs/ARCHITECTURE.md`
this is the "true self-modifying / streamed code" case that must run on the
interpreter tier. Boktai therefore spends essentially all of its time inside
Stage-1 bridges, which makes it a good stress case for the bridge contract.

Relevant detail: `runtime_arm_default_aborts.cpp` chooses
`stop_pc = g_cpu.R[14]` as a fallback for a top-level miss, and its own comment
warns "LR can equal entry_pc, a stop the routine never reaches." With
`GBARECOMP_SELFHEAL_VERBOSE=1` that degenerate case is exactly what Boktai
produces:

```
bridge entered at top level for pc=0x03003024; LR=0x03003024 (fallback stop)
bridge entered at top level for pc=0x03003030; LR=0x03003030 (fallback stop)
```

The loop is supposed to compensate by healing to static, but that check requires
landing on an exact static function *entry*
(`runtime_has_static_entry`), and this game's hot code never does:
`healed_native=0`, `native_calls=0` — the static build executes **none** of its
5,114 generated functions once it enters the bridge.

## Ruled out

- **Bridge iteration cap** — `kBridgeIterationCap` is 200,000,000 and only
  365,563 instructions are interpreted; the abort path never fires, and the run
  exits 0.
- **SWI routing differences** — the SWI handling in `runtime_bridge_interpret`
  and in `runtime_force_interp_step` is the same code shape (set `R[15]` to the
  return address, then `runtime_swi`).
- **Interior resume points** — adding `resume = true` entries for the 7 interior
  misses inside the copied loader took misses 23 → 16 and was accepted
  (`midfn_aliases: 7 entries -> 4 hosts`), but did not change the stall.
- **State corruption** — 2,305 SWIs match exactly.

## Suspected area

The interaction of **Stage-1 bridge × SWI × IRQ/IntrWait nesting**. The file
already models an "IRQ-continuation mode" (`irq_cont`) for bridging a miss while
an IRQ handler is in progress, and notes that the normal stop contract is invalid
there. The failure is the first time this game combines a bridged SWI with a set
IntrWait flag, so the handoff on that path looks like the place to start —
specifically whether the post-`runtime_swi` resume PC and the bridge's stop
contract stay consistent when the SWI is taken with an interrupt pending.

I have not attempted a fix: getting IRQ-nesting semantics wrong here would be
worse than the current honest stall, and the diagnosis above should make it cheap
for someone who knows this code.

## Workaround

`GBARECOMP_FORCE_INTERP=1` plays the game fully. Slower, and coverage is
`NOT_STATIC` by definition, but correct.
