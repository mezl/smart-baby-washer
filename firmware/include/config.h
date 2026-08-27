#pragma once
// Momcozy D8 CN2 sniffer — build-time configuration.
// Secrets (WiFi + OTA password) live in secrets.h, which is gitignored.

#include "secrets.h"

#define FW_NAME     "d8-cn2sniffer"
#define FW_VERSION  "1.17.2"


// ---- Network identity ------------------------------------------------------
// mDNS name, so nothing has to track the DHCP lease. Both the browser UI and
// the OTA uploader follow the board instead of an address that moves.
#define OTA_HOSTNAME              "d8-sniffer"
#define OTA_PORT                  3232

#define WIFI_CONNECT_TIMEOUT_MS   20000UL   // stop *blocking* after this...
#define WIFI_RETRY_MS             10000UL   // ...then re-kick the supplicant
#define WIFI_REBOOT_AFTER_MS      60000UL   // ...and finally reboot, but only
                                            // if the CN2 link is idle too
// How quiet the panel link must be before a WiFi restart is allowed to take it
// down with it. Both ends broadcast every ~200 ms, so 3 s is fifteen missed
// frames -- by then there is no link left to protect.
#define WIFI_REBOOT_LINK_IDLE_MS  3000UL

// ---- Robustness ------------------------------------------------------------
// The task watchdog is deliberately slack. It exists to catch a wedged loop(),
// not to police latency — and an OTA flash write blocks loop() for seconds at a
// time. (net.cpp feeds it from the OTA progress callback so that is safe.)
#define WDT_TIMEOUT_MS            30000UL

// Boot-loop guard. Three resets without ever reaching a healthy uptime and we
// come up with the payload disabled and nothing running but WiFi + OTA.
#define SAFE_MODE_AFTER_BOOTS     3
#define HEALTHY_UPTIME_MS         30000UL   // uptime WITH WiFi that clears it

// ---- CN2 tap ---------------------------------------------------------------
// XIAO ESP32-C3 pin choice, see ../D8_REPAIR_SSOT.md §7b:
//   D1..D4 are plain GPIOs — no strapping function, no ROM boot chatter.
//   Avoided: GPIO2/8/9 (strapping; GPIO9 is also the BOOT button) and GPIO21,
//   which is UART0 TX by default and which the ROM bootloader transmits boot
//   messages on at every reset — wiring that to a line the main board listens
//   on would inject garbage into the panel link on every reset.
//
// UART0 faces the main board, UART1 faces the front panel.
//
// AS-WIRED on the bench (2026-08-06), through a 4-ch BSS138 level converter:
//
//   controller pin 1  (board TX, OUTPUT) -> HV1 -> LV1 -> D1 / GPIO3   RX
//   controller pin 2  (board RX, INPUT)  -> HV3 -> LV3 -> D2 / GPIO4   TX
//   panel      pin 1  (panel RX, INPUT)  -> HV2 -> LV2 -> D3 / GPIO5   TX
//   panel      pin 2  (panel TX, OUTPUT) -> HV4 -> LV4 -> D4 / GPIO6   RX
//
// The invariant that makes this safe: every ESP32 RX pin sits on a stub that is
// DRIVEN by the other end, and every ESP32 TX pin sits on a stub that is an
// INPUT at the other end. Two outputs on one wire is the only way to do damage.
// Straight-through: converter channel N carries pin DN, so the board and the
// converter line up one-to-one and there is nothing to mis-read. Any assignment
// works electrically -- the C3's GPIO matrix routes a UART's RX and TX to any
// pins -- so this is chosen for legibility, not necessity.
//
// These are only the DEFAULTS. /api/detect writes a discovered map to NVS, and
// a saved map wins over anything here.
#define PIN_RX_BOARD    3     // D1  <- controller pin 1  ch1  (controller TX)
#define PIN_TX_BOARD    4     // D2  -> controller pin 2  ch2  (controller RX)
#define PIN_TX_PANEL    5     // D3  -> panel pin 1       ch3  (panel RX)
#define PIN_RX_PANEL    6     // D4  <- panel pin 2       ch4  (panel TX)

// ---- flow meter, J3 on the carrier board -----------------------------------
// The board splices the FV signal line the same way CN2 is spliced: cut it,
// meter side to J3.1, controller side to J3.2. The ESP32 then sees every real
// pulse and decides what the controller is told.
//
// Both signals must come off the SAME side of the XIAO to be routable on a
// 2-layer board, and of the three GPIOs left free only GPIO10 (D10) and GPIO20
// (D7) are on the right-hand column -- GPIO7 (D5) is on the left. That is what
// moved FLOW_SIM off GPIO7.
//
// FLOW_SIM is OPEN-DRAIN on purpose: the hall sensor is open-collector with a
// 10k pull-up, so we only ever pull low and let that pull-up make the high
// level. Two open-drain outputs wire-OR, so the real sensor can stay connected.
#define PIN_FLOW_IN     10    // D10 <- J3.1, pulses from the real meter
#define PIN_FLOW_SIM    20    // D7  -> J3.2, what the controller sees

// Switch simulators. A switch input is normally pulled up by the board and
// shorted to ground when the switch closes, so OPEN-DRAIN low = "closed".
// Releasing the pin leaves the real switch in charge.
//
// Only GPIO7 is left, so SW2 (the lid) gets it and SW1 gets none. SW1 is the
// unidentified 3-pin switch; if you need it, it is still reachable on the
// XIAO's own header pin -- the module is socketed.
#define PIN_SW2_SIM     7     // D5  -> SW2 (lid switch) signal pin

// ---- wash-pump relay, external ---------------------------------------------
// The board's own low-side switch for WS PUMP does not close when panel b0 is
// commanded, so the pump is driven by an external relay module instead.
//
// D5/D7/D10 are declared above for the switch and flow simulators but are NOT
// WIRED on this machine, and -- this is the part that makes them safe to reuse
// -- none of them is touched at boot. simSet() and flowSim() each call pinMode()
// only when that feature is first used, and the flow input is only claimed by
// the /api/detect scan. So an unused simulator pin is genuinely idle, not
// merely unconnected.
//
// GPIO10 is the pick. It is the only free pin with no strapping role, no UART
// role and no boot-time behaviour at all:
//
//   GPIO9  (D9)  BOOT button. Low at reset = ROM download mode. Unusable.
//   GPIO2  (D0)  Strapping. Must be high at reset or SPI boot fails. Unusable.
//   GPIO21 (D6)  UART0 TX -- the ROM prints the boot log on it at every reset.
//   GPIO8  (D8)  Strapping for download boot; also blocks USB flashing if a
//                module holds it low. OTA would still work, but no need.
//   GPIO20 (D7)  UART0 RX. Not driven by the ROM, so usable, but taken.
//   GPIO7  (D5)  Plain GPIO -- the clean alternative if D10 is inconvenient.
//
// Cost of this choice: the flow-meter tap loses its input pin. That tap is a
// diagnostic; the pump is the machine's only means of washing. If the flow
// splice is ever built, move the relay to GPIO7 and set it over HTTP -- the pin
// is runtime-settable and persisted, so it costs a click, not a reflash.
#define PIN_WS_RELAY    10    // D10 -> relay module IN
//
// ⚠️ AN EXTERNAL PULL-DOWN IS NOT OPTIONAL. Between reset and cn2::begin() the
// pin is high-Z, and in SAFE MODE it is never configured at all. Whatever holds
// the module in its OFF state during those windows has to be passive:
//
//   active-HIGH module ->  10k from the relay pin to GND   (idle low  = open)
//   active-LOW  module ->  10k from the relay pin to 3V3   (idle high = open)
//
// Without it the pump can be energised by a crash, a reset, or a boot loop.
//
// This machine uses an ACTIVE-HIGH module, so the resistor goes to GND. Getting
// this backwards is the one wiring error here that is actually dangerous: a
// pull-UP on an active-high module drives the relay CLOSED for the whole of
// every boot, and closed again on any crash or boot loop.
#define WS_RELAY_ACTIVE_LOW   0
//
// The pump has no feedback of any kind, so the firmware cannot tell whether it
// is running -- these two limits are the only protection there is.
#define WS_RELAY_MAX_ON_MS    1800000UL  // 30 min hard cap, then latch open
#define WS_RELAY_LINK_DEAD_MS 3000UL     // no panel frame this long -> open

// ---------------------------------------------------------------------------
// Untargeted-flush cap
// ---------------------------------------------------------------------------
// The end-of-cycle cool-down rinse is panel byte 3 = 0xFF with the intake bit
// set, and it carries no volume target and no timer at EITHER end. What ends it
// is the water stopping: across the archive the same Self-Clean program ran it
// 64.1 s once and 114.6 s another time, and all three captured flushes end just
// after the sump temperature bottoms out and starts back up -- the signature of
// a hand-filled tank running dry. That makes the tank the only thing bounding
// it. Feed the tank from a float valve, or any always-on supply, and nothing
// does.
//
// So bound it here. Once a flush has run this long the forwarded byte 1 has the
// INTAKE bit cleared and the DRAIN bit left alone, which reproduces exactly the
// event the controller already terminates on rather than inventing a new one.
// The sump empties, the temperature rises, the cycle moves on.
//
// 180 s is 1.6x the longest flush ever observed (114.6 s) and longer than the
// cycle runner's own longest (116 s), so it cannot truncate normal operation.
// Settable at runtime via POST /api/flushcap; 0 disables.
// DEFAULT OFF. The cap assumed that stripping the intake would let the
// controller end the flush the way it always had -- water stops arriving, sump
// empties, stage completes. That assumption fails on a locked controller: it
// ends nothing, so the panel sat in one flush for 2131 s while the cap held the
// intake off, turning a stall into a stall with no water. Only enable it on a
// machine whose controller is known to honour the release.

// ---------------------------------------------------------------------------
// Hold WiFi off until the panel link is up and quiet
// ---------------------------------------------------------------------------
// Opening the UARTs before WiFi removed the 4 s blackout at power-on, but it
// also put relayTask on the air DURING association -- and association starves
// it. Measured on the first boot after that reorder: worst gap between pump()
// passes 95.9 ms, against 1 ms in steady state. 96 ms is eleven byte times at
// 9600 baud, wide enough to split a frame at whichever end is mid-transmission,
// and it lands squarely in the window the panel and controller use to find each
// other.
//
// So forward on a quiet link first, and only then bring up the radio. Ten good
// frames each way is two seconds at the 200 ms broadcast rate. The timeout is
// what stops a bench board with no machine attached waiting forever.
#define LINK_SETTLE_FRAMES      10
#define LINK_SETTLE_TIMEOUT_MS  5000UL

// How long phase 2 of the pin autodetect will starve the controller waiting for
// it to raise E5. It needs the fault ASSERTED to tell the two transmit
// permutations apart; without it the first one tried always "passes". Observed
// on one machine: the bit appeared ~47 s after a link interruption, so this is
// generous on purpose. Nothing is energised while it runs.
#define PHASE2_PROVOKE_MS       60000UL

// Consecutive frames the false-E5 filter must fail to disprove bit 6 before it
// stops masking. The controller broadcasts every 200 ms, so 25 frames is ~5 s
// -- long enough that no transient can latch E5 on the panel, short enough
// that a genuine sustained fault still reaches the display. Bit 6 LATCHES, so
// the asymmetry is deliberate: reporting late costs seconds, reporting a false
// positive costs the owner a power cycle.
#define E5_FILTER_DOUBT_FRAMES  25

// How long the intake bit may be commanded with the flow count frozen before
// the relay strips it. A full 90-count fill takes ~40 s at the measured
// 2.24 counts/s, so 120 s is three times the longest normal fill -- no honest
// fill can reach it, and nothing else in this machine will ever stop one.
// 0 disables. See cn2core::FillStall.
#define FILL_STALL_MS_DEFAULT   120000UL

// Heater ceiling for PANEL-run cycles. A captured steam phase on this machine
// reached 99 C, so anything at or below that would break normal operation --
// this is a runaway backstop, not a setpoint. Byte 1 is uncalibrated and reads
// LOW, so the real water temperature is higher than the number. 0 disables.
// Extra drain time for PANEL-run cycles, on top of the machine's own fixed
// stage. Measured on this install: a 90-count charge needs 160 s to clear a
// restricted line against a 28 s stage, so 132 s is the shortfall. 0 disables.
// Keep it close to what the line actually needs -- the drain pump runs dry for
// whatever time is left over.

// ---------------------------------------------------------------------------
// Stuck-load watchdog
// ---------------------------------------------------------------------------
// The controller locks out on status bit 6 and then ignores the panel's
// end-of-cycle release, leaving the blower and air heater energised -- observed
// holding 52-55 C for 88 minutes with the panel commanding nothing. Nothing on
// the link can stop that, so the plug is cut instead.
//
// The dwell has to be long enough to TELL THE TWO APART. A machine settling
// after a cycle cools at ~0.6 C/min, so 180 s of dwell yields only ~1.8 C of
// drop -- under the 2 C the peak comparison needs, meaning a merely-warm
// machine would have had its mains cut. 300 s yields ~3 C, which resolves
// cleanly, and is still five minutes against the 88 that were observed.
// 40 C floor because a cool machine is not worth cutting mains over.
// 0 disables.
#define STUCK_DWELL_MS_DEFAULT  300000UL
#define STUCK_HOT_C             40
#define STUCK_OFF_S             30

#define HEAT_CEILING_C          105
#define HEAT_RELEASE_C          95

// Unknown until the first scope capture. Changeable at runtime over HTTP and
// persisted in NVS, because guessing wrong should cost a click, not a reflash.
#define DEFAULT_BAUD    9600UL

// Ring buffer of captured bytes. 4096 × 8 B = 32 KB, comfortable next to WiFi.
#define SNIFF_RING      4096

// Bytes separated by more than this many character-times start a new frame.
// 3.5 is the Modbus idle-line convention and works well for the poll/response
// framing these appliance panels use.
#define FRAME_GAP_CHARS 3.5f
#define FRAME_GAP_MIN_US 400UL
