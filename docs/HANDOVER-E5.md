# HANDOVER: Momcozy D8 "unit 2" E5-at-boot — for the next agent

Written 2026-08-27 by the outgoing agent after a three-day investigation that
fixed everything except this. Read `postmortem.md` and `boot-review.md` after
this file; they carry the full evidence chains. Trust the measurements in
this document; distrust every narrative, including mine.

## 0. The one-line problem

Since Aug 26 ~17:45, the washer's controller latches status **bit 6** (panel
shows **E5**) ~5 s after every power-on **when the ESP32 interceptor module
is inline**, and boots clean when the panel cable is connected directly
(bare). The machine worked perfectly with the same module, same firmware,
same NVS at 15:31 the same day. Nothing that is constant across that
boundary can be the cause; everything software-side has been proven constant
or was wiped.

## 1. System map

- **Machine**: Momcozy D8 bottle washer, later touch-panel revision.
  Controller board talks to front panel over `CN2` (4-pin: GND, +5V, two
  UART lines, 9600 8N1). Frame formats in `docs/protocol.md`.
- **Module**: XIAO ESP32-C3 + 4-ch BSS138 shifter, INLINE on CN2 (cable cut,
  both halves land on the module). Powered FROM CN2 pin 4 → it reboots with
  the machine. **As-wired pin map: rxb=5 txb=6 txp=3 rxp=4** (differs from
  docs/build.md which describes 3/4/5/6 — the owner rebuilt it Aug 25; NVS
  map matches physical, verified: frames decode on 5/4, dead on 3/6).
  IP `192.168.14.13`, mDNS `d8-sniffer.local`. Firmware in this repo,
  `firmware/`; deployed 1.17.7. OTA: `POST /update?key=<OTA_PASSWORD>` —
  password in `firmware/include/secrets.h` (gitignored, on the dev box).
- **Smart plug**: Shelly Plug US **Gen4** `192.168.14.95`, name "D8 Washer",
  `initial_state=on`. HTTP RPC, no auth. Real power metering. `toggle_after`
  gives a true self-restoring power cycle — `tools/shelly.py`
  (status/on/off/cycle) or `POST <esp>/api/kasa?cycle_s=N`.
- **HA**: `homeassistant.local:8123` (ssh alias `ha`, key auth, /config).
  Shelly integration live; `packages/momcozy_d8.yaml` REST-polls the ESP.
  Dashboard tab: `/dashboard-cradlewise/d8`. NO daemons — owner directive:
  ESP32 must be self-contained; HA is display-only.
- **Repos**: `~/software/d8-smart` (private, full history) and
  `~/software/smart-baby-washer` (public mirror). Firmware source of truth
  the owner edits from: `~/software/momcozy_d8/cn2sniffer`.

## 2. Owner directives (standing, do not violate)

- CPU relay is the operating mode; NEVER auto-switch to wire mode. Wire is a
  diagnostic, on explicit request. (Both persist via NVS `wire`.)
- No PC/HA daemons in the run/recovery path.
- Self-heal (`/api/heal`) is opt-in OFF; its strike counter resets only by
  human command. It once power-cycled the machine all night — see §6.
- Repos stay private until the owner flips them; never commit secrets.

## 3. Tooling you have

- ESP API: `/api/status` (everything), `/api/frames?n=` (timestamped ring,
  ~60 s deep), `/api/hist` (dedup frame history; array is oldest→newest,
  `first/last` = frames-ago — MISREAD TWICE, cost hours), `/api/graph`
  (temp/flow/power trends), `/api/panel_ovr`, `/api/mode_ovr`, `/api/wire`
  (GET shows live+stored), `/api/pinmap`, `/api/kasa` (`type=shelly`,
  `test`, `cycle_s`), `/api/nvswipe?confirm=yes`, `/api/heal`, `/api/prof`,
  `/api/wirecheck` (edge counts on all four pads — proves conduction).
- `firmware/minimal/main_min.cpp` (env `c3min`): bridge+WiFi+OTA only, the
  exoneration instrument. NOTE: no WiFi reconnect — if a boot misses
  association it stays dark; power cycle again.
- OTA over the flaky link: fresh-boot trick — Shelly `toggle_after=6`, wait
  for `/api/version`, sleep 10–12 s, then upload full speed. Usually lands
  within 4 rounds. Uploads die if attempted on an "aged" link.
- Host tests: `pio test -e native` (111 green, includes the E5Model suite
  that reproduces every firmware-caused E5 class and forbids regressions).

## 4. Precise symptom (from frame captures — `captures/boot-frames-e5-*.txt`)

- Controller boots **clear** (byte3=0x00), replies to the panel's 200 ms
  polls within 3–12 ms, checksums perfect, then latches byte3=0x40 at
  **t≈5.1 s** between two byte-identical polls.
- Controller frame **byte 5**: healthy era idles `0x09` (0b1001); broken era
  `0x0A/0x0B` (bit1 ON; bit0 flickers). It initializes ~0x00→0x0A at
  t≈0.7 s. It has read steady-`09` for stretches (once 5+ minutes) and the
  latch STILL occurred on the next boot — so byte5-bit1 correlates but the
  gate may sample earlier than any window we catch, or byte5 is not the gate.
- The latch survives cuts of 5 s–10 min. Historical cut data (CSV in
  `captures/`): during Aug-23-era locks, 60 s cuts 0/5, 120 s 3/5, 180 s 2/2
  — but the CURRENT condition relatches at every boot regardless.
- A latched controller ignores external load commands (proven: intake+target
  → zero flow; 60 s wash+heat → 0.0 °C rise) but its frames keep flowing.

## 5. The exoneration ladder — do not re-climb it

Each rung was TESTED, not argued (details in postmortem/boot-review):
1. Firmware logic — 20-line minimal image (silicon bridge from first app
   instruction, no logic in data path): **latches**.
2. Image size / boot speed — 23 % smaller, zero init: latches.
3. Every firmware version, incl. rebuilt same-day-working 1.16.3: latches.
4. NVS — full wipe to virgin + minimal re-provision: latches.
5. Pin map — re-verified empirically both ways (decode dead on 3/4/5/6,
   alive on 5/6/3/4).
6. GPIO10 boot drive (real bug, fixed 1.17.3 — wsr pin untouched when off):
   not the cause.
7. Frame content/timing — byte- and ms-identical to working logs.
8. Masks (`e5filter`), wire vs CPU, boot-bridge timing (t≈300 ms): no effect.
9. ROM-stage pad analysis (JTAG pulls on GPIO4/5/6): cannot pull a line low;
   constant since day one.
10. **Bare machine: clean** (owner-run, repeated).

## 6. Open contradictions — the real leads

- **The overnight 38 W**: Aug 26 23:00→Aug 27 08:50, HA's Shelly power
  history shows a flat ~38 W pump-class load for 7.6 h starting AT a boot,
  during virgin-NVS **wire mode** (ESP TX physically detached) with the
  panel E5-latched. Owner states the controller never self-starts loads on
  boot. Then WHO drove 38 W? Unresolved. Suggest: capture what a LATCHED
  panel actually transmits (nobody ever verified it stays idle in all latch
  entry paths), and what byte1 the controller sees at such a boot.
- **The Wednesday 15:31→17:45 differential**: that afternoon contained heavy
  cycle testing (heat, vibration, dozens of hard power cycles) and a flush
  bug that ran water ~2 min (see postmortem). No code change survives the
  elimination ladder. Something PHYSICAL changed. Candidates, in my order:
  BSS138 channel stressed/failing at analog margins; solder joint; CN2
  connector seating; controller input stage damaged; and (weakly) some
  panel/controller NVM state that bare-boot masks — though bare-clean argues
  against controller NVM.
- **byte5 semantics**: never decoded. Bits 0/1 swap between healthy/broken;
  a new value 0x06 appeared once after the pump-night. Manipulating lid,
  tank, buttons moved nothing. Worth mapping properly (drive each machine
  input while streaming `fb`).

## 7. Ranked next experiments

1. **USB split-power test** (decisive, cheap; owner has the module
   accessible): USB-C charger into the XIAO → ESP never reboots with the
   machine → bridge alive from the controller's first microsecond → Shelly-
   cycle the machine. Clean boot ⇒ ROM dark-window timing was the cause AND
   the permanent fix (leave USB in). Latch ⇒ analog presence confirmed.
   Note XIAO diode-ORs USB and 5 V rail; dual feed is safe.
2. **Multimeter on the shifter during a latch** (HV1..4 idle ≈5 V via 10 k
   pull-ups; a dead channel reads floating/low; wiggle-test joints).
3. **Latched-panel transmission capture**: `/api/frames` while the panel
   displays E5 through a fresh latch — verify it truly idles 0x00 in every
   entry path (bears on the 38 W mystery).
4. **Scope/logic-analyzer on CN2 at the controller connector** during a
   module boot vs a bare boot — the only view that sees what the
   controller's input pin actually receives during t=0–5 s.
5. If all else stalls: swap the BSS138 board (~$1) on spec; it is the
   component most exposed to that afternoon's stress and least observable.

## 8. Lessons paid for in hours — read before acting

- `HardwareSerial::setPins(samePins)` is a silent no-op; the GPIO-matrix
  bridge steals pads invisibly to periman. Reconnect TX signals explicitly
  (see wireSet(false)). This one cost 12 hours of ghost-hunting.
- Test UI through a BROWSER, not curl: the board serves ONE http client;
  parallel fetches race and the loser is dropped (chain posts).
- The shared `Preferences s_prefs` handle breaks after any begin()/end()
  pair elsewhere; use local handles for every persist (several fixed).
- A 0xFF flush is controller-LATCHED: stopping it = intake off, drain held
  ~20 s, then release. Plain release leaves water running.
- FramePos alias: idle-stream byte loss self-sustains and XOR-validates;
  detection needs the byte-1 invariant + wire-gap re-lock (in cn2core, host
  tested).
- `pkill -f <pattern>` matching your own command line kills your own shell.
- Owner's instincts have repeatedly out-diagnosed instrument readings.
  When they say "not X", retire X quickly and retest assumptions instead.

## 9. Artifact index

- `docs/postmortem.md` — full multi-day evidence narrative + retractions
- `docs/boot-review.md` — instruction-level boot audit
- `captures/boot-frames-e5-20260826.txt` — the latch, frame by frame
- `captures/unit2-firstcycle-*` — healthy-era reference traffic
- `captures/unlock-attempts.csv`, `ha-*-final.*` — cut/latch field data
- `firmware/test/test_cn2core/` — 111 host tests incl. the E5Model suite
- HA history: `sensor.d8_washer_d8_washer_power` — the overnight 38 W curve

Good luck. The machine's owner wants a washer, not a mystery — if experiment
1 fixes it, stop there and let the analog question rest.


---

## Update 2026-08-28: module went fully dark

The ESP died in place overnight (last alive 2026-08-27 after the 1.17.8
flash, link healthy). Evidence: never associates with WiFi (ARP
incomplete), absent from a full /24 sweep, machine standby draw normal
(1.8 W). Not recoverable remotely: 3 power cycles + 30 s cut + 5-min
cold cut all failed. Chip is not booting.

Significance: strongly supports the hardware-degradation branch. A
failing power path (CN2 pin-4 rail / regulator) explains BOTH the
E5-at-boot onset on 08-26 (rail sag disturbing the link during boot)
AND the final no-boot state -- one fault, progressing.

Decisive test unchanged (= ranked experiment #1, now also the recovery
step): USB-C into the XIAO.
- Boots on USB -> rail/feed dead -> E5 root cause likely found; run the
  module on USB power permanently.
- Dead on USB -> board failed -> replace XIAO, `pio run -t upload`,
  pin map 5/6/3/4 is the compiled default since 1.17.8.

Note: with the module inline and unpowered the panel<->controller link
is SEVERED (bridge exists only while the ESP runs). Machine unusable
until physical access; panel-direct-to-CN2 restores standalone use.


## Update 2026-08-28 (later): machine WORKS with the "dead" module inline -- diagnosis nearly closed

Kai reports the machine working again, module untouched, ESP still absent
from the network. This overturns the "chip not booting" call: the link
physically routes through the module, so a working machine PROVES the
bridge is forwarding, i.e. the CPU boots and runs instantBridge (first
instruction, register-state only, ~uA). Only the RADIO is dead.

Coherent single-fault story, now spanning every observation:
degrading 5V feed (CN2 pin-4 rail / regulator). Quiescent CPU + GPIO
bridge survive; WiFi startup bursts (~300 mA) collapse the rail.
- Pre-08-26: rail healthy -> everything worked.
- 08-26 onward: rail sags during WiFi startup -> link disturbed at
  t~5.1 s after boot (exactly the radio-burst window) -> controller
  latches E5 every boot.
- 08-28: degradation progressed; WiFi cannot start at all (possibly a
  brownout-reset loop; bridge re-establishes within ms each pass) ->
  module offline, machine runs CLEAN because the disturbance source
  (radio) never comes up.

The module has degraded itself into the "dumb wire, no radio" arm of the
experiment ladder -- and the machine running clean in that arm points at
WiFi power draw on the shared rail, not the data path and not firmware.

Confirmation + fix in one step (unchanged, USB split-power): feed the
XIAO from a USB-C charger. Expected: WiFi returns AND E5 stays away.
If E5 returns with WiFi on USB power, this story is falsified and the
shifter/analog branch reopens.

Interim state: machine usable from the panel; no web UI / HA / logging
until the module gets USB power.


## Update 2026-08-28 (USB split-power attempt): back-feed invalidates the test

Kai fed the XIAO from USB (PC) with the machine open. Result: ESP alive on
WiFi (1.17.8), link PERFECT (2381 frames, 0 bad checksums), but the panel
still shows E5 and the controller reports st=0xC2.

WHY THE TEST DID NOT RUN: the split was never achieved. wiring.md is
explicit -- "controller pin 4 (+5 V) = panel pin 4 = converter HV = XIAO
5V, one net". USB VBUS therefore back-feeds the machine logic rail through
the XIAO 5V pad. PROVEN: with the Shelly at 0.0 W (mains fully off) the
controller kept emitting B>P frames every 200 ms -- the controller and
panel MCUs were running on USB power.

Consequences:
- The Shelly can no longer reset the controller/panel while USB is
  plugged, so the E5 latch CANNOT be cleared and every "power cycle" is a
  brownout dip (loads drop, logic stays up) -- itself a good way to INDUCE
  a fault. st was observed going 0x82 -> 0xC2 across one such dip.
- All boot-latch results taken in this configuration are void.
- st=0xC2 = 0x40 latch + 0x82 lid-open (machine is physically open;
  0x82 is expected while the lid is off and is NOT a fault).

REQUIRED to actually run the experiment: disconnect ONLY the wire from the
CN2 pin-4 (+5 V) net to the XIAO **5V pad**. Keep everything else --
converter HV stays on the machine 5 V net, GND common, all four signal
lines. Then the XIAO is USB-only, the machine is Shelly-controlled, and no
back-feed exists.

That cut is simultaneously the decisive TEST and the candidate FIX: it
removes the module inrush/WiFi load from the machine 5 V rail at boot,
which is the surviving hypothesis for the t~5.1 s latch.


## Update 2026-08-28 (evening): WiFi ELIMINATED — radio-off test with serial telemetry

New tooling (fw 1.18.3): a **USB serial console** plus an NVS `nowifi`
mode that skips net/web/kasa entirely. This finally decoupled telemetry
from the radio, which had been the last unexonerated load. Console keys:
`s` status, `m` 1 Hz monitor, `c` CPU relay, `w` wire, `e`/`E` E5 mask
on/off, `r` reboot, `W`/`X` radio on/off, `?` help. Flash over USB with
`pio run -e c3 -t upload --upload-port /dev/ttyACM0` -- no OTA needed.

RESULT: with the radio completely dark, the controller STILL latches.
Captured at 1 Hz over serial, twice:

    t=40s  A2 16 00 82 04 00 03 31   clean boot, NO latch
    t=41s  A2 16 00 82 04 0C 03 3D   byte5 settles 0x00 -> 0x0C
    t=45s  A2 16 00 C2 04 0C 03 7D   bit 6 set -- exactly 5 s later
           bad_c frozen at 54 across the whole window: ZERO bad frames

**WiFi power draw and RF are therefore NOT the E5 trigger.** The theory
that survived three days is dead, and with it the whole rail-load branch
as far as the radio is concerned.

Confirmed structure of the fault: the controller boots CLEAN, communicates
perfectly for 5 s, then raises bit 6 with no corrupted frame anywhere in
between. It is a timed decision, not a reaction to bad data.

Byte 5 is the earliest divergence and now looks like a state code, not a
bitfield: 0x09 healthy idle, 0x0A on the 2026-08-26 latch (lid closed),
0x0C today (lid open, machine on the bench). Still undecoded, but it
settles ~1 s after boot -- 4 s BEFORE the status bit -- so whatever the
controller decides, it decides early.

Also established today:
- The 5 V wire removal DID isolate the controller: during a mains cut its
  line goes to noise (bad frames, ~500 byte/s of garbage) and ok_c freezes,
  i.e. the controller is genuinely unpowered. The PANEL stays up on USB.
- Therefore the panel no longer resets with the machine, so its own E5
  latch cannot be cleared by the Shelly. Clearing it needs mains off AND
  USB unplugged together.
- Bit 6 can only be masked in CPU relay mode. In wire mode the pad bridge
  means no byte passes through software and the E5 filter is inert however
  it is set -- worth knowing before trusting `e5f_mode=2`.

Open at the end of the session: the machine reads 0.0 W on the Shelly with
the outlet ON (standby was 1.4-1.8 W), i.e. it is not receiving mains --
a physical matter with the unit open on the bench.

Remaining ranked experiments: unchanged, minus WiFi. The fault is a
5-second timed decision by the controller that only occurs with the module
electrically inline, so the next evidence must come from a meter or a
scope on the CN2 lines, not from firmware.


## 2026-08-28 (night): ROOT CAUSE FOUND — the ESP->controller TX channel is open

Kai ran the clean A/B: CN2 back to the controller with the ESP entirely
removed -> NO E5, machine works. ESP back inline -> E5. The module is the
cause, as every earlier test implied. `/api/pinprobe` then localised it to
a single channel, reproducibly (4 probes, 2 rounds):

    GPIO6 -> CONTROLLER   pullup=1 pulldown=0 drive=1/0   external pull-up MISSING
    GPIO3 -> PANEL        pullup=1 pulldown=1 drive=1/0   external pull-up PRESENT

Both pads drive and read back their own level (edges 200/200), so the ESP
is fine. The difference is entirely external, and the panel channel is the
built-in control: same board, same probe, opposite result.

`pulldown` is the discriminating measurement. With the pad's INTERNAL
pull-down (~45k) enabled, a healthy channel still reads HIGH because the
shifter's 10k pull-up wins. The controller channel reads LOW -> no external
pull-up is present on that LV net -> that channel is open or unpowered.

Consequence, and it explains every observation of the last three days:
the ESP can pull the controller's RX line low through the MOSFET body diode
but cannot return it high, so the controller receives nothing usable. It
declares panel starvation ~5 s after boot and raises bit 6 -- which is
exactly the unit-1 semantics of that bit, and exactly the 5 s timing
captured over serial. Meanwhile:

- the ESP RECEIVES both directions perfectly (bad=0) -- RX paths are
  independent of this break, which is why every capture looked healthy;
- the bare machine works -- the panel drives the controller directly and
  the shifter is not in the path at all;
- wire mode, CPU relay, the minimal image and radio-off were all
  irrelevant, because the break is on the FAR side of the ESP pad. Every
  software experiment was testing the wrong side of the fault.

That is why the firmware could never be made to fix it, and why the fault
appeared after Wednesday's heavy cycle testing -- heat and vibration on a
solder joint or a wire.

THE FIX (hardware, one connection): repair the path from XIAO **GPIO6** to
the controller's CN2 **pin 2** (its RX input) -- the converter LV pad wire,
that channel's pull-up/solder joints, and the HV wire out to pin 2. Verify
with `POST /api/pinprobe?pin=6`: it must report "pin drives and reads back;
shifter pull-up present", matching GPIO3. A 10k from that LV net to 3V3
restores the pull-up if the resistor/joint is the failure; an open wire
needs re-making.

Diagnostic lesson worth keeping: we could see everything the module
RECEIVED and nothing it TRANSMITTED, so a broken transmit path presented
as a perfect link. `/api/pinprobe` -- comparing a suspect channel against a
known-good one on the same board -- is the tool that closed it, and should
be the FIRST thing run whenever both ends complain while the captures look
clean.


### Detect confirms the map, and narrows the fault to GPIO6 (2026-08-28 night)

`GET /api/detect` (passive edge counting, independent of config.h and of
wiring.md):

    edge counts   GPIO5=136   GPIO4=66   GPIO3=0   GPIO6=0
    rx_ctrl=5  rx_panel=4   tx stubs = {6, 3}

The two DRIVEN stubs are GPIO5 (controller output, 8-byte frames) and
GPIO4 (panel output, 5-byte frames) -- the 136:66 ratio matches 8-byte vs
5-byte frames at the same 5 Hz. GPIO3 and GPIO6 carry ZERO edges, i.e.
they are the two device INPUTS. That is a third independent confirmation
of the map, after frame headers (A2 on GPIO5, AA on GPIO4) and the
no-corruption test (driving GPIO6 200 times leaves bad_p at 0).

Kai reports the module was never rewired, so wiring.md's table disagrees
with the copper. Measurement wins; the table is stale and should not be
used to trace this harness.

Which input is which: Kai reports the LID OVERRIDE still works during E5.
That override rewrites the controller->panel frame (cn2.cpp:886,
"what the panel will see"), which goes out on txp = GPIO3. So GPIO3 ->
panel input is CONFIRMED BY FUNCTION, and GPIO6 -> controller input
follows by elimination.

This makes the diagnosis airtight rather than inferred: GPIO3 is a
working input stub WITH its shifter pull-up; GPIO6 is the other input
stub WITHOUT one. A connected channel presents ~10k from the ESP pad to
3V3. GPIO6 presents none, so that net is open.

Note the disproof of the "D4 goes to panel pin 2" reading: GPIO6 shows
ZERO edges. The panel transmits 5 frames/s, so anything on the panel's TX
output would show ~66 edges like GPIO4 does. GPIO6 sees nothing at all.

SHARPEST MEASUREMENT (no channel labels needed, power off):
measure resistance from the XIAO **D4/GPIO6 pad to 3V3**, then from
**D1/GPIO3 pad to 3V3** as the reference. Good channel ~10k; the faulty
one reads open. That one comparison localises the break.


## 2026-08-28 (late): the LV (3V3) rail to the level converter is the fault

The pull-up probe CHANGED STATE during the session, which is what finally
identified the failing part:

    earlier tonight   GPIO3->PANEL  PRESENT     GPIO6->CONTROLLER  MISSING
    now               GPIO3->PANEL  MISSING     GPIO6->CONTROLLER  MISSING

Kai reports the lid override worked earlier tonight (visible on the panel
even while E5 was displayed) and does not work now. That matches the probe
exactly: the panel channel had its pull-up while the override worked, and
lost it when the override stopped working. Hardware does not change between
two identical probes -- the connection is intermittent, and it degraded
while the unit was being handled.

Both TX channels failing TOGETHER points at one shared node, not two broken
signal wires. The shared node is the converter LV rail: **XIAO 3V3 -> the
converter LV pin**.

Why that produces exactly this fault, on a BSS138-style shifter whose gates
are tied to the LV rail:

- LV rail dead -> every MOSFET gate sits at 0 V -> the transistor never turns
  on, so the ESP->device direction is dead. Only the body diode conducts, so
  our LOWS still reach the far side and our HIGHS never do. Both the panel
  and the controller therefore receive broken data and each raises its own
  comms fault -- panel shows E5, controller sets bit 6 five seconds after
  boot.
- device->ESP still works, because the Arduino UART enables the ESP32 pad
  pull-up on RX, so reception needs nothing from the converter. That is why
  every capture looked perfect: bad=0 in both directions, all week.
- the bare machine works because the converter is not in the path at all.
- the fault comes and goes with handling, which matches "it was working in
  the morning" and "lid override worked before, even on E5".

FIX: re-make the XIAO **3V3 -> converter LV** connection (and confirm the
converter GND pad is common with XIAO GND). No meter needed to verify:

    POST /api/pinprobe?pin=3
    POST /api/pinprobe?pin=6

Both must report "shifter pull-up present". Today they read MISSING, and a
healthy channel read PRESENT earlier tonight, so the test has a confirmed
positive control on this very board.

Software was never implicated: the transmitted frames are byte-perfect with
valid checksums, verified from the to_panel history
(A2 18 00 C2 04 0C 03 73 received -> A2 58 00 82 04 0C 03 73 sent with a
deliberate 88 C spoof and bit 6 stripped).


## 2026-08-29: firmware EXONERATED by full-history bisection

Kai asked for a revert. Three builds spanning the entire project were
flashed to the same hardware and power-cycled, with the machine on the
Shelly so the controller genuinely restarted each time:

| build  | what it is                                   | result |
|--------|----------------------------------------------|--------|
| 1.18.5 | current                                       | latches, st=0xC2 |
| 1.14.1 | 2026-08-23, the build that ran a FULL cycle inline | latches, st=0xC2 |
| 1.0.0  | first public commit (9bae832), PLAIN RELAY -- no E5 filter, no wire mode, no rewriting of any kind | latches, st=0xC2 |

Every one latches within ~5 s of controller power-up with zero bad
checksums in either direction. **The firmware is not the cause, across its
entire history.** 1.0.0 in particular rewrites nothing at all, so there is
no software behaviour left to blame.

Combined with Kai's own A/B (module removed -> machine runs clean; module
back -> E5), the fault is the module's physical presence and nothing else.
No further firmware work can address it.

Also settled tonight, and worth not re-litigating:
- The ESP->panel path WORKS. The panel displays exactly what we transmit:
  it showed lid OPEN because the controller reports 0xC2 and we forwarded
  it faithfully, and the frame history confirms every emitted frame is
  byte-perfect with a valid checksum. Kai was right that the panel keeps
  updating lid state during E5 -- my "frozen panel" theory was wrong.
- The lid override works: 0xC2 -> 0x00 on the wire, checksum valid.
- A reported "st_fwd oscillation" was NOT real: it was the status JSON
  being sampled while the relay task updated the mirrors. The to_panel
  history shows a perfectly uniform stream. Read /api/hist, never the
  status mirrors, when the question is what actually went out.
- 1.14.1 boots into WIRE mode, where the E5 mask is inert (pad bridge, no
  byte passes through software) and it predates the setPins() fix, so
  switching it to CPU relay is a silent no-op. It is strictly worse than
  current firmware for running the machine.

Practical position: the machine runs correctly with the module removed.
Anything further on E5 needs bench instrumentation on the module itself,
not another firmware change.
