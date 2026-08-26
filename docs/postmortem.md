# Postmortem: the E5 lockouts and everything mistaken for them

Aug 22–25, 2026, touch-panel D8 ("unit 2"). Every claim below is tied to a
recorded observation; where the evidence stops, the claim stops.

## What status bit 6 (panel: E5) actually is — PROVEN

The controller's **panel-frame-starvation latch**. Evidence:

1. On unit 1 the controller raises bit 6 when starved of panel frames
   (documented during the original build; there it clears again by itself).
2. Unit 2 latched it **seconds after power-on** twice (Aug 23, boot+60 s and
   boot+5 s) — precisely the window where the ESP32 is still booting and the
   inline link is dark. Closed by `earlyBridge()` (pads bridged in the first
   milliseconds of `setup()`, fw 1.15.0).
3. A 97-minute latch exactly spanned the one incident where forwarding
   provably wedged while the CPU stayed up (Aug 24 16:58, OTA interrupted the
   relay; 92 frames forwarded, then silence).
4. Frame data was never the cause: **zero bad checksums in either direction
   across the entire investigation** (~500k+ frames), byte counts in == out,
   and captured onsets show byte 3 as the only byte that changes.
5. The panel drops every load 0.2 s after seeing bit 6 and latches E5 on its
   display. On unit 2 the controller then ignores all load commands until
   **mains removal** — cut duration required is nondeterministic (field CSV:
   60 s failed 5/5, 120 s worked 3/5, 180 s worked 2/2).

## The Aug 23 lock storm (19 events) — mechanism proven, per-event trigger not

All 19 occurred under early firmware (1.2.0–1.13.x) whose relay ran without
today's instrumentation, so no per-event stall trace exists. What the record
supports: the starvation mechanism above, one measured 96.8 ms relay stall at
WiFi association, and locks recurring fastest when HTTP/recovery traffic was
heaviest. What ended them is a matter of record: from Aug 23 19:02 (bridge
forwarding from boot) — zero locks in 28 h; and Aug 25 (genuine CPU relay,
post-fix, profiled ≤9.6 ms worst stall under deliberate load) — zero locks
through manual load tests, a measured diagnostic, and three supervised cycles
including a full wash+steam+dry. The precise Aug 23 trigger was never
captured and is not claimed.

## The "CPU relay is cursed / wire mode is magic" myth — RETRACTED

`HardwareSerial::setPins()` is a silent no-op when pin numbers are unchanged,
so after the first wire-mode engagement, **every switch back to "CPU relay"
left the silicon bridge connected and the UART TX signals detached**
(1.14.0→1.15.0). Consequences, all reproduced and then fixed in 1.15.1:

- every mode-comparison datum in that window had bridge forwarding in both
  arms — the "wire = reliable / CPU = trouble" table was measuring one mode;
- every override (loads, lid forcing, cycle-runner stages, intake fills
  byte-identical to the panel's own `AA 20 AB 1C 3D`) was written into a
  detached UART — producing twelve hours of ghost theories (intake "gate"
  bytes, drain choreography, atomic transitions, lid switches, tank seats);
- the proof of fix: the first intake command after 1.15.1 pulled water
  (flow 0→40) with the exact bytes "refused" all morning.

Post-fix reality: **genuine CPU relay is the operating mode and runs
lock-free** (Aug 25, hours of operation, full cycles). Wire mode remains a
diagnostic, engaged only on explicit request.

## Errors that were never bit 6

- **E1 / "no water" (Aug 24 morning)** — genuine: a panel-driven fill moved
  64 counts and stalled; water was not reaching the pump; reseating the tank
  fixed it. Honest machine behavior.
- **"Intake and drain stuck on"** — the controller's own autonomous flush
  routine; it self-terminated (~10 min) and left no fault. Not welded relays
  (a weld cannot release itself).
- **WiFi/page lag/OTA failures** — a link that degrades with uptime and
  resets to full speed on reboot (1.29 MB OTA: 14.5 s on a fresh boot; the
  same transfer died 8× on an aged link). Mechanism unproven; unrelated to
  the machine (wire/CPU forwarding both immune). The 11 dBm TX cap was ruled
  out as the lock cause (locks predate it) and removed regardless.

## Self-recovery limits — measured, not assumed

The ESP32 is powered from CN2 pin 4, i.e. by the smart plug it can switch.
Three mechanisms for surviving its own mains cut were tested on the HS103:
`count_down` is cancelled by any manual relay write; a one-off `schedule`
rule survives the relay change but never fires; the `count_down` table holds
exactly one rule ("table is full"). **Self-power-cycle is impossible with
this plug.** The self-contained design therefore is: detect the latch
(`locked_ms`), protect (StuckLoad → one-way mains cut), persist cycle
progress, and after a *human* power cycle, auto-resume behind strict gates
(link settled, controller clear, panel idle, 20 s stable uptime, one-shot).

## The NVS shared-handle trap (found while making CPU relay the boot default)

Several setters call `begin()/end()` on the shared `Preferences` handle;
afterwards, reads through that handle return defaults and writes fail
silently. The boot path then did `read wire pref (default true) →
wireSet(true) → persist true`, actively overwriting a stored `false` on
every boot. Proven by readback: `stored:false` before boot, `stored:true`
after. Fixed by local handles at every persist/read site (1.16.1–1.16.3);
boot default verified across power cycles.

## Final architecture (1.16.3)

ESP32 100 % self-contained: CPU relay forwarding + full override capability,
safety guards (fill-stall, heat ceiling, stuck-load mains cut), lock
detection, NVS cycle persistence with gated auto-resume. Home Assistant
polls read-only REST for display. No PC or HA daemon exists in the running
or recovery path. Removed as dead or externally-coupled: the HA watchdog,
telemetry logger, reachability watcher, lockout webhook (`hook.cpp`),
FlushCap, DrainExtend, the WiFi TX cap.

## Addendum (Aug 26): two more, both caught by the app's new flush button

- **A 0xFF flush is controller-LATCHED**: it ignores a plain load release and
  ends only when water stops arriving — the machine's own programs end it the
  same way. Stopping one therefore means intake OFF, drain HELD (~20 s) until
  the controller notices dry and terminates, then release. Releasing
  everything at once leaves the intake running until a power cycle. (This is
  also the behavior the removed FlushCap guarded — its mechanism was right
  even if the feature was dead weight.)
- **The idle-stream parse alias**: lose one byte on `AA 00 00 00 AA` traffic
  and the previous frame's checksum plays the header — fake frames that
  XOR-validate and self-sustain, wedging position-keyed rewrites forever
  (observed as `b1fwd=0xAA`). Detection needs the byte-1 invariant (panel
  byte 1 never carries bits 6/7); recovery needs a wire-gap-phased re-lock,
  because header-hunting from inside the alias re-locks the same phase.
  Fixed in cn2core::FramePos with host tests (fw 1.16.5).
