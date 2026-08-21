# Safety

Read this before opening the machine.

This is a **120 V, 560 W appliance that heats water to near boiling**. The
low-voltage board you will be probing sits a few centimetres from live mains, and
the machine is designed to run unattended with a lid closed.

## Non-negotiables

**Unplug it before opening it.** Not "switched off" — unplugged. There is a
switching supply inside with charged bulk capacitors; give it a minute.

**Verify mains isolation before attaching an earthed scope ground.** Unplugged,
measure resistance from CN2 pin 3 (GND) to each mains pin on the cord. You want
**megohms**. If either reads low, stop — you have a non-isolated board, and an
earthed scope ground would be a short across the mains. Expect a small leakage
current through the primary-to-secondary Y-capacitor when you attach the clip;
that is normal, a dead short is not.

**Never put a scope ground clip on a supply positive.** Both channel grounds are
common and tied to earth.

**Set the bench up so you are not reaching over anything live** while the machine
is powered and open. You will be running cycles with the case off, and there will
be hot water.

## ⚠️ Panel-spoofing is not safe

It is tempting to assume the controller holds the safety decisions and will veto
anything dangerous. **It does not.** The panel is the command authority; byte 1 of
its frame is a direct bitmap of which loads to energise:

| Load | Bit | Gated by the controller? |
|---|---|---|
| wash pump 24 V | `b0` | **no** — and see the note below |
| drain | `b1` | **no** |
| water heater 110 V | `b2` | **no** |
| air heater 110 V | `b3` | **no** |
| dry: blower + heater | `b4` | **no** |
| intake motor | `b5` | ✅ **yes** — needs a fill target in byte 3 |

**Only one thing is gated.** `b5` needs a non-zero fill target and does nothing
without one — confirmed by forcing the bit alone (no pump, ever) then with `0x07`
(pump runs, counter advances). That is a real interlock but a thin one: it stops
an accidental single-bit write from pumping water, and nothing more. Two bytes
still start a fill.

Setting `b2`, `b3` or `b4` energises a heater whether or not a cycle is running,
whether or not there is water, and whether or not the lid is on. Measured, not
theorised.

⚠️ **`b0` is a special case.** Commanding it does not close the control board's
low-side switch on the `WS PUMP` header, so on this machine the pump is driven by
an **external relay** that follows b0 instead. Anything setting b0 now closes a
relay directly — see [hardware.md](build.md#if-a-load-will-not-switch).

Four more findings point the same way:

- ⚠️ **NEITHER END has a fill timeout.** Held with `b5` set the intake motor runs
  indefinitely, and the *panel* has none either: given **zero flow for 330 s while
  it held intake commanded** — 8× a normal 40 s fill — it produced no error, no
  alert, no timeout. The dangerous case is not a dry machine but a **failed flow
  meter with water still flowing**: the count stops, the target is never reached,
  and the motor runs forever into a machine that keeps filling. Any automation
  driving `b5` must carry its own timer.

  The end-of-cycle flush is the same hazard with no target at all, and the
  firmware caps it — see [`POST /api/flushcap`](api.md). ⚠️ **The tank is the
  only other thing bounding a fill.** Plumb the machine to an always-on supply
  and you replace a bounded spill with an unbounded one.
- **Do not rely on status bit 0 to catch a dry pump.** It set within ~5 s on one
  run with a bone-dry line, but on a later run a counter frozen for 5.7 s under
  commanded intake left it at 0. Carry your own timer and watch the flow count
  directly.
- **The lid has two switches** — reed on status bit 1, micro switch on bit 7 — and
  the machine wants both before it treats the lid as closed. Rewriting either bit
  overrides half an interlock, not a status light.
- **Faking a fill is a one-byte change.** `E1 Water Inlet Fault` is raised by the
  panel, and the evidence behind it — controller byte 2, the flow count — is
  trivially rewritable in the relayed stream. Panel byte 3, the fill target, is
  equally rewritable: forcing it low makes the machine believe a short fill is
  complete and move on to heating. **Two single-byte edits are enough to make a dry
  machine run a full heat cycle.**

### What this means for you

When you write to byte 1, **you own dry-fire and thermal runaway.** Whatever
protection remains is in the NTC and any thermal cutout in the heater itself, and
neither has been characterised here.

- **Never set `b2`, `b3` or `b4` on a machine you are not watching.**
- **Never drive a heater unless you know water is in there.** Note that "always run
  the wash pump with the heater" is *not* the rule — the OEM breaks it on purpose:
  the sterilise step runs the **water heater alone, no circulation, for 256 s and
  again for 251 s**, reaching 97 °C. That is safe only because the machine counted
  **20 flow pulses** in first and is boiling a measured charge. You do not have
  that guarantee unless you are reading controller byte 2 and acting on it.
- Treat every byte-1 write as directly switching a relay, because it is.
- Prefer PROBE mode and read-only relaying for exploration — it cannot command a
  load at all.
- If you build automation on this, put your own interlock in front of the heater
  bits. The machine will not.

**Splicing into the heater relay is no longer meaningfully more dangerous than
writing byte 1.** Choose the panel path for convenience and reversibility, not
because it is protected.

## Known quirks

**The 24 V wash pump has no feedback at all.** A pump that shuts itself down on
internal protection is invisible to the board, and the countdown keeps running as
though nothing is wrong. **Do not build automations that assume "cycle timer
finished" means "cycle succeeded".**

**`E5 Communication failure`** is the code you are most likely to provoke. Both
sides broadcast on their own clock and each raises `E5` independently when the
other goes quiet. In RELAY mode the ESP32 and its level converter are a **single
point of failure** for both directions — unpowered, crashed or mid-OTA, the panel
stops working. `E5` is **latched at the panel** and survives the link recovering,
so judge the link by byte 3 of the controller frame, not by the display.

**An OTA update interrupts forwarding for 15–20 s.** Never flash mid-cycle.

## This washes baby bottles

If your modification can cause a cycle to *appear* to complete without actually
sterilising, that is a real-world consequence, not a cosmetic bug. Prefer reading
status over inferring it, and fail loudly.

## Liability

MIT licensed, provided as-is, with no warranty. Modifying a mains appliance will
void its warranty and may affect its safety certification. You do this at your own
risk.
