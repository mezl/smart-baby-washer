# CN2 protocol

**9600 baud, 8N1, 5 V logic, LSB-first, no parity, one stop bit, both lines idle
HIGH, XOR checksum.**

Verified on a Momcozy D8 (`BW05`) with an ESP32-C3 as a transparent
man-in-the-middle, across full wash, self-clean and storage cycles.

**The panel is the command authority; the controller executes.** The panel runs its
whole UI locally and transmits a bitmap of which loads to energise plus a fill
target. The controller drives them and reports its own state back. It gates
**nothing except the intake motor**. Invert that assumption or none of this reads
correctly.

Not fully decoded: controller byte 5, panel byte 2's top bit, status bit 0, panel
b6/b7, and how the panel starts a cycle. None of that blocks normal use — the
ESP32 runs cycles itself rather than asking the panel to.

## Every bit, at a glance

```
 PANEL -> CONTROLLER    AA  b1  b2  b3  xor
                            |   |   `- FILL TARGET
                            |   `----- determined by b3, no independent info
                            LOAD CONTROL BITMAP

 CONTROLLER -> PANEL    A2  T   F   S   04  ??  02  xor
                            |   |   |       `- unknown, 5..14 — NOT a temperature
                            |   |   `- STATUS BITFIELD
                            |   `----- FLOW-METER PULSE COUNT
                            `--------- TEMPERATURE (uncalibrated)
```

| Bit | Panel byte 1 — **energise** | Controller byte 3 — **state** |
|---|---|---|
| 0 | ⚠️ **wash pump** 24 V — unconfirmed | ⚠️ **flow-related, unresolved** |
| 1 | ✅ **drain** | ✅ **lid reed/magnet** |
| 2 | ✅ **water heater** 110 V | ✅ **`E3` Sensor Open Circuit** |
| 3 | ✅ **air heater** 110 V | ✅ **`E4` Sensor Short Circuit** |
| 4 | ✅ **dry — blower + heater** | ✅ **`E0` Voltage Anomaly** |
| 5 | ✅ **intake motor** (gated) | ✅ **`E7` Fan Failure** (mid-cycle only) |
| 6 | ❌ never seen set | ✅ **`E5` Communication Failure** |
| 7 | ❌ never seen set | ✅ **lid micro switch** |

⚠️ **Almost nothing is gated.** `b1`–`b4` drive their loads directly — heaters fire
with no cycle, no water, lid open. `b5` needs a non-zero fill target in byte 3 and
does nothing without one; that is the only gate we can demonstrate. See
[safety.md](safety.md).

## Physical layer

| Property | Value | How |
|---|---|---|
| Baud | **9600** | narrowest pulse 103.68 µs → 9645 est, +0.5 % |
| Format | **8N1**, LSB-first | 10 bit-times per frame |
| Logic level | **5 V** (measured 5.13 V high) | ⇒ a 3.3 V MCU **needs** level shifting |
| Idle | **HIGH**, both lines | ordinary UART |
| Checksum | **XOR** of all preceding bytes | below |

### The checksum is XOR, not sum

The controller frame proves it; the panel's idle frame cannot, because every
payload byte is `00` and it satisfies both rules.

```
A2 ^ 18 ^ 00 ^ 42 ^ 04 ^ 0E ^ 02  =  F0    ✔ matches
A2 + 18 + 00 + 42 + 04 + 0E + 02  =  0x16  ✘
```

**If your capture contains only all-zero payloads you cannot tell sum from XOR.**

## Pin map

| CN2 pin | Frame | Len | Header | Direction |
|---|---|---|---|---|
| **1** | `A2 23 00 02 04 0D 02 88` | 8 | `0xA2` | **controller → panel** ✅ measured |
| **2** | `AA 00 00 00 AA` | 5 | `0xAA` | **panel → controller** ✅ measured |
| 3 | — | — | — | `GND` |
| 4 | — | — | — | `+5V` |

Measured, not inferred: overrides on the pin-1 stream change what the *panel
displays*; overrides on pin 2 change what the *controller does*.

**Say `controller→panel` / `panel→controller`, never "RX"/"TX"** — the controller's
TX is the panel's RX is the ESP32's RX.

## Timing — independent broadcasts, NOT request/response

| | Period (median) |
|---|---|
| controller → panel | **200.00 ms** |
| panel → controller | **201.00 ms** |

Each side transmits on its own clock and neither waits for the other. The 1 ms
difference slides the phase continuously — across one 25 s capture the panel's
offset drifted **0 → 99 ms** smoothly. Any "reply latency" is just phase: it read
28, 32, 41 and 49 ms on successive attempts, and each time looked like a response
delay.

**To impersonate the panel, just transmit every ~200 ms.** There is no poll to answer.

## Framing

Delimited by **header + length**, never by idle gap: `0xA2` → 8 bytes, `0xAA` → 5.

Gap delimiting fails under preemption — timestamps come from when the task *reads*
a byte, and task gaps of 7–8 ms against a ~3.6 ms threshold chopped frames into
phantom changes.

### ⚠️ A valid checksum is not proof of alignment

The idle frame `AA 00 00 00 AA` is self-similar. Start one byte late:

```
AA 00 00 00 AA | AA 00 00 00 AA        the real stream
            ^-- start here
   ->  [AA, AA, 00, 00, 00]            XOR = 0x00, so it VALIDATES
```

It re-locks identically every frame. **Observed live: 386 consecutive
checksum-valid frames reporting byte 1 = `0xAA`** — drain + air heater + intake on
an idle machine — with `bad_p` at zero throughout.

Fix: **gap-based resync**, not gap-based delimiting. Frames are ~200 ms apart and
five bytes take ~5 ms, so a >20 ms inter-byte gap is unambiguously a boundary and
resets the assembler. Length still decides where a frame *ends*.

---

# Controller → panel — 8 bytes, header `0xA2`

```
  A2  23  00  02  04  0D  02  88
  |   |   |   |   |   |   |   `- XOR of bytes 0..6
  |   |   |   |   |   |   `----- constant 0x02, unknown
  |   |   |   |   |   `--------- unknown, 5..14 — NOT a temperature
  |   |   |   |   `------------- constant 0x04, unknown
  |   |   |   `----------------- STATUS BITFIELD
  |   |   `--------------------- FLOW-METER PULSE COUNT
  |   `------------------------- TEMPERATURE, rises with heating
  `----------------------------- header
```

## Status byte — byte 3

Seven bits identified, **bit 0 is not**. Bits 2–6 are a **fault register**; they
read zero across the archive because nothing has ever failed on this machine, and
were mapped by injecting each and reading the panel.

| Bit | Mask | Meaning |
|---|---|---|
| 0 | `0x01` | ⚠️ **UNRESOLVED, flow-related** — [below](#bit-0--flow-related-unresolved) |
| 1 | `0x02` | **lid reed/magnet** |
| 2 | `0x04` | **`E3` Sensor Open Circuit** |
| 3 | `0x08` | **`E4` Sensor Short Circuit** |
| 4 | `0x10` | **`E0` Voltage Anomaly** |
| 5 | `0x20` | **`E7` Fan Failure** — only mid-cycle |
| 6 | `0x40` | **`E5` Communication Failure** |
| 7 | `0x80` | **lid micro switch** |

Observed values: `0x00` `0x01` `0x02` `0x03` `0x42` `0x04` `0x0B` `0x80` `0x85`.

**A bitmask, not a code field.** Sending `0x0C` (bits 2+3) displayed `E3`, not a
third code — independent flags, and the panel shows the **lowest set bit**.

### ⚠️ Fault bits latch

One frame is enough, and it holds until the machine is power-cycled. Clearing the
bit does not clear the display.

**A single corrupted frame with bit 2 set would strand the panel in `E3` until a
power cycle.** The XOR checksum is the only thing between a noisy wire and a
permanent false fault — the strongest argument here for level shifters and for
watching `bad_c`/`bad_p`. It also means the relay must never emit a malformed
frame toward the panel.

### The panel does not range-check the temperature

Forcing byte 1 to `0x00` or `0xFF` does **not** provoke `E3`/`E4`. The controller
is wired to the thermistor, does the range check itself and reports a verdict in a
bit; the panel only ever sees a number. That is the shape of the whole protocol —
the controller reports *hardware* faults as flags, everything else is the panel's
inference.

### Bit 5 is context-dependent

Set on an **idle** panel it does nothing. Set while a **cycle runs**, the same bit
displays `E7`. It fired while the fan was *not* running (the panel was commanding
`drain+intake`), so the gate is "a cycle is active", not "the fan is active".

Worth remembering for the two codes still unaccounted for: a bit can look dead
purely because everything was tested at idle.

### `E0` is reachable, and it points at byte 5

`E0` is bit 4, raised by the controller — so **the controller measures mains
voltage**, which gives byte 5 its first real candidate. Byte 5 sits at 10–11, sags
to 9 under sustained blower load, reads highest when nothing runs, has no
correlation with temperature (r = −0.065), and during a power-down went to `0x00`
for the only time on record:

```
A2 15 00 82 04 00 02 33     checksum valid, byte 5 = 0x00
```

At ~10 V per count, 11–12 is 110–120 V and 9 is a sag. Still a hypothesis: `0x05`
held for 1212 s during an idle period does not fit, and per-load means are not
cleanly monotonic.

### `E1` and `E6` have no bit

**`E1` is not raised by a stalled flow count.** Given zero flow for **330 s while
the intake motor was commanded** — 8× a normal 40 s fill — the panel produced no
error, no alert, no timeout. It simply waited.

⚠️ **Neither end has a fill timeout.** If the flow meter fails while water still
flows, the count never reaches target and the intake motor runs indefinitely into
a machine that keeps filling. See [safety.md](safety.md).

The manual's **Water Shortage** alert is *"insufficient water in the tank"* — the
removable reservoir, not the sump — and recovers by refilling and pressing
Start/Pause, unlike the latching hard faults. The board has one unexplained input
that would suit it: `SW1`, a black 3-pin connector on a machine with a removable
tank. Unproven, but the only candidate.

## Bit 0 — flow-related, UNRESOLVED

Two experiments disagree and neither reading survives both. **Do not rely on this
bit, and do not use it as a dry-run alarm.** A flow counter frozen for 5.7 s under
commanded intake did *not* set it, but a fill released short of its target blipped
it for a single frame. The archive's 5 occurrences are all release-edge transients.

Treat it as unmapped.

## The lid has TWO switches — bits 1 and 7

Bit 1 is a reed/magnet sensor, bit 7 a micro switch, and the machine treats the lid
as closed only when **both** are triggered — one detects the lid present, the other
that it is latched.

They change at **different moments** as the lid is handled, which is what two
switches with different actuation points look like; a single sensor reported twice
would move together.

⚠️ **`b7`'s polarity is not settled.** `b1` is: the lid was removed and refitted and
the bit round-tripped `0x00 → 0x02 → 0x00`, so **b1 set = lid off**. For `b7` the
machine washed 2209 s in `b1=0,b7=0` and 3354 s in `b1=0,b7=1` — either reading
fits. To settle it, open the lid slowly and watch both bits live on `/dev`.

> ⚠️ Bit 1 was first read as "idle/ready" because the machine sat open for a whole
> session, so `0x02` appeared in every idle capture — a confounded variable, and
> `0x42` looked like a compound error code when it was only *lid off* plus *E5*.
> **Vary one condition at a time and round-trip it.** A bit that returns to its
> exact previous value is real; one that only ever changes in one direction may be
> a sensor drifting.

## `E5` is symmetric — and latched

The controller sets bit 6 when the panel goes quiet (measured), but unplug the CN2
cable and the panel displays `E5` with no data arriving at all. It is a **symmetric
watchdog**: each side independently flags the other's silence. So:

1. Clearing bit 6 in the forwarded frame hides `E5` only when the link is otherwise
   healthy. It cannot suppress the panel's own detection of a dead link.
2. **`E5` alone does not tell you which end lost the link** — see
   [troubleshooting.md](troubleshooting.md).

The panel **latches** it: once restored, the display keeps showing `E5` until
power-cycled, matching the manual's only advice — *"Power off and restart."*

**Judge the link by byte 3, not by the display.** Byte 3 is live; the display is not.

---

# Panel → controller — 5 bytes, header `0xAA`

```
  AA  20  40  20  EA
  |   |   |   |   `- XOR of bytes 0..3
  |   |   |   `----- FILL TARGET
  |   |   `--------- determined by byte 3
  |   `------------- LOAD CONTROL BITMAP
  `----------------- header
```

Idle is `AA 00 00 00 AA`.

## Byte 1 — the load control bitmap

Each bit commands one load. Setting it in the relayed frame energises that load on
a real machine; clearing it de-energises it. Every identified bit was confirmed by
forcing it and watching the physical load — direct actuation, not inference.

| Bit | Mask | Load | Connector | Gated? |
|---|---|---|---|---|
| 0 | `0x01` | ⚠️ **wash pump** 24 V — never confirmed | `WS PUMP` | ? |
| 1 | `0x02` | **drain** | `PUMP` (AC `PSB-1`) | no |
| 2 | `0x04` | **water heater**, 110 V AC | relay | no |
| 3 | `0x08` | **air heater**, 110 V AC | relay, red connector | no |
| 4 | `0x10` | **dry: blower + heater** | `− FG +` + heater | no |
| 5 | `0x20` | **intake motor** | `DC PUMP` | ⚠️ **yes — needs a fill target** |
| 6 | `0x40` | unknown — never set | — | — |
| 7 | `0x80` | unknown — never set | — | — |

Observed byte 1: `0x00 0x01 0x02 0x04 0x05 0x10 0x12 0x18 0x1A 0x20 0x22 0x24`.

`b4` is not a fan bit — it energises the blower *and* a heater, a dry-mode master.

**The 303.7 Ω sprayer valve is unattributed** and has never been commanded in any
captured cycle, and b6/b7 have never been seen set. The web UI omits them; they
stay reachable via `POST /api/panel_ovr`.

### b0 — assigned to the wash pump, never confirmed ⚠️

The heater is **not** interlocked behind the pump — all three combinations occur
across a full cycle.

b0 is assigned to the 24 V wash pump **by elimination and timing**: never commanded
before a fill (0 of 4,687 frames), never co-occurring with anything but the water
heater, and no other load fits.

⚠️ **The one confirming attempt failed.** Commanding b0 with a frame byte-identical
to the machine's own wash-phase frame — `AA 01 00 00 AB`, lid closed — does **not**
close the control board's low-side switch on the `WS PUMP` header. Board fault,
undiscovered gate, or b0 not being the pump: unresolved. The pump is now driven by
an external relay; see
[hardware.md](build.md#if-a-load-will-not-switch).

> ⚠️ A partial log of a 70-minute cycle supports almost any tidy story. Treat every
> "never observed" claim here as provisional until a full cycle has been seen end
> to end.

### The controller has no opinion about water

Held with `b5` set the intake motor runs **indefinitely** — no timeout, no
flow-based cutout, no error; 700 frames during a failed fill showed nothing
unusual. A full cycle commanded it 397 times and the counter advanced every time.

**`E1` is not a bit in the UART** — the panel raises it, from controller byte 2.
That is why a flow spoof has to be aimed at the panel, not the controller.

## Byte 3 — the fill target

| byte 3 | counts delivered | occurrences |
|---|---|---|
| `0x07` (7) | 20 | 1 |
| `0x1C` (28) | 80, 80 | 2 |
| `0x20` (32) | 90, 90, 90, 90 | 4 |
| `0x23` (35) | 100, 100 | 2 |
| `0xFF` (255) | 131, 121, 164 | 3 — sentinel, below |

```
byte3 = round(target_count * 0.35)
  20 -> 7.0 -> 0x07 ✅    80 -> 28.0 -> 0x1C ✅
  90 -> 31.5 -> 0x20 ✅   100 -> 35.0 -> 0x23 ✅
```

**Twelve fills, three cycles, four distinct targets, no exceptions**, and the count
follows the word across two different cycle programs — a commanded setpoint, not a
per-step constant. The fill is closed-loop: the panel says "35 units", the
controller counts flow pulses to 100 and releases the intake motor 1–2 s later.

**The controller enforces it itself.** With the panel fully overridden out of the
loop and the ESP32 holding `b5` with target `0x20`, the controller counted to 90
and **zeroed the counter by itself**. The counter reset is the controller
announcing the fill is finished, which makes it a far better completion signal than
arithmetic — inverting the formula lands ±1.4 counts, and `0x20` predicts 91 where
the machine delivers 90.

### `0xFF` is a sentinel — the cool-down rinse

The three `0xFF` fills delivered 131, 121 and 164 counts, no target honoured. It is
the **only** value appearing with `pb1 = 0x22` (drain + intake); every real target
appears with `pb1 = 0x20`.

It is **not** "empty the tank": `pb1` holds `0x22` continuously, never pulsed, and
the flow counter climbs steadily at ~2.2 counts/s, so fresh water is pumped *in*
the whole time the drain is open. It is **cooling the machine before the dryer**:

```
washcycle    94 -> 69 -> 45 -> 36 -> 32 -> 31 -> 30 C   in 62 s
selfclean2   55 -> 54 -> 44 -> 35 -> 31 C
selfclean    56 -> 55 -> 49 -> 44 -> 43 -> 42 C
```

94 → 30 °C in a minute is cold mains water flooding a hot sump; a drain alone would
coast down. The sequence is identical every cycle: `0xFF` rinse → drain `0x00` →
dry. `0xFF` means *no volume target* because there cannot be one — water leaves as
fast as it arrives.

**What ends it is unproven.** All three end just after the temperature bottoms out
and starts rising, consistent with a sump running dry, but at different points:

| | minimum | at end |
|---|---|---|
| washcycle | 30 °C | 34 °C |
| selfclean2 | 31 °C | 37 °C |
| selfclean | 42 °C | 43 °C |

Temperature-linked rather than volumetric is as far as the evidence goes.

## Byte 2 — carries no independent information ⚠️

Values `0x40`, `0xAB`, `0x2A`, `0xD6`, `0xFF`. Not a temperature (`0x40` preceded
stage peaks of 26, 24, 76, 74 and 58 °C), not a duration. It is **completely
determined by byte 3** — six distinct values each side, six distinct pairs,
strictly 1:1 across every captured frame.

Three of the four real values fit the same law one unit finer:

```
byte2 = floor(counts * 15/7)          byte3 = round(counts * 7/20)
  20 -> 42.9  -> 0x2A ✅                20 -> 0x07 ✅
  80 -> 171.4 -> 0xAB ✅                80 -> 0x1C ✅
 100 -> 214.3 -> 0xD6 ✅               100 -> 0x23 ✅
  90 -> 192.9 -> 0xC0 ❌               90 -> 0x20 ✅   observed 0x40
```

The mismatch differs by **exactly `0x80`** — bit 7 and nothing else. So byte 2 looks
like the same target at ~2.86× resolution with something riding in its top bit. One
outlier in four is a lead, not a conclusion: **a fifth distinct fill target settles
it**, and no captured cycle has produced one.

Practically: byte 2 is redundant. Read only byte 3.

## Cycle start sequence (steam, captured end to end)

```
   0.2  PANEL  AA 00 00 00 AA     idle
  36.0  PANEL  AA 02 00 00 A8     b1 DRAIN, no fill target yet
  56.8  PANEL  AA 20 40 20 EA     b5 intake + target 0x20 = 90 counts
  60.5  PANEL  AA 00 40 20 CA     loads clear
```

The drain step ~20 s ahead appears identically before the drying run — the machine
**clears the sump before it fills**, so every cycle starts with a drain even when dry.
