# Troubleshooting the CN2 interception

Everything here is testable from the web UI on an idle machine — no oscilloscope,
no water, no cycle. It works because **both OEM MCUs watchdog the link**: if either
stops receiving valid frames it raises **E5** (the panel on its display, the
controller in `byte3 b6`), so the machine is its own test rig and a dead path
announces itself within seconds.

## 1. The four paths

| # | Path | Direction | Confirmed by |
|---|------|-----------|--------------|
| ch1 | controller pin 1 → HV1·LV1 → D1/GPIO3 | into ESP | `ctrl→ESP` counter climbing, **0 bad** |
| ch2 | D2/GPIO4 → LV2·HV2 → controller pin 2 | out of ESP | controller `byte3 b6` (E5) stays clear |
| ch3 | D3/GPIO5 → LV3·HV3 → panel pin 1 | out of ESP | **no E5 on the panel display** |
| ch4 | panel pin 2 → HV4·LV4 → D4/GPIO6 | into ESP | `panel→ESP` counter climbing, **0 bad** |

Channels come from [wiring.md](build.md#4-go-inline); if you
wired it differently, `POST /api/detect` tells you what you actually have.

The two RX paths prove themselves — every frame ends in an XOR, so a corrupted byte
is caught and counted. **The two TX paths are blind**: the ESP32 cannot see its own
output, and they are proven only by the far end's watchdog staying quiet.

### ⚠️ E5 does not, by itself, isolate a TX path

If **ch1** is broken the ESP32 has nothing to forward, so the panel raises E5 *even
with a perfect ch3*. Read the RX counter first:

| `ctrl→ESP` counter | Panel display | Verdict |
|---|---|---|
| climbing, 0 bad | no E5 | **ch3 good** |
| climbing, 0 bad | **E5** | **ch3 is the fault** |
| stalled | E5 | ch1 broken — says nothing about ch3 |

Mirror the logic for ch2, reading E5 off controller `byte3 b6` instead.

## 2. Autodetect

```
POST /api/detect             phase 1 only — passive
POST /api/detect?phase2=1    also resolve the transmit stubs
GET  /api/detect             results
```

**Phase 1 never drives a line.** It leans on two facts: only the two OEM *outputs*
have edges (the inputs sit idle-high because nothing drives them yet), so counting
edges for 700 ms splits four pins into two receive and two transmit stubs; and the
headers name the device — `0xA2` is the controller, `0xAA` the panel, with the XOR
proving it is not noise.

```
GPIO3    108 edges  <- driven     rx controller  (0xA2)
GPIO4      0        silent        tx stub
GPIO5      0        silent        tx stub
GPIO6     66 edges  <- driven     rx panel       (0xAA)
```

The edge counts are a sanity check: 108/66 = 1.64 against a frame-length ratio of
8/5 = 1.6. Real traffic, not pickup. Phase 1 cannot tell **which** transmit stub
goes where — both are silent by definition.

**Phase 2** closes the loop through the far end: forward on a guess, then read the
**controller's E5 bit** back over the receive path phase 1 just confirmed. Only two
permutations, so one trial resolves it, and the result is saved to NVS. The
controller's bit is used specifically because it **clears itself** when the link
recovers; the panel's latches.

> ⚠️ **The panel will be showing `E5` when phase 2 finishes and needs a power
> cycle.** That is the unavoidable cost of interrupting the link to test a
> permutation.

When you already know the answer, skip the trialling — persisted to NVS, applied
immediately, no reflash:

```
POST /api/pinmap?rxb=5&txb=6&txp=3&rxp=4
```

Worth running after rewiring, when porting to a machine whose pin directions are
unknown (phase 1 removes the one mistake that can damage a board), or to rule the
pin map out in 2 seconds.

## 3. Test sequence

Run in order; stop at the first failure.

**Test 0 — power up, wait 5 s, look at the panel.** No E5 means ch3 is alive.

**Test 1 — idle 10 minutes, `LINK QUALITY` → `reset` first.** Roughly 3000 frames
each way. Zero bad on both rows means ch1 and ch4 are clean. **Any** non-zero bad
count is the finding: a partly-damaged BSS138 or a cold joint does not fail
outright, it drops the occasional bit, and only the counter catches that.

**Test 2 — ch3, actively.** Toggle the lid override and watch the icon follow. This
proves the bytes are *parsed*, not merely keeping the watchdog fed.

**Test 3 — ch2, actively.** Anything the controller visibly reacts to. Forcing
panel bytes 2–3 is *not* such a test — it does not start a cycle.

## 4. TX margin test — measuring a blind path

Tests 0 and 2 give one bit of information. A channel that is merely *weak* passes
both and fails later, so `/api/thin` makes the blind paths quantitative:

```
POST /api/thin?panel=4&ctrl=1     forward only 1 frame in 4 toward the panel
POST /api/thin?panel=1&ctrl=1     back to full rate
```

Starve one far end and find the N at which it raises E5. **Baseline it on wiring
you trust first** — the absolute number is meaningless alone; a channel that
silently drops frames fails at a lower N than it did before.

## Error codes — BW05 manual, p.29

**There is no E2** — the manual skips it. `E1` is *Water Inlet Fault*, not "no
water"; water shortage is a separate indicator alert.

| Code | Manual text | Evidence on CN2 | How to raise it |
|---|---|---|---|
| `E0` | Voltage Anomaly | ✅ **byte 3 bit 4** | set bit 4 |
| `E1` | Water Inlet Fault | ⚠️ no bit | starve byte 2 during a fill — untested, needs a cycle |
| `E3` | Sensor Open Circuit | ✅ **byte 3 bit 2** | set bit 2 |
| `E4` | Sensor Short Circuit | ✅ **byte 3 bit 3** | set bit 3 |
| `E5` | Communication Failure | ✅ **byte 3 bit 6** | set bit 6, or cut the link |
| `E6` | Heating Plate Malfunction | ⚠️ no bit | pin byte 1 while the heater runs — untested, needs a cycle |
| `E7` | Fan Failure | ✅ **byte 3 bit 5** | set bit 5 **while a cycle runs** — nothing at idle |

`E0`, `E3`, `E4`, `E5`, `E7` are confirmed: each bit was injected and the panel
displayed the matching code. Three things to know:

- **They latch.** One frame is enough and the code holds until a power cycle.
- **The lowest set bit wins.** `0x0C` (bits 2+3) shows `E3` — independent flags,
  not a code field.
- **Forcing byte 1 does not raise `E3`/`E4`.** Tested at `0x00` and `0xFF`.

`E1` and `E6` have no bit, and **`E1` is not a stalled-flow verdict** — zero flow
for 330 s with intake commanded produced nothing. Neither end has a fill timeout.

Two conditions are **indicator alerts, not codes**, and clear themselves:

| Alert | Manual text | On CN2 |
|---|---|---|
| Water Shortage | add water, press Start/Pause, auto-resumes | inferred from byte 2 |
| Lid Open | closes automatically when the lid is shut | byte 3 bits 1 and 7 |

To raise one deliberately see [api.md](api.md#lying-to-the-panel), and
[api.md](api.md#virtual-controller--a-panel-test-rig) for the rig that stages a
cycle without water — required, because `E7` and probably `E1`/`E6` are only
evaluated mid-cycle.

## 5. What a healthy link looks like

```
ctrl ->ESP   297 good, 0 bad
panel->ESP   295 good, 0 bad
byte3 = 0x02     lid reed set (lid off), E5 clear
```

Both sides broadcast every ~200/201 ms, so ten minutes is roughly 3000 frames each way.
A counter that climbs but falls well short means you are losing frames, not the
link — which is what the bad-checksum column and the TX-margin test catch.

Two protocol facts are worth repeating here because they cause misdiagnosis: **the
two sides are free-running, not request/response**, so any "reply latency" is
drifting phase between independent clocks; and **`E5` is symmetric**, so on its own
it never tells you which end lost the link.

## 6. Logging a cycle

**Nothing is stored on the ESP32.** The firmware keeps a 4096-event byte ring —
about 63 seconds at ~65 bytes/s — plus a 24-entry run-length history per direction.
A wash cycle runs 20–40 minutes, so the ring wraps roughly 38 times and a reboot
loses all of it. Capture from the host **before** you start:

```bash
python3 tools/cycle_log.py --out logs/mycycle --hz 5
```

It polls `/api/status`, writes a line whenever the frames change plus a 30 s
heartbeat, and flushes on every write, so a crash costs at most the current line.
Two files: `.jsonl` for analysis, `.log` for reading live. Network drops are
written into the log rather than waited out silently — a gap is evidence.

## 7. If the ESP32 disappears from the network

It is powered from CN2 pin 4, so **if the D8 is unplugged the ESP32 is off** —
check that before assuming a bad flash. If it is powered and still absent, scan the
subnet rather than trusting mDNS; `.local` resolution has proven unreliable here.
If an OTA reports success but the device comes back on the *old* version, the
upload did not apply — re-flash and confirm with `/api/version`.

## 8. Testing the flush cap

The end-of-cycle flush is capped in firmware ([`POST /api/flushcap`](api.md)).
Two ways to prove it works, in the order worth doing them.

### Dry — no water, machine inert

```bash
python3 tools/flushcap_test.py baby-washer.local
```

**SPOOF** feeds the controller a permanently idle panel frame, so nothing the
overrides say reaches it and no load can be energised. The masks are still
applied to the real panel stream on the way past, and that rewritten value is
what the cap watches — so the whole decision path runs while the machine sits
still. The script refuses to command anything until it has confirmed spoof is
engaged, and restores the link in a `finally`.

It checks four things: a **targeted** fill is never capped however long it runs,
an untargeted `0xFF` flush trips the cap on time, the forwarded byte 1 loses
`INTAKE` and keeps `DRAIN`, and releasing the flush re-arms it.

On the `/dev` page the same thing is visible live: while the cap holds, the
`→ctrl` column of **DRIVE LOADS** shows `b5` as `0` with a red `*`, and the
**FLUSH CAP** line reads `CAPPED`.

### Wet — a real cycle

The dry test cannot tell you the one thing only the machine knows: **whether the
controller accepts a mid-flush intake release and moves on.** For that:

1. `POST /api/flushcap?ms=20000`
2. Start **Self-Clean** and let it run to the cool-down flush (last stage before
   drying, ~60–115 s of drain and intake together).
3. Watch `/api/status`: `flush_on` goes true, `flush_ms` climbs, and at 20 s
   `flush_cap` goes true. The serial log prints `[flush] CAP at 20000 ms`.
4. The sump temperature should stop falling and start rising within a few
   seconds — that is the machine seeing the water stop, which is exactly what
   ends the stage normally.
5. Confirm the cycle advances to drying rather than sitting there.
6. Put the cap back: `POST /api/flushcap?ms=180000`.

⚠️ Capture it: `python3 tools/cycle_log.py --out logs/flushcap --hz 5`. Nothing
is stored on the ESP32, so an uncaptured run tells you nothing afterwards.

If the cycle **stalls** at step 5, the controller wants something else to end
that stage and the cap is the wrong lever — set `ms=0`, open an issue with the
log, and bound the water supply in hardware instead.

## 9. `E5` on the panel right after power-on

`E5` is *Communication Failure*, and it **latches at the panel** — once raised it
stays on the display until the machine is power-cycled, however healthy the link
becomes afterwards. So the first question is never "why is E5 showing" but
"is anything still causing it".

Check `/api/status`:

| Field | What it means |
|---|---|
| `st_real` | the controller's **own** status byte. `0x40` = it is asserting the comms fault right now. `0x00` = it is happy and the panel display is just stale |
| `st_set` | non-zero means **you** are injecting the bit — clear it with `/api/status_ovr?clr=0&set=0` |
| `bad_c` / `bad_p` | anything above zero is a corrupted byte on the wire |
| `tx_to_board` vs `panel_bytes` | must be equal — every byte received is forwarded |
| `tx_to_panel` vs `board_bytes` | likewise, the other direction |

`/api/hist` gives the same in order. Its `first` column is an **age in seconds,
not a timestamp**, so read it descending to get chronological order — that is
what tells you whether the fault was there from the first frame or arrived later.

⚠️ **This module is powered from CN2 pin 4**, so it boots at the same instant as
the panel and the controller, and until its UARTs open both TX pins float.
Firmware before **1.1.0** opened them *after* a 1.5 s `while (!Serial)` wait and
a blocking WiFi association — a measured **4.0 s** with the panel link dead at
every power-on, and up to 20 s with the access point down. Newer D8 controllers
raise a comms fault over that. 1.1.0 opens the link first; the same measurement
now reads **0.0 s**. Check with `/api/version`.

Confirmed on the machine that showed the fault: after 1.1.0 and a power cycle,
209 s with the controller's status byte never leaving `0x00`, 1046 controller and
1048 panel frames, **zero bad checksums**, boot dead window **0.00 s**. It had
previously raised the bit ~52 s in and held it.

Measure it yourself — the byte counters are exact:

```bash
# dead window = uptime - (bytes / steady rate).  Controller 40 B/s, panel 25 B/s.
curl -s http://baby-washer.local/api/status |
  python3 -c 'import sys,json;d=json.load(sys.stdin);print(d["uptime_s"]-d["board_bytes"]/40)'
```

⚠️ **Safe mode leaves the UARTs shut**, so the panel and controller cannot reach
each other at all — the machine will show `E5` and refuse to run. It is not a
broken appliance; power-cycle to clear the boot-loop guard.

### Forwarding stalls

`worst_gap_us` is the longest the relay task went between forwarding passes —
1 ms in steady state. `worst_gap_at` says **when**, in millis from boot, which is
the half that matters: WiFi association starves the task for ~96 ms and there is
no stopping that, but ~96 ms is eleven byte times at 9600 baud and wide enough to
split a frame at whichever end is mid-transmission. Landing in the window the
panel and controller use to find each other is a different thing from landing
afterwards.

So the firmware forwards on a quiet link until ten good frames have arrived each
way (~2 s) and only then brings up the radio. Measured after the change: worst
gap 96.2 ms at **t=3.69 s**, clear of the startup window. Both counters reset
with `POST /api/qclear`.

⚠️ **A weak radio used to take the panel link down with it.** `net::loop()`
restarts the board after 60 s without WiFi, and every restart drops forwarding
for a whole boot — long enough for a panel to latch `E5`, which then needs an
appliance power cycle to clear, to recover a radio a restart usually cannot fix.
From 1.2.0 the restart only fires when the CN2 link has gone idle too; while
frames are flowing it re-kicks the supplicant indefinitely and logs that it is
holding off. Check `rssi`: −59 dBm is comfortable, −78 is not.

If the link is clean and `st_real` is `0x00` but the panel still shows `E5`,
power-cycle the machine. If `E5` comes straight back, take the module out of the
loop — panel plugged directly into the controller — and power on. Clean without
it means the problem is in our path; `E5` anyway means it is the machine.
