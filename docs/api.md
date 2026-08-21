# HTTP API

Everything the web UI does, it does through these. There is no state the page
holds that the device does not.

Base URL is the device: `http://baby-washer.local/` or its IP. `GET` returns JSON;
`POST` takes query-string arguments, never a body, and returns `{"ok":true,...}`.

**Hex arguments are hex without `0x`.** `clr=42` means `0x42`. The exceptions are
`/api/tempovr` and `/api/virtual`, which take decimal.

---

## Reading

| Endpoint | Returns |
|---|---|
| `GET /api/status` | Everything the UI polls: both raw frames, decoded fields, override state, counters |
| `GET /api/version` | Firmware version, build, MD5, partition, uptime, boot count, reset reason |
| `GET /api/frames` | Newest frames from the sniff ring, annotated with checksum candidates. `?n=` to limit | Rows are annotated `xor-ck` / `sum-ck` when the last byte matches a checksum over the preceding ones.
| `GET /api/hist` | Run-length frame history per direction: `ctrl`, `panel`, `to_panel`, `to_ctrl` |
| `GET /api/deltas` | Frames that differ from the captured idle baseline |
| `GET /api/graph` | 30 min of temperature (bit 7 = **water** heater) plus a parallel `air` lane (air heater or dry), and 60 s of flow |
| `GET /api/flowlog` | Every change of the flow count with a timestamp, `[[ms,count],...]` |
| `GET /api/detect` | Result of the last pin-map scan |

### Which fields to trust

`ctrl_raw`/`fb` and `panel_raw`/`fp` are the **raw frames** and are
authoritative. The decoded fields (`pb1`, `pb2`, `pb3`, `st_real`) come from the
same frame snapshot, so they agree — but if you are analysing traffic, parse the
raw hex and verify the XOR yourself.

`to_panel` and `to_ctrl` in `/api/hist` are what was **actually transmitted**
after any rewrite. When you are testing an override, read those, not `fb`/`fp`.

---

## Driving the machine

⚠️ These energise real loads. [safety.md](safety.md) first.

### `POST /api/panel_ovr?clr=<hex>&set=<hex>`

Rewrite byte 1 of the panel frame on its way to the controller:
`out = (b & ~clr) | set`. This is the load bitmap.

```
b0 wash pump 24 V     b1 drain            b2 water heater 110 V
b3 air heater 110 V   b4 dry blower+heat  b5 intake motor
```

**`b5` alone does nothing.** The intake motor is gated on a non-zero fill target
in byte 3 — see `/api/mode_ovr`. This is the only gate we can demonstrate;
`b1`–`b4` drive their loads directly.

⚠️ **`b0` is a live question.** Commanding it does not close the control board's
low-side switch on the `WS PUMP` header, so either the board is faulty, there is
a second gate nobody has found, or b0 is not the pump. The pump is driven by an
external relay instead.

`b6` and `b7` have **never been seen set**, so the web
UI omits them from its bit table. This endpoint still takes any mask, and forcing
them by hand is the only remaining way to hunt the unattributed 303.7 Ω sprayer
valve.

### `POST /api/mode_ovr?b2=<hex>&b3=<hex>`

Rewrite panel bytes 2 and 3. Empty arguments restore pass-through.

Byte 3 is the **fill target**: `byte3 = round(counts × 0.35)`. Byte 2 is
determined by byte 3 and carries no independent information.

| Pair | Counts | Where the machine uses it |
|---|---|---|
| `2a/07` | 20 | steam charge |
| `ab/1c` | 80 | rinse |
| `40/20` | 90 | wash |
| `d6/23` | 100 | self-clean |
| `ff/ff` | none | end-of-cycle cool-down flush, no target |

⚠️ `ff/ff` is a sentinel, not a volume. The machine only ever sends it with the
drain open, and **neither end has a fill timeout** — with the drain shut the
intake motor has nothing to stop it.

### `POST /api/press?mask=<hex>&ms=<n>`

OR `mask` into byte 1 for `ms` milliseconds, then release.

---

## Lying to the panel

Everything below rewrites the controller→panel direction. The machine itself is
unaffected; only what the panel believes changes.

### `POST /api/status_ovr?clr=<hex>&set=<hex>`

Rewrite status byte 3. This is the fault register:

| Bit | Mask | Effect on the panel |
|---|---|---|
| 0 | `01` | ⚠️ **flow-related, unresolved.** The controller sets it itself; a frozen counter under commanded intake does *not* set it, but a fill released short of its target blips it. See `protocol.md` |
| 1 | `02` | lid reed reads open |
| 2 | `04` | **`E3` Sensor Open Circuit** |
| 3 | `08` | **`E4` Sensor Short Circuit** |
| 4 | `10` | **`E0` Voltage Anomaly** |
| 5 | `20` | **`E7` Fan Failure** — only while a cycle is running |
| 6 | `40` | **`E5` Communication Failure** |
| 7 | `80` | lid micro reads open |

The web UI offers three presets on this: **hide E5** (`clr=40`), **hide faults**
(`clr=7C` — bits 2–6, every fault the controller can report) and **pass all**
(`clr=0&set=0`). The lid bits are deliberately excluded from "hide faults": they
are an interlock, not a fault, and hiding one of the two desynchronises them.

⚠️ **Every fault bit latches.** One frame is enough and the code holds until the
machine is power-cycled; clearing the bit does not clear the display. The panel
shows the **lowest set bit**.

### `POST /api/tempovr?v=<0..255>`

Force byte 1, the temperature the panel sees. `v=-1` or empty restores
pass-through.

This does **not** raise `E3` or `E4` — tested at `0x00` and `0xFF`, no reaction.
The controller reads the thermistor and reports a verdict in a status bit; the
panel only ever sees a number. Pinning it mid-rise while the heater is commanded
is the untested candidate for `E6`.

### `POST /api/flowspoof?v=on|off`

Hold the flow count the panel sees at zero. Only the starve direction exists —
faking the count upward would convince the machine it had filled when it had not.

Does not raise `E1`: tested with zero flow for 330 s while the intake motor was
commanded, and the panel waited without complaint.

### `POST /api/flushcap?ms=180000`

Bounds the untargeted cool-down flush (`pb1` intake bit set with byte 3 =
`0xFF`). Neither end of the link times one out — it ends when the water stops
arriving, which on a stock machine is the hand-filled tank running dry. The same
Self-Clean program was measured at **64.1 s** and at **114.6 s**. Feed the tank
from a float valve or any always-on supply and nothing ends it at all.

After `ms` the **intake** bit is stripped from the forwarded byte 1 and the
**drain** bit is left set, which reproduces the event the controller already
terminates on rather than inventing a new one: the sump empties, the temperature
rises, the cycle moves on.

Applied last, so it also bounds the cycle runner and a manual `b5` override —
both can hold the intake on indefinitely and neither is a reason to allow it.
Metered fills are untouched: they carry a real target and the controller ends
them itself on the flow count.

Default **180 s**, persisted to NVS. That is 1.6× the longest flush ever
observed and longer than the runner's own longest (116 s), so it cannot truncate
normal operation. `0` disables it — only safe on a hand-filled tank, which
bounds the flush by running dry.

Status carries `fcap_ms`, `flush_on`, `flush_ms`, `flush_cap` and `flush_n`.

### `POST /api/wsrelay?mode=off|on|auto[&pol=low|high][&pin=N]`

Drives the **external wash-pump relay**. The control board holds 24 V on the
`WS PUMP` header permanently and switches the low side, and that low-side switch
does not close when panel b0 is commanded — verified with a frame byte-identical
to the one the machine sends during its own wash phase. So the pump gets a relay
of its own.

| Mode | Behaviour |
|---|---|
| `auto` | Follow **b0 of the panel frame as forwarded**. Default. |
| `off` | Contacts held open whatever b0 does. |
| `on` | Forced closed. **Not persisted** — a reboot comes back in `auto`. |

`auto` is the mode that makes this transparent: it reads the *forwarded* byte, so
the machine's own cycles, the ESP32 cycle runner and any `panel_ovr` b0 write all
drive the pump without knowing the relay exists.

`pol` matches the module (`low` = the usual opto-isolated board, default).
`pin` moves it; both persist to NVS.

**Two limits, and they are the only protection there is** — the pump reports
nothing, so the firmware cannot tell whether it is turning:

- **30 min runtime cap.** Trips, latches open, and only rearms when the command
  drops. `wsr_lock` in `/api/status` shows it.
- **Panel-link watchdog.** No panel frame for 3 s and the contacts open, because
  in `auto` the command comes from a frame and stale b0 is worthless.

The relay is also opened before an OTA write, and on every boot before anything
can command it.

⚠️ **An external pull resistor is not optional.** Between reset and `cn2::begin()`
the pin is high-Z, and in safe mode it is never configured at all. 10 k to 3V3
for an active-low module, 10 k to GND for active-high. Without it a crash, a
reset or a boot loop can energise the pump.

### `POST /api/lid?v=on|off`

Force both lid bits together. The machine wants `b1` and `b7` both clear before
it treats the lid as shut, so moving one alone leaves it seeing half an open lid.

---

## Virtual controller — a panel test rig

### `POST /api/virtual?v=on|off[&auto=on|off][&temp=][&flow=][&st=][&b5=]`

Synthesise the controller→panel frame and drop the real controller's. The panel
sees a machine reporting whatever you set.

Turning it on also engages **PROBE**, and that is what makes it safe rather than
merely quiet: the real controller keeps receiving an idle panel frame, so it stays
synchronised and `E5`-free while being unable to receive a single command. Both
directions are neutralised.

`auto` runs a model of the machine from measured constants, so the **panel can
complete a whole cycle against nothing**:

```
fill    2.24 counts/s     12 fills, 5 cycles
heat    0.103 °C/s        24 -> 57 °C in 321 s
cool    tau 459 s         90 -> 36 °C over 13 min
reset   ~1.5 s after the intake motor is released
```

Overrides compose: `tempovr`, `flowspoof`, `status_ovr` and `lid` all apply to
the synthesised frame exactly as they would to a real one.

**Not persisted.** A reboot comes back in plain relay — returning from a crash
into "the machine is fake" would be a bad default.

### Why it exists

Most error codes are only evaluated while a cycle is running. `E7` is the proof:
set status bit 5 on an idle panel and nothing happens, set the same bit mid-cycle
and the panel displays `E7`. It was nearly documented as a dead bit.

So testing fault behaviour needs a cycle in progress, and a rig that can stage one
without water, heat or a 91-minute wait.

### Procedure

1. `POST /api/virtual?v=on&auto=on&temp=26&flow=0&st=0`
2. Start a cycle on the panel — nothing physical happens
3. Inject the fault at the phase you want to test
4. Read the display, then clear the injection **before** power-cycling, or the
   panel re-latches on the way back up

---

## Cycle runner

### `GET /api/cycle`

Stage table plus live state: `state` (0 idle, 1 running, 2 done, 3 aborted),
`stage`, `elapsed`, `why` (abort reason).

### `POST /api/cycle?run=start|stop|resume|defaults`
### `POST /api/cycle?mode=<0..5>`
### `POST /api/cycle?i=<n>&secs=<n>`

Start, stop, resume, restore the compiled defaults, switch program, or retime
stage `n`.

`state` is `0` idle, `1` running, `2` complete, `3` aborted, **`4` paused**. An
open lid or an empty tank **pause** rather than abort — a lid pause resumes on
its own once the lid shuts, a water pause waits for `run=resume`. Pausing
releases every load and keeps the stage and its elapsed time. Faults, over
temperature and a dead link still abort.

Six programs, from the manual (p.25–26). Each has its own stage list, its own
saved durations, and the manual's maximum water temperature as an abort ceiling
on top of the global one:

| n | Program | Max °C |
|---|---|---|
| 0 | Rapid Wash | 55 |
| 1 | Normal Wash (default) | 68 |
| 2 | Steam Sterilization | 96 |
| 3 | Drying | 96 |
| 4 | 72h Fresh Air Storage | 96 |
| 5 | Self-Cleaning | 70 |

### `POST /api/cycle?water=<°C>&dry=<°C>`

Temperature targets for the current program, persisted. `-1` or an omitted
argument leaves that one alone; `0` disables it.

**One sensor.** Byte 1 is the sump NTC and there is no air probe, so `water` is a
real setpoint (a `b2` heat stage ends when it is reached) while `dry` can only be
a ceiling on the same reading (a `b3`/`b4` stage aborts above it). Both are
clamped to the program's maximum.

`mode` is ignored while a cycle is running. Durations persist per program. **`secs=0` means run until the fill target is
reached** rather than for a fixed time — which is how the machine itself ends a
fill, and the only correct way to do it.

⚠️ This drives 110 V heaters and a water pump with the panel held idle and
nothing above it. The interlocks are in the runner because the machine has none:
no heater stage without a verified fill, and abort on fill stall, over
temperature, lid open, controller fault bit or a dead link. Abort clears every
override and releases every load.

Full state machine and stage table in [cycle.md](cycles.md).

### `GET /api/custom`
### `POST /api/custom?slot=<0|1>&name=<text>&stages=<spec>`

**Six** user-editable programs, appended after the six built-ins.

The canonical spec is **7 characters per stage**, concatenated: `CODEnnU` — a
4-letter code, two digits, and a unit (`S` seconds, `M` minutes, `H` hours, or
`P` for a fill percentage), comma separated. `[A-Za-z0-9,]` only, nothing to
URL-encode. Commas are optional on input and always present on output.

```
DRAN20S,FILL90P,WASH05M,FLSH70S,DRYR10M
```

Codes: `DRAN FILL PUMP WASH STEM AIRH BLOW DRYR FLSH WAIT` — see
[cycle.md](cycles.md#canonical-form--what-the-device-stores).

Three authoring forms are also accepted and produce identical bytes: hex
`LLTTSSSS`, `loads:target:seconds` comma separated, and readable stages such as
`drain 20s, fill 90, wash+heat 5m`. The device always returns the canonical
form.
`GET` returns every slot including the free ones, so an editor can offer them.

Delete with `POST /api/cycle?del=<slot>`. A free slot is not a program: it is
reported `"empty": true` in `/api/cycle` and `/api/app`, both pages hide it, and
`mode=` refuses to select it.

A rejected list returns **400** with the reason, so an editor can say why. The
rules and the evidence behind each are in [cycle.md](cycles.md#the-rules-and-why-each-exists)
— the short version is that the validator refuses to let you dry-fire the heater,
fill with no target, or run an untargeted flush with the drain shut, because the
machine checks none of those.

---

## Diagnostics

| Endpoint | Purpose |
|---|---|
| `POST /api/probe?v=on\|off` | Feed the controller a permanently idle panel frame. Buttons can be pressed with no possibility of starting a cycle |
| `POST /api/spoof` | Impersonate the panel toward the controller at ~200 ms |
| `POST /api/send?to=board\|panel&hex=...` | Raw frame injection |
| `POST /api/thin?panel=<n>&ctrl=<n>` | Forward only 1 frame in N toward each end — the TX-margin test |
| `POST /api/detect[?phase2=1]` | Pin-map scan. Phase 1 never drives a line; phase 2 latches `E5` on the panel |
| `POST /api/pinmap?rxb=&txb=&txp=&rxp=` | Set the pin map directly, persisted to NVS |
| `POST /api/sim?sw=2&v=on\|off` | Drive the lid switch input on the controller. `SW1` has no GPIO |
| `POST /api/flow?hz=<n>` | Flow-meter pulse simulator toward the controller. `hz=0` stops |
| `POST /api/baud?b=<n>` | Change the link baud, persisted |

## Resetting

| Endpoint | Clears |
|---|---|
| `POST /api/qclear` | Checksum counters |
| `POST /api/histclear` | Frame history |
| `POST /api/baseline` | Capture the current frames as the delta baseline |
| `POST /api/deltaclear` | Delta table |
| `POST /api/btnclear` | Last-button-code memory |
| `POST /api/flowlog?clear=1` | Flow event ring |
| `POST /api/clear` | The sniff ring |
| `POST /api/reboot` | Restarts the ESP32. A few seconds of link interruption |

## Firmware update

```
curl -f -F "firmware=@.pio/build/c3/firmware.bin" \
     "http://baby-washer.local/update?key=$OTA_PASSWORD"
```

Needs no reverse connection back to the uploading host, unlike espota. See
[ota.md](flash.md).

⚠️ **An OTA quiesces forwarding for 15–20 s.** Never mid-cycle. In virtual mode
it is worse — the panel receives nothing at all and will raise `E5`.
