# Boot-path code review: why E5 latches at boot (Aug 27)

Requested as a code review; delivered as one, with the boot timeline audited
instruction-by-instruction and each stage marked with what it can and cannot
cause. The differential to explain: machine clean until Aug 26 15:31, E5 on
every module boot since 17:45, clean bare, latching behind a 20-line minimal
image.

## Timeline of a boot, with everything that executes

| t (approx) | what runs | reviewed finding |
|---|---|---|
| 0–300 ms | **ROM + 2nd-stage bootloader** — no app code | Pads GPIO4/5/6 are the C3's JTAG pins (MTMS/MTDI/MTCK) until the app claims them: internal ~45 k pulls (up on 4/6, down on 5) fight the shifter's 10 k pull-ups — worst case sags a line to ~2.7 V, still a valid HIGH. **Cannot pull a line low. Constant since day one — cannot explain a Wednesday-afternoon change.** |
| ~300 ms | `instantBridge()` — first app instructions | 15 register writes, geometry `in5→out3, in4→out6`, identical to the wireSet used through days of completed cycles. Verified conducting by edge counts. |
| ~1.5 s | `openPorts()` steals TX pads for the UARTs | Microsecond-scale glitch, then wire-pref decides re-bridge. **Irrelevant: the minimal image never steals the pads at all and still latches.** |
| 2 s → | relay/apps | **Irrelevant: absent from the minimal image.** |

## The exoneration ladder (each rung tested, not argued)

1. firmware logic — minimal image (bridge+WiFi+OTA, no logic in path): latches
2. firmware size/boot speed — 23 % smaller, zero-init: latches
3. every firmware version incl. same-day-working 1.16.3: latches
4. NVS state — full wipe to virgin: latches
5. wire vs CPU mode, masks, pin map (re-verified), GPIO10 relay drive (gated),
   cut lengths 5 s–10 min: latches
6. bytes on the wire — frame-level diff vs working log: identical
7. bare machine, no module: **clean**

## Verdict

Within the reviewable code there is no remaining instruction that executes
before or at the latch moment in the minimal case — the entire program fits
on one screen and touches the link only through six register writes whose
geometry carried days of complete wash cycles. The ROM stage is constant
silicon. **The failure differential (working 15:31 → broken 17:45, module-in
vs module-out) is not reachable by any code path**, and the honest remaining
suspects are physical: something in the module↔controller electrical path
changed that Wednesday afternoon (a component stressed during the heavy
cycle testing — the BSS138 channel, a solder joint, the CN2 connector
seating), below the resolution of every probe the firmware can host.

Decisive instruments remaining: (1) USB-power split test — ESP powered
independently, bridge continuous across machine boots — separates the ROM
dark-window hypothesis from analog presence; (2) a multimeter on the shifter
HV pins during a latch. Software's file on this bug is closed.
