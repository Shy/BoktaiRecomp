# Boktai solar sensor — hardware model

Reference notes for implementing `GbaSolarSensor` in `gbarecomp/src/gba/`.

**Sourcing / licensing.** These are *hardware facts* read from mGBA's
`src/gba/cart/gpio.c` (`_lightReadPins`, `_outputPins`, `_rtcReadPins`). mGBA is
MPL-2.0 and gbarecomp's `third_party/README.md` requires the native build to
have **zero copyleft emulator dependencies** ("oracle binary only — never linked
into native"). So: no code is copied. gbarecomp's own `ARCHITECTURE.md`
explicitly permits borrowing "hardware reference behavior from emulators and
hardware docs", which is what this is. The implementation must be written
independently and cite this note.

## The port

`0x080000C4` data · `0x080000C6` direction · `0x080000C8` control.
Four pins, bits 0–3. Direction bit set = the *guest* drives that pin; clear =
the *device* drives it. A device may therefore only assert pins the guest has
configured as inputs.

## Pin assignment

| bit | RTC (S-3511A) | Solar sensor |
|---|---|---|
| 0 | SCK (serial clock) | **ADC clock** — counter increments on rising edge |
| 1 | SIO (serial data) | **reset** — clears the counter and latches a new sample |
| 2 | CS (chip select) | **arbitration** — sensor is inactive while this is high |
| 3 | unused | **comparator output** — driven by the sensor |

### How the two devices coexist

This was the open question, and the answer is **bit 2**. The sensor bails out
immediately when bit 2 is high:

> if the CS pin is asserted, the light sensor returns without touching anything

The RTC's protocol asserts CS while it is being addressed, so whenever the guest
is talking to the clock the sensor is dormant, and whenever CS is low the sensor
is live. They are mutually exclusive on the same wires, arbitrated by one pin.

Implementation consequence: **both device handlers run on every pin change**, and
each decides for itself whether it is being addressed. There is no external
mux — that is what the shared `GpioPort` should do.

## ADC protocol

An integrating ADC with a comparator, not a parallel value:

1. Guest raises **bit 1 (reset)** → counter := 0, and the host light level is
   sampled *once*, latched for this conversion.
2. Guest pulses **bit 0 (clock)**. Counter increments on each rising edge
   (track the previous level to detect the edge; the counter is 12 bits).
3. Sensor drives **bit 3** high once `counter >= sample`.
4. Guest counts how many clocks it took for bit 3 to flip. **That count is the
   reading.**

### The value is inverted — this matters

`sample` is a *threshold*, so a **small sample flips the output sooner**.
mGBA defaults it to `0xFF` when no light source is attached, which is the
"no light at all" case (the comparator effectively never fires).

So: **0x00 = brightest, 0xFF = darkest.** A `SensorProvider` returning raw
brightness must be inverted before it reaches the comparator, or the webcam will
work exactly backwards — bright sun reading as night. Worth an explicit named
conversion rather than a bare `255 - x` somewhere.

## Known uncertainty, carried forward

mGBA's own source flags one thing as unverified: whether the counter reset
happens on **bit 1 being high** (level-triggered) or only on its **rising edge**.
It implements level-triggered. Adopt the same and mirror the caveat in a comment;
if Boktai's readings look quantised or sticky, that is the first thing to vary.

## State to serialize

`counter` (12-bit), `sample` (8-bit), `clock_edge` (previous bit-0 level). All
three are execution-relevant and must go into savestates *and* the cosim state
hash — the RTC currently omits its own state, and that gap should not be
inherited (see `PR-eeprom-width-autodetect.md` for the same class of oversight).

## Determinism

Per the locked project decision: sample once per frame into the deterministic
input stream, with `RECOMP_SOLAR_FIXED=<0-255>` to pin a value for cosim and
tests, mirroring the existing `RECOMP_RTC_EPOCH` precedent.
