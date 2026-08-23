# Second D8 unit — session log, 2026-08-22

A day spent on an `E5` that turned out not to be ours, on a machine that is not
the one every assumption in this repo was built from. Written down because most
of it is a record of being wrong, and the wrong turns are the useful part.

## The machine

A later Momcozy D8 than the one this project was developed on: **touch panel**,
controller **byte 6 = `0x03`** where the original reads `0x02`. Same CN2 link —
9600 8N1, same frame layout, same load bitmap, same five fill targets. The
firmware ran it unmodified.

## The mistake that shaped the whole day

**`E5` was read as "communication failure" because that is what the BW05 manual
says — and the BW05 manual is the *other* machine's.**

Everything followed from that. A comms label sent me hunting comms causes: boot
blackouts, frame timing, GPIO pins, WiFi stalls, level-shifter channels. Each
was a plausible answer to a question that was probably never being asked. On a
link with **zero bad checksums in either direction all day**, "communication
failure" was the one explanation the evidence already excluded, and I kept
reaching for it anyway.

⚠️ **Do not assume an error-code table transfers between revisions.**

## What was proven about status bit 6

Measured on firmware **1.2.0, a plain relay with no rewriting at all**
(`st_real` identical to `st_fwd`), cold machine at 24 °C, panel idle, nothing
commanded:

```
t = 0–45 s   byte3 = 0x00   222 frames, clean
t = 45 s     byte3 = 0x40   raised, and held
```

The controller asserts it **by itself**. Ruled out, each by test rather than
argument:

| candidate | how it died |
|---|---|
| our firmware | bisected across 1.2.0 / 1.3.0 / 1.4.0 / 1.5.0 / 1.6.0 — all clean on a cold boot; then fully reverted to 1.2.0 and it still appeared |
| our rewriting | plain relay, forwarded byte byte-identical to received |
| temperature | appeared at 24 °C |
| link quality | 0 bad checksums both directions, byte-exact forwarding, all day |
| standing water | 30 s drain changed nothing |
| power-cycle duration | 1 s, 2 s, 8 s, 10 s — identical |
| heater stuck on | machine cools at the same rate powered and unpowered |

**The panel is what acts on it.** Captured at 5 Hz with no filtering:

```
284.4  A2 2C 00 00 …  AA 05 00 00 AF   wash + heat, healthy
295.4  A2 2C 00 40 …  AA 05 00 00 AF   controller raises bit 6
295.6  A2 2C 00 40 …  AA 00 00 00 AA   panel drops every load, 0.2 s later
```

That 0.2 s is the whole mechanism: the controller flags, the panel aborts. So
masking the bit stops the abort — which is a workaround on a fault nobody
understands, not a fix.

## Differences from the original unit

⚠️ **Both boards are the SAME revision.** Photographed and compared:
`BBW04001-UL-P`, `QM-V7 20250805`, `CEM-1 KB-5150 JZ-C E330831` on both — only
the QC stamp differs (2026.1.25 vs 2026.1.1), and the *later* machine's board
was tested three weeks *earlier*. So none of the differences below are hardware
differences. What differs is the panel, and whatever program the MCU runs.

| | early (button panel) | later (touch panel) |
|---|---|---|
| controller byte 6 | `0x02` | `0x03` — **not** a revision marker; see below |
| wash pump on `b0` | low-side switch never closes — needs the external relay | **works** |
| lid bits 1 and 7 | reported normally | **never seen set** in any capture |
| status bit 6 | only when starved of panel frames | also asserts spontaneously and holds |
| drain stage | 20 s | 28 s |
| fill rate | 2.24 counts/s | ~1.65 counts/s |

**`b0` working here means the relay mod is unnecessary on this revision.** Check
before wiring one.

## Things I broke, and what they cost

- **Masked the machine's own fault reporting.** Decided bit 6 was false and
  deleted it. That hid information and left an appliance that looked healthy and
  silently refused to work. Worst call of the day.
- **Masked it with a stale checksum.** Cleared bit 6 from byte 3 without
  recomputing the trailing XOR, because the recompute keyed off an *enumerated*
  list of overrides that the new one had not joined. Every masked frame was
  discarded by the panel — I caused a genuine communication failure while
  claiming to suppress a fake one.
- **A fill-stall guard that latched.** It stripped the intake bit and never gave
  it back, so for several minutes *I* was the thing stopping the machine.
- **Eight firmware flashes**, each starving the panel ~10 s and re-latching E5,
  each costing the owner a power cycle. Six times I called something fixed.
- **Called a transient a bug.** Reported the drain override "evaporating" off one
  sample; it held for 12 s on a careful retest.

## Things worth keeping

- **Checksums are now recomputed on observed change** (`out != b`), not from a
  list of features that goes stale the next time someone adds a rewrite.
- **Frame position is counted, not timed.** A 96 ms CPU stall during WiFi
  association made the relay drain the FIFO late, the byte gap looked like a
  frame boundary, and the index reset *mid-frame* — so byte 3 was never
  recognised and every position-keyed rewrite silently skipped it.
- **Emitted frames are checksum-checked** (`tx_bad`). Nothing was watching that
  direction; `ok_c`/`ok_p` only ever counted what we *receive*, which is why a
  corrupt-output bug survived a flash, a smoke test and my own inspection.
- **`tools/selftest.py`** — reports "not measurable" for loads with no sensor
  instead of claiming a pass.
- **The OTA gap is measured** across the reboot via the RTC clock: ~9.8 s, well
  past the panel's tolerance. An `E5` after any flash is the update, not a fault.

## Still unexplained

- What bit 6 means on this controller, and what makes it fire ~45 s after a cold
  boot with nothing running.
- Why the intake pump ran twice on command (0→36, 0→54 counts) and then stopped
  responding to the byte-identical frame for hours.
- Why the level-shifter pull-up on **`GPIO6`** — the transmit line to the
  controller — read present in the morning and absent by evening, 8/8 both times.
  It should not matter (the UART drives push-pull) but it changed, and it is on
  the one path we cannot observe.

## Controlled heat tests, 2026-08-23

Driven directly from the ESP32 with the panel held idle, bounded and monitored,
mains ready to cut. Every one ran **clean — no lockout**.

| stage | water | heat rate | lockout |
|---|---|---|---|
| wash + heat | 20 counts | **+0.2022 °C/s** | no |
| wash + heat | 80 counts | **+0.0801 °C/s** | no |
| steam (heater, no circulation) | 20 counts | **+0.3466 °C/s** | no |
| **dry (air heat + blower)** | empty | **−0.0501 °C/s** | no |

What these kill and what they leave standing:

- **Heat rate does not determine the lockout.** The 80-count run held 0.0801 °C/s
  and finished clean; cycle 3 locked out at 0.077 °C/s. Same rate, opposite
  outcome, and this run went straight through 44 °C.
- **The water heater is sound at every volume tested**, and the sump probe
  tracks it faithfully. Both of the day's earlier theories — dead element, stuck
  sensor — are dead.
- **Steam is the fastest of all** (0.35 vs 0.20 at the same volume). Without
  circulation the heat concentrates near the probe rather than spreading.
- ⚠️ **Dry can never satisfy a "heater on → temperature must rise" check.**
  With the air heater and blower running, the sump reads a steady *decline*.
  That is physics, not a fault: the blower cools the sump, the air heater warms
  air, and this machine has **only the sump NTC — there is no air probe**. Four
  of the six observed lockouts began during dry.
- Cutting the heater at 72 °C let the sump coast on to **83 °C** — an 11 °C
  overshoot. The element runs far hotter than the water and sheds heat slowly.

⚠️ **The lockout needs a LONG power-down to clear.** Measured directly: 15 s,
30 s and 45 s all came back locked; **60 s cleared it**. Quick flicks do not
work, which is why several earlier "power cycle didn't help" observations were
misleading.

⚠️ **A masked bit 6 does not unlock the controller.** Masking stops the *panel*
aborting; the controller still ignores every load command while locked. The
machine only runs if bit 6 is clear *when the cycle starts*.

## The test never run

**Unplug the module; connect the panel straight to the controller's `CN2`
header.** It is the only measurement that separates *the machine* from *the
thing plugged into the machine*, and no amount of instrumenting from inside the
link can substitute for it.
