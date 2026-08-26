#include "cn2.h"

#include <esp_task_wdt.h>

#include "kasa.h"
#include <esp_system.h>
#include <rom/rtc.h>
extern "C" uint64_t esp_rtc_get_time_us(void);
#include <ctype.h>
#include <cn2core.h>

#include <Preferences.h>
#include "esp_rom_gpio.h"
#include "soc/gpio_sig_map.h"
#include "soc/gpio_struct.h"
#include "hal/gpio_ll.h"
#include <esp_timer.h>

#include "config.h"

namespace cn2 {

struct Ev {
  uint32_t t_us;
  uint8_t  side;
  uint8_t  b;
};

static Ev       s_ring[SNIFF_RING];
static uint32_t s_head = 0;                 // monotonic write cursor
static uint32_t s_count[2] = {0, 0};
static uint32_t s_last_ms[2] = {0, 0};
static uint32_t s_overflow = 0;
static uint32_t s_tx[2] = {0, 0};
static esp_timer_handle_t s_flow_timer = nullptr;
static uint32_t s_flow_hz = 0;
static volatile uint32_t s_flow_pulses = 0;
static volatile bool s_flow_level = true;
static bool s_sim[2] = {false, false};
static uint8_t  s_done[2][24], s_donen[2] = {0, 0};
static volatile uint32_t s_seq[2] = {0, 0};   // seqlock guarding s_done/s_donen

// Copy the last complete frame from one side without tearing. Returns its
// length, or 0 if none has arrived yet or a writer kept winning the race.
// Everything reported over HTTP goes through here, so the decoded fields and the
// raw hex in the same response are always from the same frame.
static uint8_t frameSnapshot(uint8_t side, uint8_t *out) {
  if (side > 1) return 0;
  for (uint8_t tries = 0; tries < 8; tries++) {
    uint32_t a = s_seq[side];
    if (a & 1u) continue;                     // writer mid-copy
    uint8_t n = s_donen[side];
    if (n > 24) n = 24;
    memcpy(out, s_done[side], n);
    if (s_seq[side] == a) return n;           // nothing moved underneath us
  }
  return 0;
}
static uint8_t  s_lid_mode = 0;   // 0 = pass real, 1 = force LID ON, 2 = force LID OFF
static bool     s_lid_real = false;  // bit1 as RECEIVED from the controller
static bool     s_lid_fwd  = false;  // bit1 as SENT on to the panel
static uint8_t  s_st_clr = 0x00;     // byte3 bits force-CLEARED toward the panel
static uint8_t  s_st_set = 0x00;     // byte3 bits force-SET toward the panel
static uint8_t  s_st_real = 0, s_st_fwd = 0;
static bool     s_flow_spoof = false;          // hold the panel's flow count at 0
static uint8_t  s_flow_real = 0, s_flow_fwd = 0;
// VIRTUAL CONTROLLER. The ESP32 synthesises the controller->panel frame itself
// and the real controller's frames are dropped. The panel sees a healthy machine
// reporting whatever we choose; the real controller is fed the idle panel frame,
// so it stays in sync and E5-free but is never asked to do anything.
//
// This is the mirror of PROBE mode, which does the same trick in the other
// direction. Together they let the panel be exercised with the machine
// electrically inert.
static bool     s_virt = false;
static uint32_t s_virt_last = 0, s_virt_n = 0;
static uint8_t  s_virt_temp = 30, s_virt_flow = 0, s_virt_st = 0x00,
                s_virt_b5 = 0x0B;   // 0x0B is byte 5's most common value, 56%
// AUTO: run a simple model of the machine so the PANEL can complete a real
// cycle against it. Without this the panel commands a fill, the flow count never
// moves, and it stalls or faults -- which makes it impossible to reach the
// states where most error codes are evaluated.
//
// Every constant here is measured from captures/, not invented:
//   fill    2.24 counts/s      12 fills, 5 cycles
//   heat    0.103 C/s          24 -> 57 C in 321 s, wash cycle
//   cool    tau 459 s          90 -> 36 C over 13 min, storage hold
//   reset   ~1.5 s after the intake motor is released
static bool     s_virt_auto = true;
static float    s_virt_tempf = 30.0f, s_virt_flowf = 0.0f;
static uint32_t s_virt_sim_ms = 0, s_virt_intake_off = 0;

static int16_t  s_temp_ovr = -1;               // -1 = pass; else force byte 1
static uint8_t  s_temp_real = 0, s_temp_fwd = 0;
// Injected button press: OR'd into byte 1 of the panel->board frame for a while.
// Rewrite of byte 1 on its way panel->controller:  out = (b & ~clr) | set
//
// Byte 1 bit 5 (0x20) is the INTAKE MOTOR, not a RUN or pause flag. Setting it
// runs the pump; there is no gating of any kind. Two earlier readings of this
// bit — "pause", then "RUN" — were both wrong, and both came from short bench
// captures. Five full cycles settled it: the bit is on exactly while water is
// being drawn, and byte 2 counts pulses throughout.
static uint8_t  s_p1_clr = 0, s_p1_set = 0;
// Byte 3 is the FILL TARGET for the next fill; byte 2 always accompanies it and
// has never been seen to vary independently. Substituting them changes how much
// water a fill draws. It does NOT start a cycle — that was tested and ruled out,
// and there is no RUN flag for the controller to assert.
static int16_t  s_p2_ovr = -1, s_p3_ovr = -1;
static uint8_t  s_press_mask = 0;
static uint32_t s_press_until = 0;
static uint8_t  s_pb_i = 0, s_pb_x = 0;
static cn2core::FrameTx s_pb_tx;    // checksum decision, panel->controller
static cn2core::FramePos s_pb_pos;
static uint32_t s_pb_prev = 0;
static uint8_t  s_panel_b1 = 0, s_panel_b2 = 0, s_panel_b3 = 0;
// Byte 1 as FORWARDED, i.e. what the controller is actually told to energise.
// Differs from s_panel_b1 whenever an override or the cycle runner is driving:
// the real panel sends 0x00 and the command exists only in the rewritten stream.
static uint8_t  s_panel_b1_fwd = 0;
// Byte 1 as the overrides WANT it, before the flush cap subtracts the intake
// bit. The cap has to latch off this value, not off s_panel_b1_fwd: the moment
// it strips b5 the condition it triggered on would read false, un-latch, and
// the pair would oscillate at the frame rate.
static uint8_t  s_panel_b1_want = 0;
// Byte 3 as forwarded, so the cap sees the target the CONTROLLER is acting on
// rather than the one the panel asked for.
static uint8_t  s_panel_b3_fwd = 0;
// ---- untargeted-flush cap (see FLUSH_CAP_MS_DEFAULT in config.h) ----------
static cn2core::FillStall s_fstall;   // panel-run fill guard; see cn2core
static cn2core::HeatCeiling s_heat;   // panel-run heater backstop
static cn2core::StuckLoad   s_stuck;  // mains cut-out; see cn2core::StuckLoad
static uint16_t s_stuck_off_s = 30;
static uint8_t  s_e5f_mode = E5F_AUTO;   // false-E5 filter; see e5FilterActive()
static uint32_t s_wifi_delay = 0;   // radio hold at boot; see setWifiDelayMs()
// PROBE mode: the controller is fed a permanently idle panel frame while the
// real panel's bytes are still captured. Buttons can then be pressed freely to
// learn their codes without any of them reaching the controller.
static bool     s_probe = false;
static uint8_t  s_b1_last = 0;      // most recent NON-ZERO byte 1 seen
static uint32_t s_b1_at = 0;
static uint32_t s_b1_n = 0;

// ---- delta detector -------------------------------------------------------
// Snapshot an "idle" frame per direction, then record every DISTINCT frame that
// differs from it. Answers "did anything at all change while I did X?" without
// staring at a scrolling log.
// Per-direction run-length history: an identical repeat only bumps the count and
// the last-seen time; anything different appends a row that then persists. The
// whole session's distinct frames stay on screen instead of scrolling away.
struct Hist { uint8_t n, b[12]; uint32_t first_ms, last_ms, count; };
#define HIST_MAX 24
// 0 = RX from controller   1 = RX from panel
// 2 = TX to panel           3 = TX to controller   (i.e. after any rewrite)
static Hist    s_hist[4][HIST_MAX];
static uint8_t s_histn[4] = {0, 0, 0, 0};

// Assemble the frames we actually SEND, so the UI can show received vs
// transmitted side by side and any override is visible rather than inferred.
static void histAdd(uint8_t side, const uint8_t *f, uint8_t n);
static cn2core::Assembler s_asm_out[2];

static inline void collectOut(uint8_t dir, uint8_t b) {
  uint8_t n = s_asm_out[dir].feed(b);
  if (n) histAdd(2 + dir, s_asm_out[dir].buf, n);
}

static void histAdd(uint8_t side, const uint8_t *f, uint8_t n) {
  if (n > 12) n = 12;
  uint8_t &c = s_histn[side];
  if (c) {
    Hist &last = s_hist[side][c - 1];
    if (last.n == n && !memcmp(last.b, f, n)) {     // same as last -> just tick
      last.count++; last.last_ms = millis(); return;
    }
  }
  if (c >= HIST_MAX) {                              // full: drop the oldest
    memmove(&s_hist[side][0], &s_hist[side][1], sizeof(Hist) * (HIST_MAX - 1));
    c = HIST_MAX - 1;
  }
  Hist &h = s_hist[side][c++];
  h.n = n; memcpy(h.b, f, n);
  h.first_ms = h.last_ms = millis(); h.count = 1;
}

struct Delta { uint8_t side, n, b[12]; uint32_t first_ms, count; };
static Delta   s_dl[10];
static uint8_t s_dln = 0;
static uint8_t s_base[2][12], s_basen[2] = {0, 0};

static void deltaCheck(uint8_t side, const uint8_t *f, uint8_t n) {
  if (!s_basen[side]) return;                       // no baseline yet
  if (n == s_basen[side] && !memcmp(f, s_base[side], n)) return;   // matches idle
  for (uint8_t i = 0; i < s_dln; i++)
    if (s_dl[i].side == side && s_dl[i].n == n && !memcmp(s_dl[i].b, f, n)) {
      s_dl[i].count++; return;
    }
  if (s_dln >= 10) return;                          // table full, keep the first 10
  Delta &d = s_dl[s_dln++];
  d.side = side; d.n = (n > 12) ? 12 : n;
  memcpy(d.b, f, d.n); d.first_ms = millis(); d.count = 1;
}
static uint8_t  s_bp_i = 0;             // index within the current board frame
// The checksum decision for the controller->panel direction. cn2core owns it
// so it is exercised on the host; see cn2core::FrameTx.
static cn2core::FrameTx s_bp_tx;

// PURE mode and the edit counters answer one question: are we a wire?
//
// "Byte-perfect" is worth nothing as a claim about the code, because the claim
// goes stale the next time a rewrite is added -- that is exactly how the stale
// checksum bug happened. So it is measured instead, at the single point where a
// byte leaves us: compare what we are about to emit against what we received.
// That catches every rewrite path, including ones not yet written.
//
// s_pure forces the emitted byte back to the received byte at that same point,
// so it disables all rewriting in both directions regardless of which feature
// asked for it, now or later.
static bool     s_pure   = false;
static uint32_t s_lock_ms = 0;      // millis when bit 6 was first seen held
static uint32_t s_edit_c = 0;   // bytes we changed, controller->panel
static uint32_t s_edit_p = 0;   // bytes we changed, panel->controller
static cn2core::FramePos s_bp_pos;   // byte index, counted not timed
static uint8_t  s_bp_x = 0;             // XOR of bytes we have actually SENT
static uint32_t s_bp_prev = 0;   // bytes actually written out: [0]=to board, [1]=to panel

static uint32_t s_baud    = DEFAULT_BAUD;
static bool     s_open    = false;
static bool     s_capture = true;

static Preferences s_prefs;

// The panel's idle "nothing pressed" frame, captured on the wire.
static const uint8_t PANEL_IDLE[5] = {0xAA, 0x00, 0x00, 0x00, 0xAA};
static uint8_t  s_spoof_fr[16] = {0xAA, 0x00, 0x00, 0x00, 0xAA};
static uint8_t  s_spoof_len = 5;
static bool     s_spoof = false;
static uint32_t s_spoof_ms = 200;      // matches the observed ~200 ms poll
static uint32_t s_spoof_last = 0;
static uint32_t s_spoof_n = 0;
static TaskHandle_t s_task = nullptr;
static void relayTask(void *);   // defined below; started from begin()

static uint8_t  s_wsr_mode  = WSR_AUTO;
static bool     s_wsr_on    = false;
static bool     s_wsr_low   = (WS_RELAY_ACTIVE_LOW != 0);
static int8_t   s_wsr_pin   = PIN_WS_RELAY;
static bool     s_wsr_lock  = false;      // hit the cap; waits for the command to drop
static uint32_t s_wsr_since = 0;
static uint32_t s_wsr_n     = 0;
static const char *s_wsr_why = "boot";

static void wsrIdlePin(int8_t pin);
static void wsrService();
static void wsrDrive(bool on, const char *why);
static void pump();
static void wsrPanelFrame();
static void flushFrame();
static void closePorts();   // ordered teardown; see the definition

// ---------------------------------------------------------------------------
// TREND BUFFERS
//
// The page cannot look back 30 minutes on its own, so the history lives here.
// Both are 1 Hz rings, which is plenty: the temperature byte moves a count every
// few seconds and the flow counter about twice a second.
//
// Temperature and heater state share one byte. The temperature has never been
// seen above 97, so bit 7 is free and carries "water heater commanded" -- one
// buffer instead of two, and the pair can never drift out of step.
#define G_TEMP_N 1800            // 30 minutes at 1 Hz
#define G_FLOW_N   60            // 60 seconds at 1 Hz
static uint8_t  s_g_temp[G_TEMP_N];
// Bit 7 of s_g_temp carries the WATER heater. The air heater needs its own lane
// and the temperature already uses seven bits, so it gets a packed bitmap --
// 1 bit per sample, 225 bytes, rather than another 1800-byte array.
static uint8_t  s_g_air[(G_TEMP_N + 7) / 8];
static uint8_t  s_g_flow[G_FLOW_N];
static uint16_t s_g_temp_head = 0, s_g_temp_len = 0;
static uint8_t  s_g_flow_head = 0, s_g_flow_len = 0;
static uint32_t s_g_last_ms = 0;
// The setpoint is not in the protocol. What we can see is the temperature at the
// moment the heater is released, so that is what gets drawn -- labelled as the
// observed cut-out rather than as a target the machine told us.
static uint8_t  s_g_cutout = 0;
static bool     s_g_heat_prev = false;
static uint32_t s_g_flow_move_ms = 0;   // last time the pulse count changed
static uint8_t  s_g_flow_prev = 0;

// EVERY CHANGE OF THE FLOW COUNT, captured where the frames land.
//
// Polling this over HTTP cannot be complete: the controller broadcasts at 5 Hz
// and a 5 Hz poller has no headroom, so a single stalled request skips a count.
// A real fill measured 91 of 100 captured that way. Recording the change here
// removes the network from the path entirely -- 256 events covers a fill of ~100
// counts twice over.
struct FlowEv { uint32_t ms; uint8_t count; };
static FlowEv   s_fev[256];
static uint8_t  s_fev_head = 0;
static uint16_t s_fev_len  = 0;
static uint8_t  s_fev_prev = 0;
static bool     s_fev_have = false;

static inline void flowEvent(uint8_t count) {
  if (s_fev_have && count == s_fev_prev) return;
  s_fev_prev = count; s_fev_have = true;
  s_fev[s_fev_head] = { millis(), count };
  s_fev_head = (uint8_t)((s_fev_head + 1) % 256);
  if (s_fev_len < 256) s_fev_len++;
}
static uint32_t s_late_us = 0;      // worst observed gap between pump() passes
static uint32_t s_late_at = 0;      // millis() when that worst gap was recorded

// On the C3 with a USB-CDC console, both hardware UARTs are free. Serial is the
// USB device; Serial0 is hardware UART0.
static HardwareSerial &uBoard = Serial0;
static HardwareSerial &uPanel = Serial1;

// The CN2 pin map is runtime state, not a compile-time constant, so autodetect
// can rewrite it. config.h supplies the defaults; NVS overrides them if a scan
// has been run and saved.
static int8_t s_pin_rxb = PIN_RX_BOARD, s_pin_txb = PIN_TX_BOARD;
static int8_t s_pin_txp = PIN_TX_PANEL, s_pin_rxp = PIN_RX_PANEL;

// WIRE mode: the two CN2 signal paths are bridged pad-to-pad inside the GPIO
// matrix (SIG_IN_FUNC 97/98 -- the C3's dedicated pad-bypass signals), so
// forwarding happens in silicon with nanosecond latency and NO dependence on
// the CPU, FreeRTOS, WiFi or flash stalls. The UARTs keep LISTENING on the
// same pads (an input pad can feed any number of matrix signals), so decode,
// graphs, HA and the lockout webhook all still work. What stops working is
// everything that rewrites: overrides, the E5 mask, the cycle runner -- their
// TX bytes go to a detached signal and vanish.
//
// Why this exists: on 2026-08-23 the touch-panel unit's controller was proved
// to lock BECAUSE of the module's presence (machine ran clean the moment the
// ESP32 was unplugged). The data path was byte-perfect the whole time; the
// suspect is store-and-forward DELIVERY -- a stalled task bunches panel
// frames, and this controller latches its starvation bit where the old one
// only flashed E5. The comment in openPorts() about FIFO batching pushing
// the panel outside the response window was the same lesson, milder.
static bool s_wire = false;

// ---- relay-path profiling -------------------------------------------------
// Question under test: what stalls the forwarding task, how often, for how
// long, and who is to blame. The histogram is the victim's view (gaps between
// relayTask passes); the per-cause timers are the suspects' view (how long
// each suspect operation actually ran). Suspects are timed at their call
// sites: HTTP request handling, NVS flush, the lockout webhook.
//
// Buckets: <2ms, 2-5, 5-10, 10-20, 20-50, 50-100, >100ms.
struct ProfCause { uint32_t n = 0; uint32_t max_us = 0; uint64_t total_us = 0; };
static uint32_t  s_prof_hist[7] = {0};
static uint32_t  s_prof_passes = 0;
static uint32_t  s_prof_rxmax  = 0;      // deepest RX backlog seen at pump entry
static ProfCause s_prof_http, s_prof_nvs, s_prof_hook;

void profNote(uint8_t which, uint32_t us) {
  ProfCause *c = which == 0 ? &s_prof_http : which == 1 ? &s_prof_nvs : &s_prof_hook;
  c->n++; c->total_us += us; if (us > c->max_us) c->max_us = us;
}
void profRx(uint32_t depth) { if (depth > s_prof_rxmax) s_prof_rxmax = depth; }
static inline void profGap(uint32_t us) {
  s_prof_passes++;
  int b = us < 2000 ? 0 : us < 5000 ? 1 : us < 10000 ? 2 : us < 20000 ? 3
        : us < 50000 ? 4 : us < 100000 ? 5 : 6;
  s_prof_hist[b]++;
}
void profReset() {
  memset(s_prof_hist, 0, sizeof(s_prof_hist));
  s_prof_passes = 0; s_prof_rxmax = 0;
  s_prof_http = ProfCause(); s_prof_nvs = ProfCause(); s_prof_hook = ProfCause();
  s_late_us = 0;
}
uint32_t profPasses()        { return s_prof_passes; }
uint32_t profHist(int b)     { return s_prof_hist[b]; }
uint32_t profRxMax()         { return s_prof_rxmax; }
void profCause(uint8_t w, uint32_t &n, uint32_t &mx, uint32_t &tot) {
  ProfCause *c = w == 0 ? &s_prof_http : w == 1 ? &s_prof_nvs : &s_prof_hook;
  n = c->n; mx = c->max_us; tot = (uint32_t)(c->total_us / 1000);
}

int8_t pinRxBoard() { return s_pin_rxb; }
int8_t pinTxBoard() { return s_pin_txb; }
int8_t pinRxPanel() { return s_pin_rxp; }
int8_t pinTxPanel() { return s_pin_txp; }
bool wire() { return s_wire; }

// Bridge the pads at the EARLIEST possible moment of boot -- before NVS-heavy
// init, before the UARTs, long before WiFi. The boot logs recorded two
// lockouts that happened seconds after power-on: the controller starves while
// this firmware boots, because until something forwards, the link is dark.
// Every millisecond shaved here is starvation the controller never sees.
//
// Reads only NVS (fast) to respect a saved LISTEN map (tx pins -1 => drive
// nothing) and the wire preference. openPorts() later hands the TX pads to
// the UARTs for about a millisecond; wireSet(true) immediately re-bridges.
void earlyBridge() {
  Preferences p;
  p.begin("d8link", true);
  const int8_t rxb = (int8_t)p.getChar("prxb", PIN_RX_BOARD);
  const int8_t txb = (int8_t)p.getChar("ptxb", PIN_TX_BOARD);
  const int8_t txp = (int8_t)p.getChar("ptxp", PIN_TX_PANEL);
  const int8_t rxp = (int8_t)p.getChar("prxp", PIN_RX_PANEL);
  const bool   w   = p.getBool("wire", true);
  p.end();
  if (!w || rxb < 0 || txb < 0 || txp < 0 || rxp < 0) return;
  gpio_ll_input_enable(&GPIO, (uint32_t)rxb);
  gpio_ll_input_enable(&GPIO, (uint32_t)rxp);
  esp_rom_gpio_connect_in_signal(rxb, SIG_IN_FUNC_97_IDX, false);
  esp_rom_gpio_connect_out_signal(txp, SIG_IN_FUNC_97_IDX, false, false);
  esp_rom_gpio_connect_in_signal(rxp, SIG_IN_FUNC_98_IDX, false);
  esp_rom_gpio_connect_out_signal(txb, SIG_IN_FUNC_98_IDX, false, false);
  GPIO.func_out_sel_cfg[txp].oen_sel = 1;
  GPIO.func_out_sel_cfg[txb].oen_sel = 1;
  GPIO.enable_w1ts.enable_w1ts = (1UL << txp) | (1UL << txb);
  Serial.println("[link ] early bridge: pads wired before init");
}

bool wireSet(bool on) {
  if (s_pin_txb < 0 || s_pin_txp < 0) return false;   // LISTEN drives nothing
  if (on == s_wire) return true;
  if (on) {
    // controller out -> panel-in pad, panel out -> controller-in pad
    esp_rom_gpio_connect_in_signal(s_pin_rxb, SIG_IN_FUNC_97_IDX, false);
    esp_rom_gpio_connect_out_signal(s_pin_txp, SIG_IN_FUNC_97_IDX, false, false);
    esp_rom_gpio_connect_in_signal(s_pin_rxp, SIG_IN_FUNC_98_IDX, false);
    esp_rom_gpio_connect_out_signal(s_pin_txb, SIG_IN_FUNC_98_IDX, false, false);
    // Output enable from the GPIO enable register, not the (now absent)
    // peripheral -- the bridge signals carry data only.
    GPIO.func_out_sel_cfg[s_pin_txp].oen_sel = 1;
    GPIO.func_out_sel_cfg[s_pin_txb].oen_sel = 1;
    GPIO.enable_w1ts.enable_w1ts = (1UL << s_pin_txp) | (1UL << s_pin_txb);
  } else {
    // Hand the TX pads back to the UARTs -- BY FORCE. setPins() was the
    // original implementation here and it is a silent no-op when the pin
    // numbers have not changed: the core's peripheral bookkeeping still
    // records the pins as attached to these UARTs, so it "skips" the matrix
    // reattach it doesn't know the bridge stole. The consequence was a
    // twelve-hour ghost hunt: every "CPU relay" session after the first
    // wire engagement still forwarded through the bridge, while every
    // override -- intake fills byte-identical to the panel's, lid forcing,
    // the cycle runner's stages -- was written into a detached UART and
    // never touched the wire. Reconnect the TX signals explicitly.
    GPIO.func_out_sel_cfg[s_pin_txp].oen_sel = 0;
    GPIO.func_out_sel_cfg[s_pin_txb].oen_sel = 0;
    esp_rom_gpio_connect_out_signal(s_pin_txb, U0TXD_OUT_IDX, false, false);  // uBoard = Serial0
    esp_rom_gpio_connect_out_signal(s_pin_txp, U1TXD_OUT_IDX, false, false);  // uPanel = Serial1
  }
  s_wire = on;
  // A LOCAL handle, deliberately. Several setters do s_prefs.begin()/end()
  // on the shared handle, and after any of them runs, writes through s_prefs
  // fail silently -- which is how "wire off" kept un-persisting across
  // reboots and the board came up bridged against Kai's standing directive.
  { Preferences p; p.begin("d8link", false); p.putBool("wire", on); p.end(); }
  Serial.printf("[link ] %s\n", on ? "WIRE: pads bridged in the GPIO matrix"
                                    : "CPU relay: UARTs own the TX pads");
  return true;
}

static void openPorts() {
  int8_t txBoard = s_pin_txb;
  int8_t txPanel = s_pin_txp;

  uBoard.setRxBufferSize(1024);
  uPanel.setRxBufferSize(1024);
  uBoard.begin(s_baud, SERIAL_8N1, s_pin_rxb, txBoard);
  uPanel.begin(s_baud, SERIAL_8N1, s_pin_rxp, txPanel);

  // Latency, not throughput, is what matters for a relay. By default the UART
  // driver only moves bytes out of the hardware FIFO once ~120 have arrived or
  // an idle timeout expires — so a whole 8-byte frame sits in the FIFO before
  // available() ever reports it. That batching is what pushed the panel's reply
  // outside the board's response window and made E5 flash.
  //
  // Interrupt on EVERY byte and use the shortest idle timeout instead. At
  // 9600 baud that is ~960 interrupts/s, which is nothing.
  uBoard.setRxFIFOFull(1);
  uPanel.setRxFIFOFull(1);
  uBoard.setRxTimeout(1);
  uPanel.setRxTimeout(1);

  s_open = true;

  Serial.printf("[link ] relay @ %lu 8N1   board rx=GPIO%d tx=GPIO%d"
                "   panel rx=GPIO%d tx=GPIO%d\n",
                (unsigned long)s_baud, (int)s_pin_rxb, (int)txBoard,
                (int)s_pin_rxp, (int)txPanel);
}

// RTC_NOINIT survives esp_restart(); so does the RTC clock, which is the only
// timebase that spans a reboot.
static RTC_NOINIT_ATTR uint64_t s_ota_mark_us;
static RTC_NOINIT_ATTR uint32_t s_ota_magic;
static uint32_t s_ota_gap_ms = 0;
static const uint32_t OTA_MAGIC = 0x0D8A5A11;

uint32_t lastOtaGapMs() { return s_ota_gap_ms; }

void markOtaStart() {
  s_ota_mark_us = esp_rtc_get_time_us();
  s_ota_magic   = OTA_MAGIC;
}

// Forward whatever has arrived, right now, from whichever task is calling.
// pump() is re-entrant only in the sense that relayTask is the usual caller;
// during an OTA relayTask is being starved by flash writes, and the point is
// to hand the UART something to clock out while the cache is off.
void serviceNow() { if (s_open) pump(); }

void begin() {
  s_prefs.begin("d8link", false);
  s_baud = s_prefs.getULong("baud", DEFAULT_BAUD);
  s_pin_rxb = (int8_t)s_prefs.getChar("prxb", PIN_RX_BOARD);
  s_pin_txb = (int8_t)s_prefs.getChar("ptxb", PIN_TX_BOARD);
  s_pin_txp = (int8_t)s_prefs.getChar("ptxp", PIN_TX_PANEL);
  s_pin_rxp = (int8_t)s_prefs.getChar("prxp", PIN_RX_PANEL);
  s_wsr_pin  = (int8_t)s_prefs.getChar("wsrp", PIN_WS_RELAY);
  s_wsr_low  = s_prefs.getBool("wsrl", WS_RELAY_ACTIVE_LOW != 0);
  s_wsr_mode = s_prefs.getUChar("wsrm", WSR_AUTO);
  if (s_wsr_mode > WSR_AUTO) s_wsr_mode = WSR_AUTO;
  s_fstall.stall_ms = s_prefs.getULong("fstall", FILL_STALL_MS_DEFAULT);
  s_heat.ceiling_c  = s_prefs.getUChar("hceil", HEAT_CEILING_C);
  s_stuck.dwell_ms  = s_prefs.getULong("stkms", STUCK_DWELL_MS_DEFAULT);
  s_stuck.hot_c     = s_prefs.getUChar("stkc", STUCK_HOT_C);
  s_stuck_off_s     = (uint16_t)s_prefs.getUShort("stkoff", STUCK_OFF_S);
  s_heat.release_c  = HEAT_RELEASE_C;
  s_wifi_delay  = s_prefs.getULong("wifid", 0);
  s_e5f_mode    = s_prefs.getUChar("e5f", E5F_AUTO);
  if (s_e5f_mode > E5F_FORCE) s_e5f_mode = E5F_AUTO;
  s_prefs.end();
  // Before anything else can command it. Until this runs the pin is high-Z and
  // the external pull resistor is the only thing holding the pump off.
  wsrIdlePin(s_wsr_pin);
  Serial.printf("[wsr  ] wash-pump relay on GPIO%d, active-%s, mode %u\n",
                (int)s_wsr_pin, s_wsr_low ? "LOW" : "HIGH", s_wsr_mode);
  // How long the panel was starved across the last update, reboot included.
  if (s_ota_magic == OTA_MAGIC) {
    s_ota_magic = 0;
    const uint64_t now = esp_rtc_get_time_us();
    s_ota_gap_ms = (now > s_ota_mark_us) ? (uint32_t)((now - s_ota_mark_us) / 1000) : 0;
    Serial.printf("[ota  ] panel starved %lu ms across the update%s\n",
                  (unsigned long)s_ota_gap_ms,
                  s_ota_gap_ms > 1000 ? "  — expect a latched E5" : "");
  }
  cycleLoad();                 // stage durations survive a reboot
  openPorts();
  // Wire is the boot DEFAULT (NVS-overridable): the machine's link comes up
  // as silicon before WiFi is even started, so nothing this firmware does --
  // boot, OTA, HTTP, a crash -- can starve the controller of panel frames.
  // Read through a LOCAL handle. The shared s_prefs read returned the
  // default (true) here even when NVS held false -- and because wireSet(true)
  // PERSISTS what it applies, every boot then overwrote the stored false and
  // the board kept coming up bridged against the standing directive. The
  // local handle reads the flash truthfully (verified: stored:false survives
  // until this line, and is true again right after boot).
  bool wire_pref;
  { Preferences p; p.begin("d8link", true); wire_pref = p.getBool("wire", true); p.end(); }
  if (wire_pref) wireSet(true);
  if (!s_task) {
    xTaskCreate(relayTask, "cn2relay", 3072, nullptr, 10, &s_task);
    Serial.println("[cn2  ] relay task started (priority 10)");
  }
}

void quiesce() {
  // Deliberately does NOT call end(): with the UART closed the TX pin floats,
  // and a floating RX at the panel reads as a break condition. Idle-high is the
  // safe thing to leave behind while the flash is being written.
  s_capture = false;
  // An OTA write blocks this task for seconds. Whatever b0 was doing, the pump
  // must not be left running across a flash.
  wsrDrive(false, "OTA in progress");
  Serial.println("[link ] capture paused (UARTs left open, TX idle-high)");
}


void setBaud(uint32_t baud) {
  if (baud < 300 || baud > 1000000) return;
  s_baud = baud;
  s_prefs.begin("d8link", false);
  s_prefs.putULong("baud", s_baud);
  s_prefs.end();
  if (s_open) {
    uBoard.end();
    uPanel.end();
    openPorts();
  }
}

static uint32_t frameGapUs();   // defined below

// LINK QUALITY. Every frame ends in an XOR of the bytes before it, so a bad
// checksum means a byte was corrupted between the far MCU and us. This is the
// number that matters when re-terminating the CN2 stubs or moving a stub onto a
// different level-shifter channel: a marginal channel does not fail outright,
// it drops the occasional bit, and "it still works" hides that completely.
static uint32_t s_ok[2] = {0, 0}, s_bad[2] = {0, 0};

// TX MARGIN TEST. The ESP cannot see its own output, so a level-shifter channel
// that is merely weak looks identical to one that is perfect — right up until it
// isn't. What we can do is starve the far end deliberately: forward only 1 frame
// in N and find the N at which it gives up and raises E5. That threshold is a
// direct measure of how many of the frames we think we are sending actually land.
// Baseline it on known-good wiring, then compare after rewiring; a channel that
// silently drops frames will fail at a lower N than it used to.
static cn2core::Thinner s_thin_panel, s_thin_ctrl;

// Frame assembly by HEADER + LENGTH, not by idle gap.
//
// Gap-based delimiting looked fine until the relay task got preempted: the
// timestamps come from when the task *reads* a byte, so any scheduling delay
// longer than the gap threshold (~3.6 ms at 9600) splits one frame into two.
// Worst-case task gaps were measured at 7-8 ms, so frames were being chopped up
// and showing as phantom "changes".
//
// Both frame types are fixed: 0xA2 -> 8 bytes, 0xAA -> 5. Delimiting on that is
// immune to jitter. A byte lost on the wire desyncs us, but the next unexpected
// header resyncs within one frame.
static cn2core::Assembler s_asm[2];

static inline void collect(uint8_t side, uint8_t b, uint32_t t_us) {
  // Resync the assembler on a real gap.
  //
  // Header+length delimiting alone can lock onto the WRONG byte and never
  // recover, because this protocol is self-similar: the panel's idle frame is
  // AA 00 00 00 AA, so starting one byte late yields [AA,AA,00,00,00] -- whose
  // XOR is 0x00, so it VALIDATES, and it re-locks identically every frame.
  // Observed live: 386 consecutive "valid" frames reporting byte 1 = 0xAA,
  // which would mean drain + air heater + intake on an idle machine.
  //
  // Frames are ~200 ms apart and five bytes take ~5 ms, so an inter-byte gap
  // this large is unambiguously a frame boundary. This does not reintroduce
  // gap-based delimiting -- length still decides where a frame ends. It only
  // says where one may begin.
  static uint32_t s_last_us[2] = {0, 0};
  if (s_last_us[side] && (uint32_t)(t_us - s_last_us[side]) > 20000u)
    s_asm[side].reset();
  s_last_us[side] = t_us;
  uint8_t n = s_asm[side].feed(b);
  if (!n) return;
  const uint8_t *fr = s_asm[side].buf;
  s_ok[side]  = s_asm[side].ok;
  s_bad[side] = s_asm[side].bad;
  // Seqlock: odd while the copy is in flight. relayTask writes this; the web
  // task reads it. Without the fence a reader could take bytes from two
  // different frames -- that really happened, and it put phantom "bit 6 set"
  // rows into three cycle captures before the raw frames gave it away.
  s_seq[side]++;
  memcpy(s_done[side], fr, n);
  s_donen[side] = n;
  s_seq[side]++;
  if (side == 1) { wsrPanelFrame(); flushFrame(); }   // whole frames only
  if (side == 0 && n >= 8) {
    // Stuck-load watchdog. Reads the CONTROLLER's own bytes and the panel's
    // real byte 1 -- never the forwarded copies, so masking bit 6 for the
    // panel does not blind the thing that cuts mains.
    if (s_stuck.frame(s_panel_b1, fr[3], (uint8_t)(fr[1] & 0x7F), millis())) {
      Serial.printf("[stuck] panel idle, bit 6 set, sump %u C not falling for %lu s"
                    " — cutting mains\n", (unsigned)(fr[1] & 0x7F),
                    (unsigned long)(s_stuck.dwell_ms / 1000));
      kasa::powerOff();
    }
  }
  if (side == 0 && n >= 8) flowEvent(fr[2]);   // byte 2, every change
  deltaCheck(side, fr, n);
  histAdd(side, fr, n);
}

static inline void push(uint8_t side, uint8_t b) {
  Ev &e = s_ring[s_head % SNIFF_RING];
  e.t_us = micros();
  e.side = side;
  e.b    = b;
  collect(side, b, e.t_us);
  s_head++;
  s_count[side]++;
  s_last_ms[side] = millis();
}

static void virtSim() {
  uint32_t now = millis();
  if (!s_virt_sim_ms) { s_virt_sim_ms = now; return; }
  float dt = (now - s_virt_sim_ms) / 1000.0f;
  s_virt_sim_ms = now;
  if (dt <= 0 || dt > 5) return;                 // clock jump: skip a step
  const uint8_t p = s_panel_b1;

  if (p & 0x20) {                                // intake motor commanded
    s_virt_flowf += 2.24f * dt;
    s_virt_intake_off = 0;
  } else if (!s_virt_intake_off) {
    s_virt_intake_off = now;
  } else if (now - s_virt_intake_off > 1500) {
    s_virt_flowf = 0;                            // counter resets after the fill
  }
  if (s_virt_flowf > 255.0f) s_virt_flowf = 255.0f;

  if (p & 0x04)      s_virt_tempf += 0.103f * dt;             // water heater
  else if (p & 0x18) s_virt_tempf += 0.020f * dt;             // air heater / dry
  else               s_virt_tempf -= (s_virt_tempf - 24.0f) / 459.0f * dt;
  if (s_virt_tempf > 97.0f) s_virt_tempf = 97.0f;
  if (s_virt_tempf < 20.0f) s_virt_tempf = 20.0f;

  s_virt_temp = (uint8_t)(s_virt_tempf + 0.5f);
  s_virt_flow = (uint8_t)s_virt_flowf;
}

// ---- false-E5 filter (see cn2core::E5Filter) -------------------------------

static bool     s_e5f_on    = false;      // masking at this instant
static uint32_t s_e5f_n     = 0;
static const char *s_e5f_why = "idle";

static cn2core::E5Filter s_e5f;
static uint32_t s_e5f_leaks = 0;    // frames where bit 6 reached the panel

static bool e5FilterActive() {
  if (s_e5f_mode == E5F_OFF)   { s_e5f_why = "off";   return s_e5f_on = false; }
  if (s_e5f_mode == E5F_FORCE) { s_e5f_why = "forced"; return s_e5f_on = true; }
  cn2core::E5Filter &f = s_e5f;
  f.transparent = s_thin_panel.every == 1 && s_thin_ctrl.every == 1 &&
                  !s_probe && !s_spoof && !s_virt;
  f.board_fresh = lastByteAgeMs(FROM_BOARD) < 1000;
  f.panel_fresh = lastByteAgeMs(FROM_PANEL) < 1000;
  f.clean       = (s_bad[0] == 0 && s_bad[1] == 0);
  s_e5f_on = f.mask(E5_FILTER_DOUBT_FRAMES);
  const char *doubtwhy =
        !f.transparent ? "not a transparent relay — bit 6 is earned"
      : !f.board_fresh ? "controller frames stale"
      : !f.panel_fresh ? "panel frames stale"
                       : "bad checksums seen — cannot disprove";
  s_e5f_why = f.doubt == 0 ? "link verified healthy"
            : s_e5f_on     ? doubtwhy      // doubting, still holding the mask
                           : doubtwhy;     // gave up: bit 6 now reaches the panel
  if (!s_e5f_on && s_e5f_leaks < 0xFFFFFFFFu) {
    if (!s_e5f_leaks)
      Serial.printf("[e5f  ] releasing the mask after %u doubtful frames: %s\n",
                    f.doubt, doubtwhy);
    s_e5f_leaks++;
  }
  return s_e5f_on;
}

void setE5Filter(uint8_t m) {
  s_e5f_mode = (m > E5F_FORCE) ? E5F_AUTO : m;
  s_prefs.begin("d8link", false);
  s_prefs.putUChar("e5f", s_e5f_mode);
  s_prefs.end();
  static const char *N[] = {"OFF (relay it)", "AUTO (mask when provably false)",
                            "FORCE (always mask)"};
  Serial.printf("[e5f  ] %s\n", N[s_e5f_mode]);
}
uint8_t     e5FilterMode()    { return s_e5f_mode; }
bool        e5FilterMasking() { return s_e5f_on; }
uint32_t    e5FilterFrames()  { return s_e5f_n; }
uint32_t    e5FilterLeaks()   { return s_e5f_leaks; }
uint16_t    e5FilterDoubt()   { return s_e5f.doubt; }
const char *e5FilterWhy()     { return s_e5f_why; }

// One per direction: [0] toward the panel, [1] toward the controller --
// the same side numbering collectOut() uses.
static cn2core::TxCoalesce s_txq[2];

// Frames we EMIT, checked against their own trailing XOR.
//
// Nothing was watching this. A rewrite whose checksum was not recomputed
// produced frames the far end silently discarded, and the only symptom was the
// far end reporting a comms fault -- which read as the machine's problem, not
// ours. ok_c/ok_p only ever counted what we RECEIVE.
static uint32_t s_txbad[2] = {0, 0};

static void fwFlush(uint8_t side) {
  cn2core::TxCoalesce &q = s_txq[side];
  if (!q.n) return;
  if (q.emit) {
    if (q.want && q.n == q.want) {
      uint8_t x = 0;
      for (uint8_t i = 0; i + 1 < q.n; i++) x ^= q.buf[i];
      if (x != q.buf[q.n - 1] && s_txbad[side] < 0xFFFFFFFFu) {
        if (!s_txbad[side])
          Serial.printf("[tx   ] EMITTING BAD CHECKSUM to %s: %02X != %02X\n",
                        side == 0 ? "panel" : "board", x, q.buf[q.n - 1]);
        s_txbad[side]++;
      }
    }
    if (side == 0) s_tx[1] += uPanel.write(q.buf, q.n);
    else           s_tx[0] += uBoard.write(q.buf, q.n);
    for (uint8_t i = 0; i < q.n; i++) collectOut(side, q.buf[i]);
  }
  q.clear();
}

static void pump() {
  if (!s_open) return;

  // A partial frame whose sender stopped mid-way must not sit in the queue
  // forever -- flush it once the line has been quiet for a frame gap.
  if (s_txq[0].n && (uint32_t)(micros() - s_bp_prev) > frameGapUs()) fwFlush(0);
  if (s_txq[1].n && (uint32_t)(micros() - s_pb_prev) > frameGapUs()) fwFlush(1);

  // Forward first, log second — the relay is the thing the machine depends on.
  // The budget bounds one pass so a firehose can never starve net::loop() and
  // stall OTA; at 9600 baud it is never reached.
  int budget = 512;
  while (uBoard.available() && budget--) {
    uint8_t b = (uint8_t)uBoard.read();
    uint32_t now_us = micros();
    // A clock gap may only resync when nothing is in flight. Mid-frame it is
    // far more likely to be OUR stall than a real break -- see FramePos.
    if ((uint32_t)(now_us - s_bp_prev) > frameGapUs()) {
      s_bp_pos.markGap();                       // ends a declared desync hunt
      if (!s_bp_pos.insideFrame()) { s_bp_pos.reset(); s_bp_x = 0; fwFlush(0); }
    }
    s_bp_i = s_bp_pos.feed(b);       // 0xFF = unsynced: rewrite nothing
    s_bp_prev = now_us;
    // Decide once per frame, at its first byte, so a frame is never half-sent.
    if (s_bp_i == 0) s_thin_panel.atFrameStart();

    // Streaming rewrite: byte 3 is the status bitfield and byte 7 the XOR over
    // bytes 0..6, both at fixed offsets in the 8-byte 0xA2 frame. So we can edit
    // in flight and emit a corrected checksum without ever buffering the frame —
    // no added latency, which matters because the link is timing-sensitive.
    uint8_t out = b;
    // Byte 1 is the temperature the panel sees. Forcing it out of range is how
    // the sensor faults are provoked: the manual lists E3 "sensor open circuit"
    // and E4 "sensor short circuit", which on an NTC divider are the two rail
    // readings. Freezing it while the heater runs is the E6 "heating plate
    // malfunction" candidate — heater commanded, temperature never rises.
    if (s_bp_i == 1) {
      s_temp_real = b;
      out = (s_temp_ovr >= 0) ? (uint8_t)s_temp_ovr : b;
      s_temp_fwd = out;
    }
    // Byte 2 is the flow-meter pulse count. E1 "no water" is raised by the
    // PANEL, not the controller — it watches this counter while it has the
    // intake motor commanded, and complains when water never arrives. Holding
    // the forwarded count at zero reproduces exactly that.
    //
    // This is the safe direction of the spoof: the machine concludes there is
    // no water, so it will not heat. Faking counts UP is the dangerous inverse
    // and is deliberately not offered here.
    if (s_bp_i == 2) {
      s_flow_real = b;
      out = s_flow_spoof ? 0x00 : b;
      s_flow_fwd = out;
    }
    if (s_bp_i == 3) {
      // "Lid off" if EITHER sensor says so — the machine wants both the reed
      // (bit 1) and the micro switch (bit 7) clear before it calls the lid shut.
      s_lid_real = !cn2core::lidClosed(b);          // what the controller says
      cn2core::StatusOvr so{s_lid_mode, s_st_clr, s_st_set};
      out = cn2core::rewriteStatus(b, so);
      // Applied last, so a deliberate E5 injection still works: st_set puts
      // the bit back after the filter would have taken it out.
      if ((out & 0x40) && !(s_st_set & 0x40) && e5FilterActive()) {
        out = (uint8_t)(out & ~0x40);
        if (s_e5f_n < 0xFFFFFFFFu) s_e5f_n++;
      }
      s_lid_fwd = !cn2core::lidClosed(out);         // what the panel will see
      s_st_real = b; s_st_fwd = out;
      // Continuous hold time of the lockout bit, for the webhook that summons
      // the HA watchdog. Duration, not an edge: a single-frame glitch (we have
      // captured exactly one, ever) must not power-cycle the machine.
      if (b & 0x40) { if (!s_lock_ms) s_lock_ms = millis() | 1; }
      else s_lock_ms = 0;
    }
    // Recompute the checksum whenever ANY byte of this frame was altered.
    //
    // This used to enumerate the override flags -- lid, status mask, flow
    // spoof, temp override -- and the false-E5 filter was added as a fifth
    // rewrite without being added to the list. The panel then received a frame
    // carrying the ORIGINAL checksum over EDITED bytes, failed it, and reported
    // a communication failure: the filter caused the exact fault it exists to
    // suppress, and only the raw to_panel history showed it.
    //
    // A list of "ways a byte might change" goes stale the next time someone
    // adds a rewrite. Watching whether the byte actually changed cannot.
    // Unsynced (0xFF) parks at index 0, so start() sees a non-header, gets
    // len 0, and never substitutes a checksum: pure pass-through.
    if (s_bp_pos.i == 0) s_bp_tx.start(b);
    if (s_pure) {
      out = b;                        // before the checksum: nothing to correct
      // The "what the panel sees" mirrors are set inside the per-index blocks
      // above, which run before this point. In pure mode they would otherwise
      // report the rewrite that was suppressed -- misleading precisely when
      // someone is checking whether we are a wire.
      if      (s_bp_i == 1) s_temp_fwd = b;
      else if (s_bp_i == 2) s_flow_fwd = b;
      else if (s_bp_i == 3) { s_st_fwd = b; s_lid_fwd = !cn2core::lidClosed(b); }
    }
    out = s_bp_tx.feed(b, out);
    if (out != b) s_edit_c++;         // counts the checksum substitution too
    s_bp_pos.advance();

    // While the virtual controller is running we must NOT also forward the real
    // one, or the panel sees two controllers interleaved and rejects both.
    // Frame-atomic: the write happens when the frame is COMPLETE -- see
    // cn2core::TxCoalesce for why.
    // Thinning and the virtual controller DROP bytes, which breaks byte-for-byte
    // forwarding just as surely as rewriting one does. Pure mode overrides both.
    if (s_txq[0].feed(out, s_pure || (!s_virt && s_thin_panel.fwd))) fwFlush(0);
    if (s_capture) push(FROM_BOARD, b);
  }
  if (budget < 0) s_overflow++;   // could not drain in one pass — see /api/status

  budget = 512;
  while (uPanel.available() && budget--) {
    uint8_t b = (uint8_t)uPanel.read();

    uint32_t nu = micros();
    if ((uint32_t)(nu - s_pb_prev) > frameGapUs()) {
      s_pb_pos.markGap();                       // ends a declared desync hunt
      if (!s_pb_pos.insideFrame()) { s_pb_pos.reset(); s_pb_x = 0; fwFlush(1); }
    }
    s_pb_i = s_pb_pos.feed(b);
    s_pb_prev = nu;
    if (s_pb_i == 0) s_thin_ctrl.atFrameStart();
    bool pressing = s_press_mask && (int32_t)(millis() - s_press_until) < 0;

    // The panel frame is 5 bytes: AA, b1, b2, b3, xor. Byte 1 is the LOAD BITMAP -- an
    // earlier comment here called it "momentary button bits", which pressing all
    // eight panel buttons disproved: they produce no traffic at all.
    if (s_pb_i == 1) {
      s_panel_b1 = b;
      // Bits 6 and 7 have never been set in 99,285 checksum-valid frames, so a
      // value carrying them is a byte read at the wrong offset -- which happens
      // for a frame or two at startup before the assembler syncs. Remembering it
      // forever made the UI report a load command that never occurred.
      if (b && !(b & 0xC0)) {
        if (b != s_b1_last) s_b1_n++;
        s_b1_last = b; s_b1_at = millis();
      }
    } else if (s_pb_i == 2) s_panel_b2 = b;
    else if (s_pb_i == 3)   s_panel_b3 = b;

    cn2core::PanelOvr po;
    po.press_mask = s_press_mask; po.pressing = pressing;
    po.p1_clr = s_p1_clr; po.p1_set = s_p1_set;
    po.p2 = s_p2_ovr; po.p3 = s_p3_ovr; po.probe = s_probe;

    uint8_t out = cn2core::rewritePanelByte(s_pb_i, b, po);
    // The cap is the last thing applied, so it overrides the cycle runner and a
    // manual b5 override alike -- both can hold the intake on indefinitely, and
    // neither is a reason to let it.
    if (s_pb_i == 1) {
      s_panel_b1_want = out;
      out = s_fstall.apply(out);                // ...and if water never came
      out = s_heat.apply(out);                  // ...and above the heat ceiling
      s_panel_b1_fwd = out;
    }
    if (s_pb_i == 3) s_panel_b3_fwd = out;
    // Same single implementation as the controller direction.
    if (s_pb_pos.i == 0) s_pb_tx.start(b);
    if (s_pure) {
      out = b;
      if      (s_pb_i == 1) s_panel_b1_fwd = b;
      else if (s_pb_i == 3) s_panel_b3_fwd = b;
    }
    out = s_pb_tx.feed(b, out);
    if (out != b) s_edit_p++;
    s_pb_pos.advance();

    // While impersonating the panel we must NOT also forward the real one, or
    // the board sees two panels interleaved and rejects both.
    if (s_txq[1].feed(out, s_pure || (!s_spoof && s_thin_ctrl.fwd))) fwFlush(1);
    if (s_capture) push(FROM_PANEL, b);
  }
  if (budget < 0) s_overflow++;

  // Pretend to be the panel. Only fires in RELAY, and only sends the idle
  // frame — the one payload that cannot ask the machine to do anything.
  if (s_spoof && s_open &&
      (uint32_t)(millis() - s_spoof_last) >= s_spoof_ms) {
    s_spoof_last = millis();
    s_tx[0] += uBoard.write(s_spoof_fr, s_spoof_len);
    s_spoof_n++;
  }

  // Be the controller. 200 ms matches the real one's measured period; the panel
  // is free-running and never polls, so nothing has to be answered.
  if (s_virt && s_open && (uint32_t)(millis() - s_virt_last) >= 200) {
    s_virt_last = millis();
    if (s_virt_auto) virtSim();
    // The synthesised frame goes through the SAME overrides a real one would.
    // Without this the error-code triggers would silently do nothing in virtual
    // mode -- they rewrite the forwarded frame, and in virtual mode there is no
    // forwarded frame to rewrite. One code path, so a button means the same
    // thing whichever mode is running.
    uint8_t vt = (s_temp_ovr >= 0) ? (uint8_t)s_temp_ovr : s_virt_temp;
    uint8_t vf = s_flow_spoof ? 0x00 : s_virt_flow;
    cn2core::StatusOvr vso{s_lid_mode, s_st_clr, s_st_set};
    uint8_t vs = cn2core::rewriteStatus(s_virt_st, vso);
    uint8_t f[8] = {0xA2, vt, vf, vs, 0x04, s_virt_b5, 0x02, 0};
    f[7] = cn2core::xorOf(f, 7);
    s_temp_fwd = vt; s_flow_fwd = vf; s_st_fwd = vs;
    s_lid_fwd  = !cn2core::lidClosed(vs);
    s_tx[1] += uPanel.write(f, 8);
    for (uint8_t i = 0; i < 8; i++) collectOut(0, f[i]);
    s_virt_n++;
  }
}

size_t sendTo(uint8_t dest, const uint8_t *data, size_t n) {
  if (!s_open) return 0;
  return (dest == TO_BOARD) ? uBoard.write(data, n) : uPanel.write(data, n);
}

void setSpoofFrame(const uint8_t *d, size_t n) {
  if (!d || n < 2 || n > sizeof(s_spoof_fr)) return;
  memcpy(s_spoof_fr, d, n);
  s_spoof_len = (uint8_t)n;
}

String spoofFrameHex() {
  String o; char b[4];
  for (uint8_t i = 0; i < s_spoof_len; i++) {
    snprintf(b, sizeof(b), "%02X", s_spoof_fr[i]);
    if (i) o += ' ';
    o += b;
  }
  return o;
}

void setSpoof(bool on, uint32_t period_ms) {
  s_spoof = on;
  if (period_ms >= 20 && period_ms <= 5000) s_spoof_ms = period_ms;
  if (!on) s_spoof_n = 0;
  Serial.printf("[cn2  ] panel spoof %s (%lu ms)\n",
                on ? "ON" : "OFF", (unsigned long)s_spoof_ms);
}
bool     spoofOn()     { return s_spoof; }
uint32_t spoofPeriod() { return s_spoof_ms; }
uint32_t spoofCount()  { return s_spoof_n; }

// Forwarding lives in its own task at a priority ABOVE the Arduino loop task
// (which is 1). The web server builds multi-kilobyte strings for /api/frames,
// and while it does, loop() does not run — that stall was enough to push the
// panel's reply outside the board's response window and make E5 flash.
// Relaying is the thing the machine actually depends on, so it preempts.
// Called once a second from the relay task.
static void sampleTrends() {
  // ANY heater, not just the water one. b2 is the 110 V water heater, b3 the air
  // heater and b4 the dry blower+heater -- shading only on b2 left the whole
  // drying phase unshaded, which is where the air heater does all its work.
  // Read the FORWARDED byte, not the received one. With the cycle runner or any
  // byte-1 override driving, the real panel sends 0x00 and the load command lives
  // only in the rewritten stream -- so sampling the panel showed a flat, unshaded
  // graph through a cycle the ESP32 itself was running.
  const uint8_t pb   = s_panel_b1_fwd;
  const bool heat  = (pb & 0x1C) != 0;              // any heater, for the cut-out
  const bool wheat = (pb & 0x04) != 0;              // b2 water heater
  const bool aheat = (pb & 0x18) != 0;              // b3 air heater | b4 dry
  (void)heat;
  // Graph what the PANEL is being told, not what the controller said. With no
  // override running the two are identical, so normal operation is unchanged.
  // In virtual mode the real controller is idle and the simulation is the only
  // thing happening -- graphing the real frame would draw a flat line through
  // the middle of a cycle.
  const uint8_t temp = s_temp_fwd;
  const uint8_t flow = s_flow_fwd;

  // The cut-out is only meaningful for the WATER heater -- that is the one with a
  // reproducible setpoint (57 C across two cycles). The air heater releasing at
  // the end of a dry run is not a setpoint and would overwrite it.
  if (s_g_heat_prev && !wheat && temp) s_g_cutout = temp;
  s_g_heat_prev = wheat;

  if (flow != s_g_flow_prev) { s_g_flow_move_ms = millis(); s_g_flow_prev = flow; }

  // Two lanes: bit 7 is the WATER heater, s_g_air is the air heater or dry.
  // They were one flag, which drew a 97 C steam phase and a 40 C dry phase in
  // the same colour -- the two things the graph most needs to tell apart.
  s_g_temp[s_g_temp_head] = (uint8_t)((temp & 0x7F) | (wheat ? 0x80 : 0x00));
  if (aheat) s_g_air[s_g_temp_head >> 3] |=  (uint8_t)(1u << (s_g_temp_head & 7));
  else       s_g_air[s_g_temp_head >> 3] &= (uint8_t)~(1u << (s_g_temp_head & 7));
  s_g_temp_head = (uint16_t)((s_g_temp_head + 1) % G_TEMP_N);
  if (s_g_temp_len < G_TEMP_N) s_g_temp_len++;

  s_g_flow[s_g_flow_head] = flow;
  s_g_flow_head = (uint8_t)((s_g_flow_head + 1) % G_FLOW_N);
  if (s_g_flow_len < G_FLOW_N) s_g_flow_len++;
}

// Oldest-to-newest hex, so the page can plot left to right without reordering.
static String ringHex(const uint8_t *buf, uint16_t cap, uint16_t head, uint16_t len) {
  String o; o.reserve(len * 2);
  char b[3];
  uint16_t start = (uint16_t)((head + cap - len) % cap);
  for (uint16_t i = 0; i < len; i++) {
    snprintf(b, sizeof(b), "%02X", buf[(start + i) % cap]);
    o += b;
  }
  return o;
}

// Oldest first, as "<ms since first>:<count>" pairs.
String flowLogJson() {
  String o = "["; 
  uint16_t start = (uint16_t)((s_fev_head + 256 - s_fev_len) % 256);
  uint32_t t0 = s_fev_len ? s_fev[start].ms : 0;
  for (uint16_t i = 0; i < s_fev_len; i++) {
    const FlowEv &e = s_fev[(start + i) % 256];
    if (i) o += ",";
    o += "[" + String(e.ms - t0) + "," + String(e.count) + "]";
  }
  return o + "]";
}
void flowLogClear() { s_fev_head = 0; s_fev_len = 0; s_fev_have = false; }
uint16_t flowLogLen() { return s_fev_len; }

String trendTempHex() { return ringHex(s_g_temp, G_TEMP_N, s_g_temp_head, s_g_temp_len); }
// The air lane, unpacked to one byte per sample so it lines up with the
// temperature ring without the page having to redo the bit arithmetic.
String trendAirHex() {
  String o; o.reserve(s_g_temp_len * 2);
  uint16_t start = (uint16_t)((s_g_temp_head + G_TEMP_N - s_g_temp_len) % G_TEMP_N);
  static const char *H = "0123456789ABCDEF";
  for (uint16_t k = 0; k < s_g_temp_len; k++) {
    uint16_t i = (uint16_t)((start + k) % G_TEMP_N);
    uint8_t v = (s_g_air[i >> 3] >> (i & 7)) & 1u;
    o += H[0]; o += H[v];
  }
  return o;
}
String trendFlowHex() { return ringHex(s_g_flow, G_FLOW_N, s_g_flow_head, s_g_flow_len); }
uint8_t trendCutout() { return s_g_cutout; }
// "Filling" means the pulse count moved in the last 10 s -- the buffer is only
// worth drawing while something is actually happening.
bool    trendFlowActive() { return s_g_flow_move_ms && (millis() - s_g_flow_move_ms) < 10000; }

// ---------------------------------------------------------------------------
// CYCLE RUNNER
//
// Drives a whole wash cycle from here: the panel is held idle and this decides
// every load, every fill target, every stage boundary.
//
// The machine offers NO protection against any of this. Confirmed, not assumed:
// the controller gates nothing except the intake motor (which needs a fill
// target), the heaters fire with no water and the lid open, and NEITHER end has
// a fill timeout -- a fill whose flow count never arrives runs forever. So every
// interlock below has to exist here, because there is none underneath.
//
// The guards, in the order they can save you:
//   1. a heater stage will not start unless its fill actually completed
//   2. a fill aborts if the count stalls for FILL_STALL_MS
//   3. anything above MAX_TEMP_C aborts immediately
//   4. the lid opening aborts
//   5. any controller fault bit aborts
//   6. losing controller frames aborts
// Abort always means: clear every override, release every load, stop.

static const uint8_t  MAX_TEMP_C     = 96;    // byte 1 is uncalibrated and reads LOW
static const uint32_t FILL_STALL_MS  = 15000; // nothing at all: the fill never started
static const uint32_t FILL_SETTLE_MS = 3000;  // counting stopped: ~2.2/s, so 3 s is 7 missed
static const uint32_t LINK_DEAD_MS   = 3000;

enum : uint8_t { ST_IDLE=0, ST_RUN=1, ST_DONE=2, ST_ABORT=3, ST_PAUSE=4 };

// PAUSE vs ABORT, and the split follows the manual rather than my taste.
//
// The two conditions a user can actually fix -- the lid being open, and the tank
// being empty -- PAUSE. That is what the machine does: "Lid Open Alert: the
// system will automatically resume once the lid is closed", and "Water Shortage:
// add water, then press Start/Pause". A lid opened to add one more bottle should
// not throw away a 29-minute cycle.
//
// Everything else still ABORTS: a controller fault bit, over temperature, or a
// dead link are not things standing at the machine fixes.
//
// Pausing releases every load. The stage and its elapsed time are kept, so a
// resume continues where it stopped rather than restarting the stage.

struct Stage {
  const char *name;
  uint8_t  loads;        // panel byte 1 to command
  uint8_t  t2, t3;       // fill target pair; 0/0 = none
  uint32_t secs;         // 0 = run until the fill target is reached
  bool     needs_water;  // refuse to start unless a fill has completed
};
struct Prog {
  const char *name; Stage *st; uint8_t n;
  uint8_t maxc;      // manual's max water temperature -- a hard abort ceiling
  uint8_t water_c;   // heat stages end here (0 = run the full duration)
  uint8_t dry_c;     // dry stages abort here (0 = only the global ceiling)
};

// The manual's six programs (p.25-26), with its stated durations and maximum
// water temperatures. Stage shapes come from the captures where one exists --
// Normal Wash and Self-Cleaning were both recorded end to end -- and follow the
// same drain / fill / heat / flush / dry skeleton elsewhere.
//
// The per-program temperature ceiling is the manual's, not a guess. It is
// enforced in addition to MAX_TEMP_C, so Rapid Wash aborts at 55 C even though
// steam is allowed to reach 96.
static Stage P_RAPID[] = {          // 19 min, 55 C
  {"drain",        0x02, 0x00,0x00,  20, false},
  {"fill 90",      0x20, 0x40,0x20,   0, false},
  {"wash + heat",  0x05, 0x00,0x00, 600, true },
  {"drain",        0x02, 0x00,0x00,  20, false},
  {"fill 80",      0x20, 0xAB,0x1C,   0, false},
  {"rinse + heat", 0x05, 0x00,0x00, 380, true },
  {"drain",        0x02, 0x00,0x00,  20, false},
};
static Stage P_NORMAL[] = {         // 29 min, 68 C
  {"drain",        0x02, 0x00,0x00,  20, false},
  {"fill 90",      0x20, 0x40,0x20,   0, false},
  {"wash + heat",  0x05, 0x00,0x00, 600, true },
  {"drain",        0x02, 0x00,0x00,  20, false},
  {"fill 80",      0x20, 0xAB,0x1C,   0, false},
  {"rinse + heat", 0x05, 0x00,0x00, 480, true },
  {"drain",        0x02, 0x00,0x00,  20, false},
  {"fill 80",      0x20, 0xAB,0x1C,   0, false},
  {"rinse + heat", 0x05, 0x00,0x00, 480, true },
  {"drain",        0x02, 0x00,0x00,  20, false},
};
static Stage P_STEAM[] = {          // 9 min, 100 C -- a small charge boiled dry
  {"drain",        0x02, 0x00,0x00,  20, false},
  {"fill 20",      0x20, 0x2A,0x07,   0, false},
  {"steam",        0x04, 0x00,0x00, 400, true },
  {"flush",        0x22, 0xFF,0xFF,  70, false},
};
static Stage P_DRY[] = {            // 60 min hot air
  {"dry",          0x18, 0x00,0x00, 3600, false},
  {"blower only",  0x10, 0x00,0x00,   30, false},
};
static Stage P_STORE[] = {          // 72 h fresh-air storage
  {"storage",      0x18, 0x00,0x00, 259200, false},
  {"blower only",  0x10, 0x00,0x00,     30, false},
};
static Stage P_CLEAN[] = {          // 30 min, 70 C -- shape from the capture
  {"drain",        0x02, 0x00,0x00,  20, false},
  {"fill 100",     0x20, 0xD6,0x23,   0, false},
  {"wash + heat",  0x05, 0x00,0x00, 700, true },
  {"drain",        0x02, 0x00,0x00,  20, false},
  {"fill 90",      0x20, 0x40,0x20,   0, false},
  {"wash + heat",  0x05, 0x00,0x00, 240, true },
  {"flush",        0x22, 0xFF,0xFF, 116, false},
  {"dry",          0x18, 0x00,0x00, 426, false},
};
// ---- user-defined programs -------------------------------------------------
// Two editable slots. Stored as: loads, fill target, seconds -- the same three
// things every built-in stage is made of.
//
// Every list goes through cycValidate() before it is accepted, because a
// hand-written stage list is precisely where this machine's missing interlocks
// would bite. The rules encode what a week of captures established:
//
//   - intake with no fill target does nothing at all (the only gate the
//     controller has), so it is a mistake, not a stage
//   - target 0xFF is the untargeted flush. The machine ONLY ever sends it with
//     the drain open, and neither end has a fill timeout -- 0xFF with the drain
//     shut is an unbounded fill with nothing to stop it
//   - the water heater after a drain with no fill in between is dry fire. The
//     controller will happily do it; nothing else checks
//   - a stage that commands nothing and waits forever is a hang, not a cycle
#define CUSTOM_SLOTS 6
#define CUSTOM_MAX   12
static Stage   CUSTOM[CUSTOM_SLOTS][CUSTOM_MAX];
static char    CUSTOM_NM[CUSTOM_SLOTS][18];
static char    CUSTOM_SN[CUSTOM_SLOTS][CUSTOM_MAX][14];
static uint8_t CUSTOM_N[CUSTOM_SLOTS] = {0};

// A readable stage name from the load bits, so the tables look the same as the
// built-in programs without asking the user to name eleven things.
static void stageName(char *out, size_t n, uint8_t loads, uint8_t t3) {
  if (loads & 0x20) { snprintf(out, n, t3 == 0xFF ? "flush" : "fill"); return; }
  if (loads & 0x10) { snprintf(out, n, "dry"); return; }
  if ((loads & 0x04) && !(loads & 0x01)) { snprintf(out, n, "steam"); return; }
  if (loads & 0x04) { snprintf(out, n, "wash + heat"); return; }
  if (loads & 0x08) { snprintf(out, n, "air heat"); return; }
  if (loads & 0x01) { snprintf(out, n, "wash"); return; }
  if (loads & 0x02) { snprintf(out, n, "drain"); return; }
  snprintf(out, n, "wait");
}

// One stage as its canonical 7 characters. The inverse of the parser above.
static void stageCode(char *out, const Stage *s) {
  const char *c = "WAIT";
  if (s->t3 == 0xFF)            c = "FLSH";
  else if (s->loads & 0x20)     c = "FILL";
  else if ((s->loads & 0x18) == 0x18) c = "DRYR";
  else if (s->loads & 0x10)     c = "BLOW";
  else if (s->loads & 0x08)     c = "AIRH";
  else if (s->loads & 0x05)     c = (s->loads & 0x01) ? ((s->loads & 0x04) ? "WASH" : "PUMP") : "STEM";
  else if (s->loads & 0x02)     c = "DRAN";
  // byte3 -> counts. A byte covers a RANGE of counts, so the inverse is
  // ambiguous: 0x20 is any of 90..92. Round-tripping "FIL090" as "FIL091" looks
  // like a bug in a form that is supposed to be canonical, so the four targets
  // the machine actually uses map back to the numbers it actually fills, and
  // anything else takes the low end of its range.
  unsigned cnt = 0;
  if ((s->loads & 0x20) && s->t3 != 0xFF) {
    switch (s->t3) {
      case 0x07: cnt = 20;  break;
      case 0x1C: cnt = 80;  break;
      case 0x20: cnt = 90;  break;
      case 0x23: cnt = 100; break;
      default:   cnt = (s->t3 * 20u >= 10u) ? (s->t3 * 20u - 10u + 6u) / 7u : 0;
    }
  }
  if (cnt > 99) cnt = 99;
  if (s->loads & 0x20 && s->t3 != 0xFF) { snprintf(out, 8, "%s%02uP", c, cnt); return; }
  // Largest unit that keeps the value under 100, so 20S stays 20S and 3600 s
  // becomes 01H rather than an unreadable 3600.
  uint32_t v = s->secs; char u = 'S';
  if (v >= 6000)      { v = (v + 1800) / 3600; u = 'H'; }
  else if (v >= 100)  { v = (v + 30) / 60;     u = 'M'; }
  if (v > 99) v = 99;
  snprintf(out, 8, "%s%02u%c", c, (unsigned)v, u);
}

// Returns nullptr if the list is safe to run, else why not.
static const char *cycValidate(const Stage *st, uint8_t n) {
  if (!n) return "no stages";
  if (n > CUSTOM_MAX) return "too many stages";
  bool water = false;                 // a fill has completed since the last drain
  for (uint8_t i = 0; i < n; i++) {
    const uint8_t L = st[i].loads;
    if (L & 0xC0) return "bits 6 and 7 have never been seen set";
    if ((L & 0x20) && st[i].t3 == 0) return "intake with no fill target does nothing";
    if (st[i].t3 == 0xFF && !(L & 0x02))
      return "0xFF is the untargeted flush: it needs the drain open";
    if ((L & 0x20) && st[i].secs == 0 && st[i].t3 == 0xFF)
      return "an untargeted flush needs a duration";
    if (!L && st[i].secs == 0) return "a stage with no loads must have a duration";
    if ((L & 0x04) && !water) return "water heater before any fill — dry fire";
    if (L & 0x20) water = true;       // filling
    if (L & 0x02) water = false;      // draining empties it again
  }
  return nullptr;
}

#define PROG(a,c) {#a, P_##a, sizeof(P_##a)/sizeof(Stage), c}
static Prog s_progs[] = {
  {"Rapid Wash",   P_RAPID,  sizeof(P_RAPID)/sizeof(Stage),  55, 50, 0 },
  {"Normal Wash",  P_NORMAL, sizeof(P_NORMAL)/sizeof(Stage), 68, 60, 0 },
  {"Steam Steril", P_STEAM,  sizeof(P_STEAM)/sizeof(Stage),  96, 92, 0 },
  {"Drying",       P_DRY,    sizeof(P_DRY)/sizeof(Stage),    96,  0, 80},
  {"72h Storage",  P_STORE,  sizeof(P_STORE)/sizeof(Stage),  96,  0, 60},
  {"Self-Clean",   P_CLEAN,  sizeof(P_CLEAN)/sizeof(Stage),  70, 65, 80},
  {CUSTOM_NM[0],   CUSTOM[0], 0, 96, 60, 80},
  {CUSTOM_NM[1],   CUSTOM[1], 0, 96, 60, 80},
  {CUSTOM_NM[2],   CUSTOM[2], 0, 96, 60, 80},
  {CUSTOM_NM[3],   CUSTOM[3], 0, 96, 60, 80},
  {CUSTOM_NM[4],   CUSTOM[4], 0, 96, 60, 80},
  {CUSTOM_NM[5],   CUSTOM[5], 0, 96, 60, 80},
};
#define CUSTOM0 6
static const uint8_t PROG_N = sizeof(s_progs)/sizeof(s_progs[0]);
static uint8_t s_mode = 1;          // the manual's default is Normal Wash

#define STG   (s_progs[s_mode].st)
#define STG_N (s_progs[s_mode].n)

static uint8_t  s_cyc_state = ST_IDLE, s_cyc_i = 0;
static uint32_t s_cyc_t0 = 0, s_cyc_flow_ms = 0;
static uint8_t  s_cyc_flow_prev = 0, s_cyc_flow_max = 0;
static bool     s_cyc_water = false;      // a fill has completed since the last drain
static char     s_cyc_why[48] = "";
static uint32_t s_cyc_held = 0;      // seconds already spent in the paused stage
static bool     s_cyc_lidpause = false;   // paused BY the lid: auto-resumes

// Cycle progress survives a mains cut. The lockout recovery is a power cycle
// at the plug, and the plug also powers this board -- so without this, every
// recovery converts a running cycle into an idle machine with clean water in
// it and nobody the wiser. Written at stage boundaries only (a handful of NVS
// writes per cycle, not a wear problem), erased on any normal exit.
//
// Deliberately NOT replayed automatically at boot: a stale record must never
// start pumps on its own after, say, a week-old crash. The HA watchdog calls
// /api/cycle_recover after it has power-cycled the machine AND verified with
// the flow probe that the controller is accepting commands again. Recovery
// restarts the interrupted stage from zero -- fills re-measure their own flow,
// and s_cyc_water carries over because the water is physically still there.
// The writes are DEFERRED to the main loop (persistTick). cycPersist is
// called from cycleTick, which runs in relayTask -- and an NVS write is a
// flash operation that stalls the single C3 core well past the task
// watchdog's patience. The first build that wrote directly from relayTask
// panicked the board at the first fill->wash boundary of a live cycle.
// relayTask only snapshots and raises a flag; the flash work happens where
// every other NVS write in this codebase already happens.
static volatile uint8_t s_cyc_save = 0;   // 0 idle, 1 write record, 2 clear
static uint8_t s_cyc_save_m = 0, s_cyc_save_i = 0;
static bool    s_cyc_save_w = false;
static void cycPersist() {
  s_cyc_save_m = s_mode; s_cyc_save_i = s_cyc_i; s_cyc_save_w = s_cyc_water;
  s_cyc_save = 1;
}
static void cycPersistClear() { s_cyc_save = 2; }

static void cycApply(const Stage &s) {
  s_p1_clr = 0xFF; s_p1_set = s.loads;          // byte 1 becomes exactly this
  s_p2_ovr = (s.t2 || s.t3) ? s.t2 : 0;
  s_p3_ovr = (s.t2 || s.t3) ? s.t3 : 0;
}
static void cycRelease() {
  s_p1_clr = 0; s_p1_set = 0; s_p2_ovr = -1; s_p3_ovr = -1;
}
static void cycPause(const char *why, bool by_lid) {
  cycRelease();                       // every load off while paused
  s_cyc_held = (millis() - s_cyc_t0) / 1000;
  s_cyc_lidpause = by_lid;
  s_cyc_state = ST_PAUSE;
  snprintf(s_cyc_why, sizeof(s_cyc_why), "%s", why);
  Serial.printf("[cycle] PAUSED: %s\n", why);
}

static void cycAbort(const char *why) {
  cycRelease();
  cycPersistClear();
  s_cyc_state = ST_ABORT;
  snprintf(s_cyc_why, sizeof(s_cyc_why), "%s", why);
  Serial.printf("[cycle] ABORT: %s\n", why);
}

// Stage durations persist. Retyping eleven fields after every reboot is the kind
// of friction that makes people stop adjusting them.
static uint32_t s_prog_def[6][12];
static bool     s_def_saved = false;
static void cycKeepDefaults() {
  if (s_def_saved) return;
  for (uint8_t m = 0; m < PROG_N && m < 6; m++)
    for (uint8_t i = 0; i < s_progs[m].n && i < 12; i++)
      s_prog_def[m][i] = s_progs[m].st[i].secs;
  s_def_saved = true;
}
// Durations persist PER PROGRAM. Retyping them after every reboot is the kind of
// friction that stops people adjusting them at all.
static void cycSave() {
  s_prefs.begin("d8link", false);
  char k[10];
  for (uint8_t i = 0; i < STG_N; i++) {
    snprintf(k, sizeof(k), "c%us%u", s_mode, i);
    s_prefs.putULong(k, STG[i].secs);
  }
  s_prefs.end();
}
void cycleLoad() {
  cycKeepDefaults();
  s_prefs.begin("d8link", true);
  char k[10];
  for (uint8_t m = 0; m < PROG_N; m++)
    for (uint8_t i = 0; i < s_progs[m].n; i++) {
      snprintf(k, sizeof(k), "c%us%u", m, i);
      s_progs[m].st[i].secs = s_prefs.getULong(k, s_progs[m].st[i].secs);
    }
  for (uint8_t slot = 0; slot < CUSTOM_SLOTS; slot++) {
    char nk[10], sk[10];
    snprintf(nk, sizeof(nk), "cu%un", slot);
    snprintf(sk, sizeof(sk), "cu%us", slot);
    String nm = s_prefs.getString(nk, "");
    String sp = s_prefs.getString(sk, "");
    if (!sp.length() && slot < 2 && !s_prefs.getBool("cuseed", false)) {
      // Two worked examples on a fresh device, so the feature is discoverable
      // rather than six empty boxes. Seeded ONCE -- deleting them must not make
      // them reappear at the next boot.
      nm = slot ? "Triple Wash" : "Quick check-up";
      sp = slot
        ? "02:00:20,20:20:0,05:00:300,02:00:20,20:1C:0,05:00:300,"
          "02:00:20,20:1C:0,05:00:300,22:FF:70,18:00:420"
        : "02:00:20,20:07:0,05:00:120,02:00:20,10:00:60";
    }
    s_prefs.end();
    if (sp.length()) cycleSetCustom(slot, nm.c_str(), sp.c_str());
    s_prefs.begin("d8link", true);
  }
  s_prefs.end();
  s_prefs.begin("d8link", false);
  s_prefs.putBool("cuseed", true);          // seeding happens once, ever
  s_prefs.end();
  s_prefs.begin("d8link", true);
  for (uint8_t m = 0; m < PROG_N; m++) {
    snprintf(k, sizeof(k), "c%uw", m);
    s_progs[m].water_c = s_prefs.getUChar(k, s_progs[m].water_c);
    snprintf(k, sizeof(k), "c%ud", m);
    s_progs[m].dry_c = s_prefs.getUChar(k, s_progs[m].dry_c);
  }
  s_prefs.end();
}
void cycleResetSecs() {
  cycKeepDefaults();
  for (uint8_t i = 0; i < STG_N && i < 12; i++) STG[i].secs = s_prog_def[s_mode][i];
  cycSave();
  Serial.printf("[cycle] %s times restored to defaults\n", s_progs[s_mode].name);
}
uint8_t     cycleModeCount()          { return PROG_N; }
uint8_t     cycleMode()               { return s_mode; }
const char *cycleModeName(uint8_t m)  { return m < PROG_N ? s_progs[m].name : ""; }
uint8_t     cycleModeMaxC(uint8_t m)  { return m < PROG_N ? s_progs[m].maxc : 0; }
uint8_t     cycleWaterC()             { return s_progs[s_mode].water_c; }
uint8_t     cycleDryC()               { return s_progs[s_mode].dry_c; }
void cycleSetTemps(int16_t water, int16_t dry) {
  Prog &P = s_progs[s_mode];
  // Never above the manual's ceiling for the program: a setpoint the abort guard
  // would trip on first is not a setpoint, it is a trap.
  if (water >= 0) P.water_c = (water > P.maxc) ? P.maxc : (uint8_t)water;
  if (dry   >= 0) P.dry_c   = (dry   > MAX_TEMP_C) ? MAX_TEMP_C : (uint8_t)dry;
  s_prefs.begin("d8link", false);
  char k[10];
  snprintf(k, sizeof(k), "c%uw", s_mode); s_prefs.putUChar(k, P.water_c);
  snprintf(k, sizeof(k), "c%ud", s_mode); s_prefs.putUChar(k, P.dry_c);
  s_prefs.end();
  Serial.printf("[cycle] %s: water %u C, dry ceiling %u C\n", P.name, P.water_c, P.dry_c);
}
// Readable stage syntax, because "02:00:20" is fine for a machine and hostile to
// a person. Both forms are accepted and the hex form still round-trips.
//
//   drain 20s          wash+heat 5m        steam 7m
//   fill 90            flush 70s           dry 10m       wait 30s
//
// A bare number is seconds; s/m/h are honoured. "fill N" is the intake motor
// with a target of N counts -- byte3 = round(N * 0.35), the relation the machine
// itself uses. "flush" is the untargeted drain-and-fill.
struct Kw { const char *w; uint8_t bits; };
static const Kw KW[] = {
  {"wash", 0x01}, {"pump", 0x01}, {"drain", 0x02}, {"heat", 0x04},
  {"waterheat", 0x04}, {"airheat", 0x08}, {"dry", 0x18}, {"blower", 0x10},
  {"intake", 0x20}, {"steam", 0x04}, {"wait", 0x00},
};

// Parses one stage into loads/target/seconds. Returns false on a word it does
// not know, so a typo is an error rather than a silently empty stage.
static bool parseStage(const char *a, const char *b, Stage &out) {
  char buf[64];
  size_t n = (size_t)(b - a); if (n >= sizeof(buf)) n = sizeof(buf) - 1;
  memcpy(buf, a, n); buf[n] = 0;
  for (char *c = buf; *c; c++) *c = (char)tolower((unsigned char)*c);

  out.loads = 0; out.t2 = 0; out.t3 = 0; out.secs = 0;
  bool any = false, isfill = false, isflush = false;
  char *tok = strtok(buf, " \t");
  while (tok) {
    if (!strcmp(tok, "fill"))       { out.loads |= 0x20; isfill = any = true; }
    else if (!strcmp(tok, "flush")) { out.loads |= 0x22; out.t3 = 0xFF; isflush = any = true; }
    else if (isdigit((unsigned char)tok[0])) {
      char *e; unsigned long v = strtoul(tok, &e, 10);
      // Integer, not float. 0.35f is 0.349999994, so 90 * 0.35f + 0.5 truncates
      // to 31 where the machine's own value for 90 counts is 32 -- a fill two
      // counts short of what was asked for. 0.35 is exactly 7/20, so do that:
      //   20 -> 7,  80 -> 28,  90 -> 32,  100 -> 35   all four observed targets
      if (isfill && !out.t3) out.t3 = (uint8_t)((v * 7 + 10) / 20);   // counts -> byte3
      else out.secs = (*e=='m') ? v*60 : (*e=='h') ? v*3600 : v;     // s is the default
    } else {
      // "wash+heat" and friends
      char *pl = tok;
      while (pl) {
        char *nx = strchr(pl, '+'); if (nx) *nx++ = 0;
        bool hit = false;
        for (const Kw &k : KW) if (!strcmp(pl, k.w)) { out.loads |= k.bits; hit = any = true; break; }
        if (!hit && *pl) return false;
        pl = nx;
      }
    }
    tok = strtok(nullptr, " \t");
  }
  return any;
}

// CANONICAL FORM -- fixed width, readable, and what the device stores.
//
// Each stage is exactly 7 characters: a 4-letter code, then two digits and a
// unit -- S seconds, M minutes, H hours, or P for a fill percentage.
//
//     CODE nnU
//     DRAN 20S     drain 20 seconds
//     FILL 90P     fill to 90 %, no time -- it ends when the count stops
//     WASH 05M     wash + heat, 5 minutes
//
//     -> "DRAN20S,FILL90P,WASH05M"     commas separate stages on output and are
//                                       optional on input
//
// [A-Za-z0-9] only. No separators, no spaces, nothing that needs URL-encoding,
// and a length that is always 7 x stages, so truncation is caught by length.
//
// 99H is the ceiling, so the 72-hour storage program is expressible. P maps 1:1
// to flow counts against a 100 % fill: 20P is the steam charge, 90P a wash fill,
// and 99P reaches 0x23 -- the largest target the machine itself uses.
//
// The hex, colon and free-text forms are still accepted for authoring; all of
// them produce the same stored bytes.
struct Mn { const char *c; uint8_t bits; };
static const Mn MN[] = {
  {"DRAN", 0x02},  // drain
  {"FILL", 0x20},  // intake, with a fill target
  {"PUMP", 0x01},  // wash pump alone, no heat
  {"WASH", 0x05},  // wash pump + water heater -- the usual wash stage
  {"STEM", 0x04},  // water heater with no circulation
  {"AIRH", 0x08},  // air heater alone
  {"BLOW", 0x10},  // blower alone
  {"DRYR", 0x18},  // air heater + blower
  {"FLSH", 0x22},  // drain + intake, untargeted
  {"WAIT", 0x00},  // nothing commanded
};
// Returns nullptr on success, else the validation failure.
const char *cycleSetCustom(uint8_t slot, const char *name, const char *spec) {
  if (slot >= CUSTOM_SLOTS) return "bad slot";
  if (s_cyc_state == ST_RUN || s_cyc_state == ST_PAUSE) return "cycle is running";

  Stage tmp[CUSTOM_MAX];
  uint8_t n = 0;
  const char *p = spec;

  size_t len = strlen(spec);
  // Commas separate stages. They are optional on input -- "DRAN20SFILL90P" and
  // "DRAN20S,FILL90P" are the same thing -- and always present on output, which
  // is what makes a long program scannable.
  char strip[CUSTOM_MAX * 8 + 2];
  {
    size_t w = 0;
    for (size_t i = 0; i < len && w < sizeof(strip) - 1; i++)
      if (spec[i] != ',') strip[w++] = spec[i];
    strip[w] = 0;
  }
  bool alnum = strip[0] != 0, hasspace = false;
  for (size_t i = 0; spec[i]; i++) {
    if (!isalnum((unsigned char)spec[i]) && spec[i] != ',') alnum = false;
    if (spec[i] == ' ') hasspace = true;
  }
  if (alnum && !hasspace) { spec = strip; len = strlen(strip); }
  else len = strlen(spec);
  // Canonical: letters and digits only, no spaces, and 7 per stage. Checked
  // before anything else so a truncated one gets its own message rather than
  // falling through to the free-text parser and reporting "unknown stage",
  // which sends you looking in the wrong place.
  if (alnum && !hasspace && isalpha((unsigned char)spec[0])) {
    if ((len % 7) != 0)
      return "canonical form is 7 chars per stage (CODEnnU) — length is not a multiple of 7";
    if (len / 7 > CUSTOM_MAX) return "too many stages";
    for (size_t i = 0; i < len; i += 7, n++) {
      char c[5] = {(char)toupper((unsigned char)spec[i]), (char)toupper((unsigned char)spec[i+1]),
                   (char)toupper((unsigned char)spec[i+2]), (char)toupper((unsigned char)spec[i+3]), 0};
      const Mn *m = nullptr;
      for (const Mn &k : MN) if (!strcmp(c, k.c)) { m = &k; break; }
      if (!m) return "unknown code — DRAN FILL PUMP WASH STEM AIRH BLOW DRYR FLSH WAIT";
      if (!isdigit((unsigned char)spec[i+4]) || !isdigit((unsigned char)spec[i+5]))
        return "expected CODEnnU — 4 letters, 2 digits, then S M H or P";
      uint32_t v = (uint32_t)(spec[i+4] - '0') * 10 + (uint32_t)(spec[i+5] - '0');
      char u = (char)toupper((unsigned char)spec[i+6]);
      tmp[n].loads = m->bits;
      tmp[n].t2 = 0;
      tmp[n].t3 = 0;
      tmp[n].secs = 0;
      if (u == 'P') {
        if (!(m->bits & 0x20)) return "P is a fill percentage — only FILL takes it";
        // 100 % is the largest fill the machine performs, and the percentage maps
        // 1:1 to flow counts: 20P -> 0x07, 80P -> 0x1C, 90P -> 0x20, 99P -> 0x23.
        tmp[n].t3 = (uint8_t)((v * 7 + 10) / 20);
      } else if (u == 'S') tmp[n].secs = v;
      else if (u == 'M')   tmp[n].secs = v * 60;
      else if (u == 'H')   tmp[n].secs = v * 3600;
      else return "unit must be S, M, H or P";
      if (!strcmp(c, "FLSH")) tmp[n].t3 = 0xFF;
      tmp[n].needs_water = (tmp[n].loads & 0x04) != 0;
    }
    const char *bad0 = cycValidate(tmp, n);
    if (bad0) return bad0;
    goto store;
  }
  // Hex triples, kept working: pure hex, 8 per stage.
  {
  bool allhex = len > 0;
  for (size_t i = 0; allhex && i < len; i++)
    if (!isxdigit((unsigned char)spec[i])) allhex = false;
  if (allhex && (len % 8) == 0) {
    auto hx = [](const char *q, uint8_t d) {
      char b[5] = {0}; memcpy(b, q, d); return (uint32_t)strtoul(b, nullptr, 16);
    };
    if (len / 8 > CUSTOM_MAX) return "too many stages";
    for (size_t i = 0; i < len; i += 8, n++) {
      tmp[n].loads = (uint8_t)hx(spec + i, 2);
      tmp[n].t2    = 0;
      tmp[n].t3    = (uint8_t)hx(spec + i + 2, 2);
      tmp[n].secs  = hx(spec + i + 4, 4);
      tmp[n].needs_water = (tmp[n].loads & 0x04) != 0;
    }
    const char *bad0 = cycValidate(tmp, n);
    if (bad0) return bad0;
    goto store;
  }
  }

  {
  const bool hexform = strchr(spec, ':') != nullptr;
  while (*p && n < CUSTOM_MAX) {
    while (*p == ' ') p++;
    const char *comma = strchr(p, ',');
    const char *end = comma ? comma : p + strlen(p);
    if (hexform) {
      char *e;
      uint8_t L = (uint8_t)strtoul(p, &e, 16);
      if (*e != ':') return "expected loads:target:seconds";
      uint8_t T = (uint8_t)strtoul(e + 1, &e, 16);
      if (*e != ':') return "expected loads:target:seconds";
      tmp[n].loads = L; tmp[n].t2 = 0; tmp[n].t3 = T;
      tmp[n].secs = strtoul(e + 1, &e, 10);
    } else if (!parseStage(p, end, tmp[n])) {
      return "unknown stage — try: drain 20s, fill 90, wash+heat 5m, flush 70s, dry 10m";
    }
    tmp[n].needs_water = (tmp[n].loads & 0x04) != 0;   // set by us, never the user
    n++;
    if (!comma) break;
    p = comma + 1;
  }
  if (!n) return "nothing parsed";
  const char *bad = cycValidate(tmp, n);
  if (bad) return bad;
  }

store:
  for (uint8_t i = 0; i < n; i++) {
    CUSTOM[slot][i] = tmp[i];
    stageName(CUSTOM_SN[slot][i], sizeof(CUSTOM_SN[slot][i]), tmp[i].loads, tmp[i].t3);
    CUSTOM[slot][i].name = CUSTOM_SN[slot][i];
  }
  CUSTOM_N[slot] = n;
  snprintf(CUSTOM_NM[slot], sizeof(CUSTOM_NM[slot]), "%s",
           (name && *name) ? name : (slot ? "Custom 2" : "Custom 1"));
  s_progs[CUSTOM0 + slot].n = n;

  s_prefs.begin("d8link", false);
  char k[10];
  snprintf(k, sizeof(k), "cu%un", slot); s_prefs.putString(k, CUSTOM_NM[slot]);
  // Always store the canonical form, whichever way it was written.
  char canon[CUSTOM_MAX * 8 + 1]; canon[0] = 0;
  for (uint8_t i = 0; i < n; i++) {
    char one[8]; stageCode(one, CUSTOM[slot] + i);
    if (i) strcat(canon, ",");
    strcat(canon, one);
  }
  snprintf(k, sizeof(k), "cu%us", slot); s_prefs.putString(k, canon);
  s_prefs.end();
  Serial.printf("[cycle] custom %u: %s, %u stages\n", slot, CUSTOM_NM[slot], n);
  return nullptr;
}
uint8_t     cycleCustomSlots()      { return CUSTOM_SLOTS; }
uint8_t     cycleCustomFirst()      { return CUSTOM0; }
const char *cycleStageSpec(uint8_t i) {          // canonical 7-char form
  static char b[9];
  if (i >= STG_N) { b[0] = 0; return b; }
  stageCode(b, &STG[i]);
  return b;
}

bool cycleModeEmpty(uint8_t m) { return m < PROG_N && s_progs[m].n == 0; }
// Delete a user program. Built-ins cannot be deleted.
const char *cycleDelCustom(uint8_t slot) {
  if (slot >= CUSTOM_SLOTS) return "bad slot";
  if (s_cyc_state == ST_RUN || s_cyc_state == ST_PAUSE) return "cycle is running";
  CUSTOM_N[slot] = 0;
  s_progs[CUSTOM0 + slot].n = 0;
  CUSTOM_NM[slot][0] = 0;
  s_prefs.begin("d8link", false);
  char k[10];
  snprintf(k, sizeof(k), "cu%un", slot); s_prefs.remove(k);
  snprintf(k, sizeof(k), "cu%us", slot); s_prefs.remove(k);
  s_prefs.end();
  if (s_mode == CUSTOM0 + slot) s_mode = 1;      // do not sit on a deleted one
  Serial.printf("[cycle] custom %u deleted\n", slot);
  return nullptr;
}

void cycleSetMode(uint8_t m) {
  if (m >= PROG_N || s_cyc_state == ST_RUN) return;   // never switch mid-run
  if (s_progs[m].n == 0) return;                      // an empty slot is not a program
  s_mode = m;
  Serial.printf("[cycle] program: %s\n", s_progs[m].name);
}

void cycleStart() {
  if (s_cyc_state == ST_RUN) return;
  s_cyc_i = 0; s_cyc_t0 = millis(); s_cyc_water = false;
  s_cyc_flow_ms = millis(); s_cyc_flow_prev = 0; s_cyc_flow_max = 0;
  s_cyc_why[0] = 0; s_cyc_state = ST_RUN;
  cycPersist();
  cycApply(STG[0]);
  Serial.println("[cycle] started");
}

// Called by resumeTick() after a power cycle (see the gates there), or
// explicitly via POST /api/cycle_recover.
bool cycleRecover() {
  if (s_cyc_state == ST_RUN || s_cyc_state == ST_PAUSE) return false;
  Preferences p; p.begin("d8link", true);
  const bool on = p.getBool("cyc_on", false);
  const uint8_t m = p.getUChar("cyc_m", 0);
  const uint8_t i = p.getUChar("cyc_i", 0);
  const bool w_ = p.getBool("cyc_w", false);
  p.end();
  if (!on) return false;
  if (m >= PROG_N) { cycPersistClear(); return false; }
  cycleSetMode(m);
  if (i >= STG_N)  { cycPersistClear(); return false; }
  s_cyc_i = i; s_cyc_t0 = millis();
  s_cyc_water = w_;
  s_cyc_flow_ms = millis(); s_cyc_flow_prev = 0; s_cyc_flow_max = 0;
  s_cyc_why[0] = 0; s_cyc_state = ST_RUN;
  cycApply(STG[i]);
  Serial.printf("[cycle] RECOVERED: %s at stage %u/%u\n",
                cycleModeName(m), i + 1, STG_N);
  return true;
}
void cycleStop() {
  cycRelease();
  cycPersistClear();
  s_cyc_state = ST_IDLE; s_cyc_why[0] = 0;
  Serial.println("[cycle] stopped by request");
}
void cycleResume() {
  if (s_cyc_state != ST_PAUSE) return;
  s_cyc_t0 = millis() - s_cyc_held * 1000UL;   // continue the stage, not restart it
  s_cyc_flow_ms = millis();
  s_cyc_lidpause = false;
  s_cyc_why[0] = 0;
  s_cyc_state = ST_RUN;
  cycApply(STG[s_cyc_i]);
  Serial.printf("[cycle] resumed at stage %u\n", s_cyc_i + 1);
}
bool     cycleRunning()  { return s_cyc_state == ST_RUN; }
uint8_t  cycleState()    { return s_cyc_state; }
uint8_t  cycleStage()    { return s_cyc_i; }
uint8_t  cycleCount()    { return STG_N; }
uint32_t cycleElapsed()  { return s_cyc_state == ST_RUN ? (millis()-s_cyc_t0)/1000 : 0; }
const char *cycleWhy()   { return s_cyc_why; }
const char *cycleName(uint8_t i)  { return i < STG_N ? STG[i].name : ""; }
uint32_t    cycleSecs(uint8_t i)  { return i < STG_N ? STG[i].secs : 0; }
uint8_t     cycleLoads(uint8_t i) { return i < STG_N ? STG[i].loads : 0; }
uint8_t     cycleTgt(uint8_t i)   { return i < STG_N ? STG[i].t3 : 0; }
void cycleSetSecs(uint8_t i, uint32_t v) {
  if (i >= STG_N || STG[i].secs == v) return;   // no write, no flash wear
  STG[i].secs = v;
  cycSave();
}

// Called once per second from relayTask.
// ---- panel-cycle tracker ---------------------------------------------------
// Fed at 1 Hz from relayTask. Reads s_panel_b1/s_panel_b3 -- the REAL panel's
// bytes -- so a cycle the ESP32 runs (panel idle throughout) never registers,
// and neither do overrides, which only edit the forwarded copy.
#define PC_LEARN_SLOTS 4
#define PC_MAX_ST      20
struct Learned {
  char     name[16];
  uint8_t  n = 0;
  cn2core::RefStage st[PC_MAX_ST];
};
static Learned  s_learned[PC_LEARN_SLOTS];
static bool     s_pc_dirty = false;   // overrides ran during this cycle: do not learn from it

static void learnedSave(uint8_t i) {
  char k[8]; snprintf(k, sizeof(k), "lp%u", i);
  s_prefs.begin("d8link", false);
  if (s_learned[i].n) s_prefs.putBytes(k, &s_learned[i], sizeof(Learned));
  else                s_prefs.remove(k);
  s_prefs.end();
}
static void learnedLoad() {
  s_prefs.begin("d8link", true);
  for (uint8_t i = 0; i < PC_LEARN_SLOTS; i++) {
    char k[8]; snprintf(k, sizeof(k), "lp%u", i);
    if (s_prefs.getBytes(k, &s_learned[i], sizeof(Learned)) != sizeof(Learned))
      s_learned[i] = Learned();
    s_learned[i].name[15] = 0;
    if (s_learned[i].n > PC_MAX_ST) s_learned[i] = Learned();
  }
  s_prefs.end();
}

static bool      s_pc_active = false;
static uint32_t  s_pc_start = 0, s_pc_stage_at = 0, s_pc_idle_at = 0;
static cn2core::ObsStage s_pc_obs[24];
static uint32_t  s_pc_at[24];        // stage start, seconds since cycle start
static uint8_t   s_pc_n = 0;
static int8_t    s_pc_guess = -1;   // 0-5 stock, 10+slot learned, -1 none
static uint8_t   s_pc_prev = 0, s_pc_run = 0;
// A cycle has short all-idle beats between stages; only a long one ends it.
static const uint32_t PC_END_MS = 150000UL;

static void pcycleGuessUpdate() {
  int best = -1, hits = 0;
  for (uint8_t p = 0; p < 6; p++) {
    // The replica tables ARE the reference: same loads, same targets, and
    // their per-stage durations reproduce the manual's totals.
    cn2core::RefStage ref[16];
    const Prog &P = s_progs[p];
    for (uint8_t i = 0; i < P.n && i < 16; i++)
      ref[i] = { P.st[i].loads, P.st[i].t3, P.st[i].secs };
    if (cn2core::matchProgram(s_pc_obs, s_pc_n, ref, P.n) >= 0) { best = p; hits++; }
  }
  for (uint8_t i = 0; i < PC_LEARN_SLOTS; i++)
    if (s_learned[i].n &&
        cn2core::matchProgram(s_pc_obs, s_pc_n, s_learned[i].st,
                              s_learned[i].n) >= 0) { best = 10 + i; hits++; }
  s_pc_guess = (hits == 1) ? (int8_t)best : -1;   // ambiguous is not a guess
}

// Called once, when a cycle ends. end_s is when the last stage actually
// stopped (the tracker only declares the end PC_END_MS later).
static void pcycleLearn(uint32_t end_s) {
  if (s_pc_dirty || s_pc_n < 4 || s_pc_n > PC_MAX_ST) return;
  // Durations come from the stage-change timestamps of THIS run.
  cn2core::RefStage run[PC_MAX_ST];
  for (uint8_t i = 0; i < s_pc_n; i++) {
    const uint32_t nxt = (i + 1 < s_pc_n) ? s_pc_at[i + 1] : end_s;
    run[i] = { s_pc_obs[i].loads, s_pc_obs[i].t3,
               (nxt > s_pc_at[i]) ? nxt - s_pc_at[i] : 1 };
  }
  // A full match against an existing profile refreshes its timings; a stock
  // match needs nothing. Otherwise take a free slot; none free, learn nothing
  // rather than silently evict.
  if (s_pc_guess >= 10) {
    Learned &L = s_learned[s_pc_guess - 10];
    if (s_pc_n == L.n) { memcpy(L.st, run, sizeof(run[0]) * L.n); learnedSave(s_pc_guess - 10); }
    return;
  }
  if (s_pc_guess >= 0) return;
  for (uint8_t i = 0; i < PC_LEARN_SLOTS; i++) {
    if (s_learned[i].n) continue;
    Learned &L = s_learned[i];
    snprintf(L.name, sizeof(L.name), "program %c", 'A' + i);
    L.n = s_pc_n;
    memcpy(L.st, run, sizeof(run[0]) * L.n);
    learnedSave(i);
    Serial.printf("[pcyc ] learned \"%s\": %u stages, %lu s total\n", L.name,
                  L.n, (unsigned long)cn2core::totalEstSecs(L.st, L.n));
    return;
  }
  Serial.println("[pcyc ] unmatched cycle, but all learn slots are full");
}

static void pcycleTick() {
  // Debounce: two consecutive 1 Hz samples must agree before anything counts.
  const uint8_t b1 = s_panel_b1;
  if (b1 != s_pc_prev) { s_pc_prev = b1; s_pc_run = 0; return; }
  if (s_pc_run < 2 && ++s_pc_run < 2) return;

  const uint32_t now = millis();
  if (b1 == 0) {
    if (s_pc_active) {
      if (!s_pc_idle_at) s_pc_idle_at = now;
      else if ((uint32_t)(now - s_pc_idle_at) > PC_END_MS) {
        Serial.printf("[pcyc ] panel cycle ended after %lu s, %u stages\n",
                      (unsigned long)((now - s_pc_start) / 1000), s_pc_n);
        pcycleLearn((s_pc_idle_at - s_pc_start) / 1000);
        s_pc_active = false; s_pc_n = 0; s_pc_guess = -1; s_pc_dirty = false;
      }
    }
    return;
  }
  s_pc_idle_at = 0;
  if (!s_pc_active) {
    s_pc_active = true; s_pc_start = now; s_pc_n = 0; s_pc_guess = -1;
    s_pc_dirty = false;
    Serial.println("[pcyc ] panel cycle started");
  }
  // Overrides rewrite what the CONTROLLER hears, so a cycle they touched is
  // not a clean specimen of the program -- track it, learn nothing from it.
  if (s_p1_clr || s_p1_set || s_probe || s_virt || s_p3_ovr >= 0)
    s_pc_dirty = true;
  const uint8_t t3 = (b1 == 0x20 || b1 == 0x22) ? s_panel_b3 : 0;
  if (s_pc_n == 0 || s_pc_obs[s_pc_n - 1].loads != b1) {
    if (s_pc_n < 24) {
      s_pc_at[s_pc_n] = (now - s_pc_start) / 1000;
      s_pc_obs[s_pc_n++] = { b1, t3 };
      pcycleGuessUpdate();
    }
    s_pc_stage_at = now;
  } else if (t3 && !s_pc_obs[s_pc_n - 1].t3) {
    s_pc_obs[s_pc_n - 1].t3 = t3;                 // target arrived a beat late
    pcycleGuessUpdate();
  }
}

bool     pcycleActive()   { return s_pc_active; }
uint32_t pcycleElapsedS() { return s_pc_active ? (millis() - s_pc_start) / 1000 : 0; }
uint8_t  pcycleStageN()   { return s_pc_n; }
int8_t   pcycleGuess()    { return s_pc_active ? s_pc_guess : -1; }
const char *pcyclePhaseName() {
  if (!s_pc_active || !s_pc_n) return "";
  if (s_pc_guess >= 0 && s_pc_n <= s_progs[s_pc_guess].n)
    return s_progs[s_pc_guess].st[s_pc_n - 1].name;
  static char b[24];
  cn2core::ObsStage &o = s_pc_obs[s_pc_n - 1];
  stageName(b, sizeof(b), o.loads, o.t3);
  return b;
}
static uint32_t pcRef(cn2core::RefStage *ref) {
  const Prog &P = s_progs[s_pc_guess];
  for (uint8_t i = 0; i < P.n && i < 16; i++)
    ref[i] = { P.st[i].loads, P.st[i].t3, P.st[i].secs };
  return P.n;
}
uint32_t pcycleRemainS() {
  if (!s_pc_active || s_pc_guess < 0) return 0;
  cn2core::RefStage ref[16]; const uint8_t rn = pcRef(ref);
  return cn2core::remainEstSecs(ref, rn, s_pc_n,
                                (millis() - s_pc_stage_at) / 1000);
}
uint32_t pcycleTotalS() {
  if (!s_pc_active || s_pc_guess < 0) return 0;
  cn2core::RefStage ref[16];
  return cn2core::totalEstSecs(ref, pcRef(ref));
}

static void cycleTick() {
  if (s_cyc_state != ST_RUN && s_cyc_state != ST_PAUSE) return;
  uint8_t f[24];
  if (frameSnapshot(FROM_BOARD, f) < 8) return;
  const uint8_t temp = f[1], flow = f[2], st = f[3];

  // Paused by the lid: come back on its own once it is shut, as the machine
  // does. A pause for no water waits for the user, because closing the lid is
  // observable from here and refilling the tank is not.
  if (s_cyc_state == ST_PAUSE) {
    const bool lid_ok = (s_lid_mode == 1) || cn2core::lidClosed(st);
    if (s_cyc_lidpause && lid_ok) cycleResume();
    return;
  }

  if (lastByteAgeMs(FROM_BOARD) > LINK_DEAD_MS) { cycAbort("controller went quiet"); return; }
  // Guard on the byte as FORWARDED, not as received, so this respects exactly
  // the owner's masking decision and nothing more. With the E5 filter on, bit 6
  // is ignored here as it is at the panel; E3, E4, E0 and E7 still abort,
  // because none of those are being suppressed. With the filter off, st_fwd ==
  // st_real and this is the original behaviour.
  if (s_st_fwd & 0x7C)                          { cycAbort("controller fault bit"); return; }
  // The lid guard honours an explicit "force LID ON" override; the fault guard
  // above does not honour anything.
  //
  // The split is deliberate. A lid sensor can be wrong in a way the operator can
  // physically check -- this machine reads 0x80 (reed on, micro not seated) with
  // the lid sitting on but not latched -- so someone standing at the machine can
  // legitimately know better. A controller fault bit is the machine reporting its
  // own hardware, and no amount of standing there makes that judgement better.
  const bool lid_ok = (s_lid_mode == 1) || cn2core::lidClosed(st);
  if (!lid_ok) { cycPause("lid open — resumes when closed", true); return; }
  const uint8_t lim = s_progs[s_mode].maxc < MAX_TEMP_C ? s_progs[s_mode].maxc : MAX_TEMP_C;
  if (temp >= lim)                              { cycAbort("over temperature"); return; }

  Stage &s = STG[s_cyc_i];
  const uint32_t el = (millis() - s_cyc_t0) / 1000;

  if (flow != s_cyc_flow_prev) { s_cyc_flow_prev = flow; s_cyc_flow_ms = millis(); }

  bool done = false;
  if (s.secs == 0) {
    // A fill is over when the count STOPS ADVANCING. That is what the panel
    // itself watches, and it needs no arithmetic.
    //
    // Two earlier rules were both wrong. Comparing against a computed target
    // failed because byte3 = round(counts*0.35) inverts to ~1.4 counts of error:
    // 0x20 predicts 91 where the machine delivers 90. Waiting for the controller
    // to zero the counter deadlocked: it only does that after the intake motor is
    // RELEASED, and the runner is the thing holding it on. Measured 16.2 s of the
    // count sitting at 90 before the controller gave up and reset by itself.
    if (flow > s_cyc_flow_max) s_cyc_flow_max = flow;
    const uint32_t still = millis() - s_cyc_flow_ms;
    const uint8_t ceiling = (uint8_t)(s.t3 / 0.35f) + 10;
    if (s_cyc_flow_max > 0 && flow < s_cyc_flow_max) {
      s_cyc_water = true; done = true;          // controller reset it first
    } else if (s_cyc_flow_max > 0 && still > FILL_SETTLE_MS) {
      s_cyc_water = true; done = true;          // stopped advancing: target met
    } else if (s_cyc_flow_max >= ceiling) {
      cycAbort("fill overran its target"); return;
    } else if (still > FILL_STALL_MS) {
      // Almost always an empty tank. The manual's own recovery is "add water,
      // then press Start/Pause", so do that rather than bin the cycle.
      cycPause("no water — refill the tank, then Resume", false); return;
    }
  } else {
    done = el >= s.secs;
    const Prog &P = s_progs[s_mode];
    // ONE sensor. Byte 1 is the sump NTC and there is no air probe anywhere on
    // this machine -- which is also why the manual lists no maximum temperature
    // for Drying. So the water target is a real setpoint and the dry figure can
    // only be a ceiling on the same reading.
    if (!done && P.water_c && (s.loads & 0x04) && temp >= P.water_c) {
      done = true;                                   // water setpoint reached
    }
    if (P.dry_c && (s.loads & 0x18) && temp >= P.dry_c) {
      cycAbort("dry ceiling reached"); return;
    }
  }
  if (!done) return;

  if (s.loads & 0x02) s_cyc_water = false;      // a drain empties it again

  if (++s_cyc_i >= STG_N) {
    cycRelease(); cycPersistClear(); s_cyc_state = ST_DONE;
    Serial.println("[cycle] complete");
    return;
  }
  Stage &n = STG[s_cyc_i];
  if (n.needs_water && !s_cyc_water) { cycAbort("heater stage with no verified fill"); return; }
  s_cyc_t0 = millis(); s_cyc_flow_ms = millis(); s_cyc_flow_max = 0;
  cycPersist();
  cycApply(n);
  Serial.printf("[cycle] stage %u/%u: %s\n", s_cyc_i+1, STG_N, n.name);
}

static void relayTask(void *) {
  uint32_t prev = micros();
  for (;;) {
    uint32_t now = micros();
    uint32_t gap = now - prev;
    if (gap > s_late_us) { s_late_us = gap; s_late_at = millis(); }
    profGap(gap);
    prev = now;
    profRx((uint32_t)uBoard.available() + (uint32_t)uPanel.available());
    pump();
    wsrService();     // after pump(): acts on the b0 we have just forwarded
    // 1 Hz trend sampling rides on this task rather than a timer, so it can
    // never run while pump() is mid-frame.
    if ((uint32_t)(millis() - s_g_last_ms) >= 1000) {
      s_g_last_ms = millis();
      sampleTrends();
      cycleTick();
      pcycleTick();
    }
    vTaskDelay(1);        // 1 ms: shorter than one 9600-baud character
  }
}

// A one-way failure is either the SoC pin, the level-shifter channel, or the
// copper past it, and from the firmware those look identical -- until you use
// the fact that the BSS138 board carries a 10k pull-up on BOTH sides of every
// channel. That pull-up is the tell:
//
//   pull-down engaged, pin still reads HIGH  -> the shifter's 10k is present,
//                                               so the channel is connected
//   pull-down engaged, pin reads LOW         -> nothing is pulling it up: the
//                                               LV pad is open, or the shifter
//                                               has no 3V3 on LV
//   driving HIGH but it reads back LOW       -> something external is holding
//                                               it down (shorted FET, bridge)
//
// Run it against the KNOWN-GOOD transmit pin first and compare. One pin's
// numbers mean little; the difference between the two means everything.
bool pinProbe(int8_t pin, PinProbe &out) {
  out = PinProbe();
  out.pin = pin;
  if (pin != s_pin_txb && pin != s_pin_txp) {
    snprintf(out.verdict, sizeof(out.verdict),
             "GPIO%d is not one of the transmit pins (%d, %d). The receive pins "
             "have an OEM output on them and must never be driven.",
             (int)pin, (int)s_pin_txb, (int)s_pin_txp);
    return false;
  }

  closePorts();                    // ~45 ms of no forwarding, safely ordered

  pinMode(pin, INPUT_PULLUP);   delay(3); out.pullup   = digitalRead(pin);
  pinMode(pin, INPUT_PULLDOWN); delay(3); out.pulldown = digitalRead(pin);

  // INPUT_OUTPUT, not OUTPUT: Arduino's OUTPUT disables the input buffer, and
  // then digitalRead() returns what we asked for rather than what the pad is
  // actually at -- which is the entire question.
  gpio_set_direction((gpio_num_t)pin, GPIO_MODE_INPUT_OUTPUT);
  gpio_set_level((gpio_num_t)pin, 1); delay(3); out.drive_hi = gpio_get_level((gpio_num_t)pin);
  gpio_set_level((gpio_num_t)pin, 0); delay(3); out.drive_lo = gpio_get_level((gpio_num_t)pin);

  for (int i = 0; i < 200; i++) {
    gpio_set_level((gpio_num_t)pin, i & 1);
    if (gpio_get_level((gpio_num_t)pin) == (i & 1)) out.edges++;
    out.toggles++;
    delayMicroseconds(50);
  }
  gpio_set_level((gpio_num_t)pin, 1);            // UART idles HIGH

  const bool follows = (out.drive_hi == 1 && out.drive_lo == 0 &&
                        out.edges == out.toggles);
  const bool pulled  = (out.pulldown == 1);
  snprintf(out.verdict, sizeof(out.verdict),
           follows
             ? (pulled ? "pin drives and reads back; shifter pull-up present"
                       : "pin drives, but NO external pull-up: LV pad open or "
                         "shifter unpowered")
             : (out.drive_hi == 0 ? "pin cannot be driven HIGH — held low externally"
                                  : "pin does not follow what is written to it"));

  openPorts();
  return follows && pulled;
}

// Close the ports so another task can have the pins.
//
// ORDER MATTERS, and both callers had it backwards: they ended the UARTs and
// THEN cleared s_open, leaving a window in which relayTask -- priority 10, a
// 1 ms cycle -- could be inside pump() holding a driver that was being
// destroyed underneath it. Doing that from the web task wedged the board hard
// enough that only the task watchdog got it back.
//
// pump() returns immediately on !s_open, so clearing the flag first and giving
// relayTask a few of its own cycles to notice is all the interlock needed. No
// suspend: parking a task that may be holding a UART lock is how the deadlock
// gets worse, not better.
static void closePorts() {
  fwFlush(0); fwFlush(1);            // nothing may die in the queue
  s_open = false;
  vTaskDelay(pdMS_TO_TICKS(5));    // >= 4 relayTask cycles
  uBoard.end();
  uPanel.end();
}

uint32_t lockedForMs() { return s_lock_ms ? (millis() - s_lock_ms) : 0; }
bool     pure()       { return s_pure; }
void     setPure(bool on) { s_pure = on; }
uint32_t editC()      { return s_edit_c; }
uint32_t editP()      { return s_edit_p; }
void     resetEdits() { s_edit_c = 0; s_edit_p = 0; }
uint32_t worstGapUs() { return s_late_us; }
uint32_t worstGapAtMs() { return s_late_at; }

// s_ok[] is written by relayTask and read here from the Arduino task. A 32-bit
// aligned load is atomic on RV32 and we only ever compare it against a
// threshold, so a torn read is not possible and a stale one costs one poll.
bool waitLinkSettled(uint16_t frames, uint32_t timeout_ms) {
  const uint32_t t0 = millis();
  while ((uint32_t)(millis() - t0) < timeout_ms) {
    if (s_ok[0] >= frames && s_ok[1] >= frames) {
      Serial.printf("[cn2  ] link settled: %u+ good frames both ways in %lu ms\n",
                    (unsigned)frames, (unsigned long)(millis() - t0));
      return true;
    }
    esp_task_wdt_reset();   // armed before the payload now; do not starve it
    delay(20);          // yields; relayTask is priority 10 and runs freely
  }
  Serial.printf("[cn2  ] link did not settle in %lu ms (ctrl %lu, panel %lu good)"
                " — continuing\n", (unsigned long)timeout_ms,
                (unsigned long)s_ok[0], (unsigned long)s_ok[1]);
  return false;
}
uint32_t txCount(uint8_t dest) { return dest < 2 ? s_tx[dest] : 0; }
uint32_t txBadCount(uint8_t side) { return side < 2 ? s_txbad[side] : 0; }

// Toggled from an esp_timer at twice the requested frequency: one full pulse
// per two callbacks. Counted on the falling edge, which is what the board's
// input sees as a pulse.
static void IRAM_ATTR flowTick(void *) {
  s_flow_level = !s_flow_level;
  digitalWrite(PIN_FLOW_SIM, s_flow_level ? HIGH : LOW);
  if (!s_flow_level) s_flow_pulses++;
}

void flowSet(uint32_t hz) {
  if (s_flow_timer) {
    esp_timer_stop(s_flow_timer);
    esp_timer_delete(s_flow_timer);
    s_flow_timer = nullptr;
  }
  s_flow_hz = hz;
  pinMode(PIN_FLOW_SIM, OUTPUT_OPEN_DRAIN);
  digitalWrite(PIN_FLOW_SIM, HIGH);      // released — the pull-up holds it high
  s_flow_level = true;
  if (hz == 0) { Serial.println("[flow ] simulator OFF"); return; }
  if (hz > 20000) hz = 20000;
  esp_timer_create_args_t a = {};
  a.callback = flowTick;
  a.dispatch_method = ESP_TIMER_TASK;
  a.name = "flowsim";
  if (esp_timer_create(&a, &s_flow_timer) == ESP_OK) {
    esp_timer_start_periodic(s_flow_timer, 500000ULL / hz);   // half-period, us
    Serial.printf("[flow ] simulating %lu Hz on GPIO%d (open-drain)\n",
                  (unsigned long)hz, PIN_FLOW_SIM);
  }
}
uint32_t flowHz()     { return s_flow_hz; }
uint32_t flowPulses() { return s_flow_pulses; }

void simSet(bool on) {
  pinMode(PIN_SW2_SIM, OUTPUT_OPEN_DRAIN);
  digitalWrite(PIN_SW2_SIM, on ? LOW : HIGH);   // LOW = closed, HIGH = released
  s_sim[0] = on;
  Serial.printf("[sim  ] SW2 %s\n", on ? "ASSERTED (closed)" : "released");
}
bool simGet() { return s_sim[0]; }

// ---------------------------------------------------------------------------
// WASH-PUMP RELAY
//
// Drives a relay module on a spare GPIO, because the control board's own
// low-side switch for WS PUMP will not close. In AUTO the relay mirrors b0 of
// the panel frame AS FORWARDED, so it follows the machine's own cycles and the
// cycle runner alike, and any b0 override applies to it too.
//
// The pump has no feedback. Nothing here can tell whether it is actually
// turning, so the two limits below are the whole of the protection: a runtime
// cap, and opening the contacts when the panel link goes quiet. Neither is a
// substitute for water in the tub.
// ---------------------------------------------------------------------------
static inline int wsrLevel(bool on) {
  return s_wsr_low ? (on ? LOW : HIGH) : (on ? HIGH : LOW);
}

// Idle the pin, then take it over. Order matters: setting the level first means
// the pad is already at the OFF level when it becomes an output, so enabling it
// cannot produce a closing glitch.
static void wsrIdlePin(int8_t pin) {
  if (pin < 0) return;
  digitalWrite(pin, wsrLevel(false));
  pinMode(pin, OUTPUT);
  digitalWrite(pin, wsrLevel(false));
}

static void wsrDrive(bool on, const char *why) {
  s_wsr_why = why;
  if (on == s_wsr_on) return;
  digitalWrite(s_wsr_pin, wsrLevel(on));
  s_wsr_on = on;
  if (on) { s_wsr_since = millis(); s_wsr_n++; }
  Serial.printf("[wsr  ] wash pump %s (%s)\n", on ? "ON" : "OFF", why);
}

// b0 as the relay is allowed to see it.
//
// s_panel_b1_fwd updates BYTE BY BYTE, which is fine for the UI and wrong for a
// pump: during startup sync, before the assembler has locked, a stray byte at
// index 1 can carry bit 0 and the contacts close for a few milliseconds. That
// happened -- one unexplained close on a fresh boot with nothing commanded.
//
// So the relay samples b0 only when a panel frame COMPLETES WITH A GOOD
// CHECKSUM, needs two consecutive frames to agree before it changes its mind,
// and ignores the link entirely until a few good frames have been seen. Cost is
// ~400 ms of latency on a pump that takes seconds to do anything.
static uint32_t s_wsr_okprev = 0;
static uint8_t  s_wsr_run    = 0;
static bool     s_wsr_b0     = false;
static uint32_t s_wsr_frames = 0;

static void wsrPanelFrame() {
  const bool valid = (s_asm[1].ok != s_wsr_okprev);
  s_wsr_okprev = s_asm[1].ok;
  if (!valid) { s_wsr_run = 0; return; }   // a bad frame proves nothing
  if (s_wsr_frames < 0xFFFFFFFFu) s_wsr_frames++;
  const bool b0 = (s_panel_b1_fwd & 0x01) != 0;
  if (b0 == s_wsr_b0) { s_wsr_run = 0; return; }
  if (++s_wsr_run >= 2) { s_wsr_b0 = b0; s_wsr_run = 0; }
}

// The safety guards, evaluated once per checksum-valid panel frame.
static uint32_t s_guard_okprev = 0;

static void flushFrame() {
  const bool valid = (s_asm[1].ok != s_guard_okprev);
  s_guard_okprev = s_asm[1].ok;
  if (!valid) return;                     // a bad frame proves nothing

  if (s_heat.frame(s_panel_b1_want, (uint8_t)(s_temp_real & 0x7F)))
    Serial.printf("[heat ] %u C >= ceiling %u — heater bits stripped until %u C\n",
                  (unsigned)(s_temp_real & 0x7F), (unsigned)s_heat.ceiling_c,
                  (unsigned)s_heat.release_c);

  const bool stalled = s_fstall.frame((s_panel_b1_want & 0x20) != 0,
                                      s_flow_real, millis());
  if (stalled)
    Serial.printf("[fill ] NO FLOW for %lu s with intake commanded — intake "
                  "released. Check the tank, then the flow meter.\n",
                  (unsigned long)(s_fstall.stall_ms / 1000));

}
uint32_t wifiDelayMs() { return s_wifi_delay; }
void setWifiDelayMs(uint32_t ms) {
  if (ms > 300000UL) ms = 300000UL;   // never strand the only way back in
  s_wifi_delay = ms;
  s_prefs.begin("d8link", false);
  s_prefs.putULong("wifid", ms);
  s_prefs.end();
  Serial.printf("[wifi] start delay = %lu ms\n", (unsigned long)ms);
}

void setFillStall(uint32_t ms) {
  s_fstall.stall_ms = ms;
  s_fstall.cut = false;
  s_prefs.begin("d8link", false);
  s_prefs.putULong("fstall", ms);
  s_prefs.end();
  Serial.printf("[fill ] stall cutout = %lu ms%s\n", (unsigned long)ms,
                ms ? "" : "  (DISABLED)");
}
void setStuckWatch(uint32_t dwell_ms, uint8_t hot_c, uint16_t off_s) {
  s_stuck.dwell_ms = dwell_ms;
  if (hot_c) s_stuck.hot_c = hot_c;
  if (off_s) s_stuck_off_s = off_s;
  s_stuck.reset();
  s_prefs.begin("d8link", false);
  s_prefs.putULong("stkms", s_stuck.dwell_ms);
  s_prefs.putUChar("stkc", s_stuck.hot_c);
  s_prefs.putUShort("stkoff", s_stuck_off_s);
  s_prefs.end();
  Serial.printf("[stuck] dwell %lu ms, above %u C, cut for %u s%s\n",
                (unsigned long)s_stuck.dwell_ms, (unsigned)s_stuck.hot_c,
                (unsigned)s_stuck_off_s, s_stuck.dwell_ms ? "" : "  (DISABLED)");
}
uint32_t stuckDwellMs(){ return s_stuck.dwell_ms; }
uint8_t  stuckHotC()   { return s_stuck.hot_c; }
uint16_t stuckOffS()   { return s_stuck_off_s; }
uint32_t stuckFires()  { return s_stuck.fires; }
bool     stuckArmed()  { return s_stuck.armed; }

void setHeatCeiling(uint8_t c) {
  s_heat.ceiling_c = c; s_heat.cut = false;
  s_prefs.begin("d8link", false); s_prefs.putUChar("hceil", c); s_prefs.end();
  Serial.printf("[heat ] ceiling = %u C%s\n", (unsigned)c, c ? "" : " (DISABLED)");
}
uint8_t  heatCeilingC()  { return s_heat.ceiling_c; }
bool     heatCeilingCut(){ return s_heat.cut; }
uint32_t heatCeilingCuts(){ return s_heat.cuts; }

uint32_t fillStallMs()  { return s_fstall.stall_ms; }
bool     fillStallCut() { return s_fstall.cut; }
uint32_t fillStallCuts(){ return s_fstall.cuts; }

// Called at ~1 kHz from relayTask.
static void wsrService() {
  const bool synced = s_wsr_frames >= 5;
  const bool cmd = (s_wsr_mode == WSR_ON) ||
                   (s_wsr_mode == WSR_AUTO && synced && s_wsr_b0);
  if (!cmd) s_wsr_lock = false;          // releasing the command rearms the cap

  bool want = cmd && !s_wsr_lock;
  const char *why = cmd ? (s_wsr_mode == WSR_ON ? "forced on" : "panel b0")
                        : (s_wsr_mode == WSR_OFF ? "mode off"
                         : !synced ? "waiting for a synced link" : "b0 clear");
  if (!want && cmd) why = "runtime cap latched";

  // Stale data must never hold the contacts closed. In AUTO the command comes
  // from a frame, so if frames stopped arriving the last b0 we saw is worthless.
  if (want && s_wsr_mode == WSR_AUTO &&
      lastByteAgeMs(FROM_PANEL) > WS_RELAY_LINK_DEAD_MS) {
    want = false; why = "panel link idle";
  }
  if (want && s_wsr_on &&
      (uint32_t)(millis() - s_wsr_since) > WS_RELAY_MAX_ON_MS) {
    want = false; s_wsr_lock = true; why = "runtime cap reached";
  }
  wsrDrive(want, why);
}

void setWsRelayMode(uint8_t m) {
  s_wsr_mode = (m > WSR_AUTO) ? WSR_AUTO : m;
  s_wsr_lock = false;
  s_prefs.begin("d8link", false);
  // ON is deliberately not persisted as ON. Coming back from a crash with the
  // pump latched on is exactly the failure this whole path has to avoid.
  s_prefs.putUChar("wsrm", s_wsr_mode == WSR_ON ? WSR_AUTO : s_wsr_mode);
  s_prefs.end();
  static const char *N[] = {"OFF", "FORCED ON", "AUTO (follow panel b0)"};
  Serial.printf("[wsr  ] mode: %s\n", N[s_wsr_mode]);
  wsrService();
}
uint8_t     wsRelayMode()   { return s_wsr_mode; }
bool        wsRelayOn()     { return s_wsr_on; }
const char *wsRelayWhy()    { return s_wsr_why; }
uint32_t    wsRelayCloses() { return s_wsr_n; }
bool        wsRelayLocked() { return s_wsr_lock; }
uint32_t    wsRelayOnMs()   { return s_wsr_on ? (millis() - s_wsr_since) : 0; }
bool        wsRelayActiveLow() { return s_wsr_low; }
int8_t      wsRelayPin()    { return s_wsr_pin; }

void setWsRelayPolarity(bool active_low) {
  if (active_low == s_wsr_low) return;
  wsrDrive(false, "polarity changed");
  s_wsr_low = active_low;
  wsrIdlePin(s_wsr_pin);
  s_prefs.begin("d8link", false);
  s_prefs.putBool("wsrl", s_wsr_low);
  s_prefs.end();
  Serial.printf("[wsr  ] module is active-%s\n", s_wsr_low ? "LOW" : "HIGH");
  wsrService();
}

void setWsRelayPin(int8_t pin) {
  if (pin < 0 || pin > 21 || pin == s_wsr_pin) return;
  wsrDrive(false, "pin changed");
  wsrIdlePin(s_wsr_pin);                 // leave the old pin in the OFF state
  s_wsr_pin = pin;
  wsrIdlePin(s_wsr_pin);
  s_prefs.begin("d8link", false);
  s_prefs.putChar("wsrp", s_wsr_pin);
  s_prefs.end();
  Serial.printf("[wsr  ] pin -> GPIO%d\n", (int)s_wsr_pin);
}

String lastFrameHex(uint8_t side) {
  uint8_t f[24];
  uint8_t n = frameSnapshot(side, f);
  if (!n) return String("");
  String o; char b[4];
  for (uint8_t i = 0; i < n; i++) {
    snprintf(b, sizeof(b), "%02X", f[i]);
    if (i) o += ' ';
    o += b;
  }
  return o;
}
void     resetGap()   { s_late_us = 0; }

// Self-contained cycle resume after a power cycle. The lockout latch is only
// cleared by a mains cut, the ESP32 dies with that cut (it is powered by the
// same plug), and no external daemon exists any more to call /api/cycle_recover
// afterwards -- the board must decide alone. The gates make a stale or
// malicious resume impossible to reach by accident:
//   - the NVS record only exists while a cycle was genuinely mid-run
//   - the link must be settled (ok_c) and the controller CLEAR
//   - the panel must be idle -- a human running the machine wins instantly
//   - 20 s of stable uptime, so a crash-looping board never pumps water
//   - one-shot: the flag is consumed BEFORE the resume is applied
static bool s_resume_done = false;
static void resumeTick() {
  if (s_resume_done || millis() < 20000) return;
  if (s_cyc_state != ST_IDLE) { s_resume_done = true; return; }
  bool armed; { Preferences p; p.begin("d8link", true); armed = p.getBool("cyc_on", false); p.end(); }
  if (!armed) { s_resume_done = true; return; }
  if (s_asm[0].ok < 50 || (s_st_real & 0x40) || s_panel_b1 != 0) return;
  s_resume_done = true;
  if (cycleRecover())
    Serial.println("[cycle] auto-resumed after power cycle (self-contained)");
}

void loop() {
  resumeTick();
  // Forwarding runs in relayTask(); the only work here is flushing the cycle
  // persistence record, so the flash writes happen in this task, never there.
  const uint8_t want = s_cyc_save;
  if (want) {
    const uint32_t t0 = micros();
    s_cyc_save = 0;
    // Local handle for the same reason as wireSet(): the shared s_prefs can
    // have been closed by any setter's begin()/end() pair, and a cycle whose
    // progress silently fails to persist is a cycle that cannot survive a
    // recovery power cycle.
    Preferences p; p.begin("d8link", false);
    if (want == 1) {
      p.putUChar("cyc_m", s_cyc_save_m);
      p.putUChar("cyc_i", s_cyc_save_i);
      p.putBool("cyc_w", s_cyc_save_w);
      p.putBool("cyc_on", true);
    } else if (p.getBool("cyc_on", false)) {
      p.putBool("cyc_on", false);
    }
    p.end();
    profNote(1, micros() - t0);
  }
}

void setLidMode(uint8_t m) {
  s_lid_mode = (m > 2) ? 0 : m;
  static const char *N[] = {"PASS (real)", "FORCE LID ON", "FORCE LID OFF"};
  Serial.printf("[lid  ] forward mode: %s\n", N[s_lid_mode]);
}
uint8_t lidMode()  { return s_lid_mode; }
bool    lidReal()  { return s_lid_real; }   // as received
bool    lidFwd()   { return s_lid_fwd; }    // as forwarded

void setStatusMask(uint8_t clr, uint8_t set) {
  s_st_clr = clr; s_st_set = set;
  Serial.printf("[stat ] byte3 override: clear 0x%02X, set 0x%02X\n", clr, set);
}
uint8_t statusClr()  { return s_st_clr; }
uint8_t statusSet()  { return s_st_set; }

// ===========================================================================
// PIN AUTODETECT
//
// Phase 1 is passive and cannot damage anything: the ESP32 never drives a line.
// It leans on two facts. First, the two OEM *outputs* are the only CN2 pins with
// edges on them — the OEM inputs sit idle-high because nothing is driving them
// yet. Second, the frame headers name the device: 0xA2 can only be the
// controller, 0xAA can only be the panel, and the XOR proves it is not noise.
//
// Phase 2 identifies the two transmit stubs, which the ESP32 cannot observe
// directly. It forwards on a guess and reads the controller's own E5 bit
// (status byte 3, bit 6) back over the receive path phase 1 just confirmed.
// There are only two permutations, so one trial resolves it.
//
// Cost: the panel latches E5 while this runs and needs a power cycle afterwards.
// The controller's bit clears itself, which is why it is the one used as the
// feedback signal.
// ===========================================================================

static Detect s_det;
static volatile uint32_t s_edge_n[4];
static TaskHandle_t s_det_task = nullptr;

static void IRAM_ATTR edgeIsr(void *arg) { s_edge_n[(uintptr_t)arg]++; }

// Listen on one pin for `ms` and report which frame header validates there.
static uint8_t sniffHeader(HardwareSerial &u, int8_t rx, uint32_t ms) {
  u.begin(s_baud, SERIAL_8N1, rx, -1);
  u.setRxFIFOFull(1);
  u.setRxTimeout(1);
  uint8_t buf[8], n = 0, expect = 0;
  uint32_t a2 = 0, aa = 0, t0 = millis();
  while ((uint32_t)(millis() - t0) < ms) {
    while (u.available()) {
      uint8_t b = (uint8_t)u.read();
      if (n == 0) {
        if (b == 0xA2)      expect = 8;
        else if (b == 0xAA) expect = 5;
        else continue;
      }
      buf[n++] = b;
      if (n >= expect) {
        uint8_t x = 0;
        for (uint8_t i = 0; i + 1 < n; i++) x ^= buf[i];
        if (x == buf[n - 1]) { if (buf[0] == 0xA2) a2++; else aa++; }
        n = 0;
      }
    }
    vTaskDelay(1);
  }
  u.end();
  if (a2 && a2 >= aa) return 0xA2;
  if (aa) return 0xAA;
  return 0;
}

// Forward on the current pin map for `ms`, then report the controller's E5 bit.
// trialClearsE5() below passes when the controller's E5 bit is CLEAR, so it can
// only tell two transmit permutations apart if the bit is SET when the trial
// starts. On a controller that was never faulted, the first permutation passes
// on its first poll -- including when it is the wrong one, which phase 2 then
// writes to NVS. On a board sealed inside an appliance that trades a
// half-working link for a dead one, so provoke the fault first and refuse to
// guess if it never comes.
static bool provokeE5(uint32_t ms) {
  s_pin_txb = -1; s_pin_txp = -1;      // LISTEN: the ports open with no TX pin
  openPorts();
  s_capture = true;
  const uint32_t t0 = millis();
  while ((uint32_t)(millis() - t0) < ms) {
    vTaskDelay(pdMS_TO_TICKS(50));
    if (s_st_real & 0x40) return true;
  }
  return false;
}

static bool trialClearsE5(uint32_t ms) {
  openPorts();
  s_capture = true;
  // s_st_real holds whatever the LAST decoded controller frame said -- including
  // 0 if none has ever been decoded. Sampling it before a fresh frame lands made
  // !(0 & 0x40) true immediately, so the first permutation always looked like a
  // success and the second was never tried. Wait for real frames first.
  const uint32_t start_ok = s_ok[0];
  uint32_t t0 = millis();
  while ((uint32_t)(millis() - t0) < 1200 && (s_ok[0] - start_ok) < 3)
    vTaskDelay(pdMS_TO_TICKS(20));
  if ((s_ok[0] - start_ok) < 3) return false;   // not even hearing the controller

  t0 = millis();
  while ((uint32_t)(millis() - t0) < ms) {
    vTaskDelay(pdMS_TO_TICKS(50));
    if (!(s_st_real & 0x40)) return true;       // E5 cleared -> it hears us
  }
  return false;
}

static void detectTask(void *arg) {
  const bool do_phase2 = (bool)(uintptr_t)arg;
  const int8_t cand[4] = { (int8_t)PIN_RX_BOARD, (int8_t)PIN_TX_BOARD,
                           (int8_t)PIN_TX_PANEL, (int8_t)PIN_RX_PANEL };
  const int8_t save[4] = { s_pin_rxb, s_pin_txb, s_pin_txp, s_pin_rxp };

  s_det = Detect();
  for (int i = 0; i < 4; i++) s_det.cand[i] = cand[i];
  s_det.running = true;
  s_det.phase   = 1;

  // ---- phase 1a: which pins are being driven? ----------------------------
  closePorts();
  for (int i = 0; i < 4; i++) {
    s_edge_n[i] = 0;
    pinMode(cand[i], INPUT);
    attachInterruptArg(digitalPinToInterrupt(cand[i]), edgeIsr,
                       (void *)(uintptr_t)i, CHANGE);
  }
  vTaskDelay(pdMS_TO_TICKS(700));            // >= 3 frame periods on both sides
  for (int i = 0; i < 4; i++) detachInterrupt(digitalPinToInterrupt(cand[i]));

  int act[4], nact = 0;
  for (int i = 0; i < 4; i++) {
    s_det.edge[i] = s_edge_n[i];
    if (s_edge_n[i] > 20) { if (nact < 4) act[nact] = i; nact++; }
  }
  if (nact != 2) {
    snprintf(s_det.note, sizeof(s_det.note),
             nact < 2 ? "Found %d driven pin(s), expected 2. Machine powered? CN2 stubs landed?"
                      : "Found %d driven pins, expected 2. Two ESP32 TX pins may be fighting an OEM output.",
             nact);
    goto restore;
  }

  // ---- phase 1b: which device is on each driven pin? ---------------------
  {
    uint8_t h0 = sniffHeader(uBoard, cand[act[0]], 600);
    uint8_t h1 = sniffHeader(uPanel, cand[act[1]], 600);
    if (h0 == 0xA2 && h1 == 0xAA) { s_det.rx_ctrl = cand[act[0]]; s_det.rx_panel = cand[act[1]]; }
    else if (h0 == 0xAA && h1 == 0xA2) { s_det.rx_ctrl = cand[act[1]]; s_det.rx_panel = cand[act[0]]; }
    else {
      snprintf(s_det.note, sizeof(s_det.note),
               "Both pins are driven but the headers did not resolve (%02X / %02X). "
               "Wrong baud, or a stub on the wrong pin.", h0, h1);
      goto restore;
    }
  }

  // the two silent candidates are the transmit stubs
  {
    int8_t tx[2]; int nt = 0;
    for (int i = 0; i < 4; i++)
      if (cand[i] != s_det.rx_ctrl && cand[i] != s_det.rx_panel && nt < 2) tx[nt++] = cand[i];
    s_det.tx_a = tx[0];
    s_det.tx_b = tx[1];
  }

  if (!do_phase2) {
    snprintf(s_det.note, sizeof(s_det.note),
             "Phase 1 only. RX pins identified; GPIO%d and GPIO%d are the transmit "
             "stubs but which is which is untested.", s_det.tx_a, s_det.tx_b);
    goto restore;
  }

  // ---- phase 2: which transmit stub goes to the controller? --------------
  s_det.phase = 2;
  s_pin_rxb = s_det.rx_ctrl;
  s_pin_rxp = s_det.rx_panel;

  // Starve the controller until it says so. Without this the trial below has
  // nothing to discriminate on -- see provokeE5().
  if (!provokeE5(PHASE2_PROVOKE_MS)) {
    snprintf(s_det.note, sizeof(s_det.note),
             "The controller did not raise E5 after %lu s with nothing transmitted "
             "to it, so there is no signal to resolve the transmit pins against. "
             "Phase 2 cannot run here -- set them by hand with POST /api/pinmap.",
             (unsigned long)(PHASE2_PROVOKE_MS / 1000));
    goto restore;
  }

  s_pin_txb = s_det.tx_a; s_pin_txp = s_det.tx_b;
  if (trialClearsE5(2500)) {
    s_det.tx_ctrl = s_det.tx_a; s_det.tx_panel = s_det.tx_b;
  } else {
    s_pin_txb = s_det.tx_b; s_pin_txp = s_det.tx_a;
    if (trialClearsE5(2500)) {
      s_det.tx_ctrl = s_det.tx_b; s_det.tx_panel = s_det.tx_a;
    }
  }

  if (s_det.tx_ctrl < 0) {
    snprintf(s_det.note, sizeof(s_det.note),
             "Neither transmit permutation cleared the controller's E5 bit. The link "
             "may be broken beyond the pin map — check the converter channels.");
    goto restore;
  }

  s_pin_txb = s_det.tx_ctrl;
  s_pin_txp = s_det.tx_panel;
  s_prefs.begin("d8link", false);
  s_prefs.putChar("prxb", s_pin_rxb); s_prefs.putChar("ptxb", s_pin_txb);
  s_prefs.putChar("ptxp", s_pin_txp); s_prefs.putChar("prxp", s_pin_rxp);
  s_prefs.end();
  s_det.applied = true;
  snprintf(s_det.note, sizeof(s_det.note),
           "Resolved and saved. Power-cycle the machine to clear the panel's latched E5.");
  openPorts();
  s_capture = true;
  s_det.running = false; s_det.done = true;
  s_det_task = nullptr;
  vTaskDelete(nullptr);
  return;

restore:
  s_pin_rxb = save[0]; s_pin_txb = save[1];
  s_pin_txp = save[2]; s_pin_rxp = save[3];
  openPorts();
  s_capture = true;
  s_det.running = false; s_det.done = true;
  s_det_task = nullptr;
  vTaskDelete(nullptr);
}

bool detectStart(bool phase2) {
  if (s_det_task) return false;
  return xTaskCreate(detectTask, "cn2det", 4096, (void *)(uintptr_t)phase2,
                     9, &s_det_task) == pdPASS;
}
const Detect &detectResult() { return s_det; }
void pinMapNow(int8_t &rxb, int8_t &txb, int8_t &txp, int8_t &rxp) {
  rxb = s_pin_rxb; txb = s_pin_txb; txp = s_pin_txp; rxp = s_pin_rxp;
}

void setFlowSpoof(bool on) {
  s_flow_spoof = on;
  Serial.printf("[flow ] panel-facing count %s\n",
                on ? "FORCED TO 0 (E1 no-water)" : "pass-through");
}
bool    flowSpoof()    { return s_flow_spoof; }

void setVirtual(bool on) {
  s_virt = on;
  // The real controller must keep hearing a panel or it raises its own E5, so
  // virtual mode also drives PROBE: it receives the idle frame and nothing else.
  s_probe = on;
  Serial.printf("[virt ] virtual controller %s (probe %s)\n",
                on ? "ON — real controller isolated" : "off", on ? "on" : "off");
}
bool     virtualOn()    { return s_virt; }
uint32_t virtualCount() { return s_virt_n; }
void setVirtualFrame(uint8_t temp, uint8_t flow, uint8_t st, uint8_t b5) {
  s_virt_temp = temp; s_virt_flow = flow; s_virt_st = st; s_virt_b5 = b5;
  s_virt_tempf = temp; s_virt_flowf = flow;      // keep the model in step
}
void setVirtualAuto(bool on) { s_virt_auto = on; s_virt_sim_ms = 0; }
bool virtualAuto()           { return s_virt_auto; }
uint8_t virtTemp()   { return s_virt_temp; }
uint8_t virtFlow()   { return s_virt_flow; }
uint8_t virtStatus() { return s_virt_st; }
uint8_t virtB5()     { return s_virt_b5; }
void setTempOvr(int16_t v) {
  s_temp_ovr = (v > 255) ? 255 : v;
  if (v < 0) s_temp_ovr = -1;
  Serial.printf("[temp ] panel-facing byte1: %s\n",
                s_temp_ovr < 0 ? "pass-through" : String(s_temp_ovr).c_str());
}
int16_t tempOvr()      { return s_temp_ovr; }
uint8_t tempReal()     { return s_temp_real; }
uint8_t tempFwd()      { return s_temp_fwd; }
uint8_t flowCountReal() { return s_flow_real; }
uint8_t flowCountFwd()  { return s_flow_fwd; }

uint8_t statusReal() {
  uint8_t f[24];
  return frameSnapshot(FROM_BOARD, f) >= 8 ? f[3] : 0;
}
uint8_t statusFwd()  { return s_st_fwd; }

void pressButton(uint8_t mask, uint32_t ms) {
  s_press_mask = mask;
  s_press_until = millis() + (ms ? ms : 800);
  Serial.printf("[press] byte1 |= 0x%02X for %lu ms\n", mask, (unsigned long)ms);
}
bool    pressActive() { return s_press_mask && (int32_t)(millis() - s_press_until) < 0; }
void setThin(uint16_t to_panel, uint16_t to_ctrl) {
  s_thin_panel.setEvery(to_panel);
  s_thin_ctrl.setEvery(to_ctrl);
  Serial.printf("[thin ] forward 1-in-%u to panel, 1-in-%u to controller\n",
                s_thin_panel.every, s_thin_ctrl.every);
}
uint16_t thinPanel() { return s_thin_panel.every; }
uint16_t thinCtrl()  { return s_thin_ctrl.every; }

uint32_t frameOk(uint8_t side)  { return s_ok[side & 1]; }
uint32_t frameBad(uint8_t side) { return s_bad[side & 1]; }
void     qualityClear() { s_ok[0] = s_ok[1] = s_bad[0] = s_bad[1] = 0; }

// Read from the completed frame, not from the streaming latches s_panel_b*.
// Those are updated byte by byte as the frame arrives, so byte 1 could already
// be from the new frame while bytes 2-3 were still from the old one.
static uint8_t panelByte(uint8_t idx) {
  uint8_t f[24];
  return frameSnapshot(FROM_PANEL, f) >= 5 ? f[idx] : 0;
}
uint8_t panelB1() { return panelByte(1); }
uint8_t panelB1Fwd() { return s_panel_b1_fwd; }
uint8_t panelB2() { return panelByte(2); }
uint8_t panelB3() { return panelByte(3); }

void setPanelMask(uint8_t clr, uint8_t set) {
  s_p1_clr = clr; s_p1_set = set;
  Serial.printf("[panel] byte1 override: clear 0x%02X, set 0x%02X\n", clr, set);
}
// Set the CN2 pin map directly and persist it. autodetect can work it out, but
// when the answer is already known this avoids trialling permutations against a
// live machine.
void setPinMap(int8_t rxb, int8_t txb, int8_t txp, int8_t rxp) {
  s_pin_rxb = rxb; s_pin_txb = txb; s_pin_txp = txp; s_pin_rxp = rxp;
  s_prefs.begin("d8link", false);
  s_prefs.putChar("prxb", rxb); s_prefs.putChar("ptxb", txb);
  s_prefs.putChar("ptxp", txp); s_prefs.putChar("prxp", rxp);
  s_prefs.end();
  openPorts();
  s_capture = true;
  Serial.printf("[link ] pin map set: rxB=%d txB=%d txP=%d rxP=%d\n",
                rxb, txb, txp, rxp);
}

void setModeOvr(int16_t b2, int16_t b3) {
  s_p2_ovr = b2; s_p3_ovr = b3;
  Serial.printf("[panel] mode override: b2=%d b3=%d\n", b2, b3);
}
int16_t modeOvr2() { return s_p2_ovr; }
int16_t modeOvr3() { return s_p3_ovr; }

uint8_t panelClr() { return s_p1_clr; }
uint8_t panelSet() { return s_p1_set; }

void setProbe(bool on) {
  s_probe = on;
  Serial.printf("[probe] %s — controller %s\n", on ? "ON" : "off",
                on ? "sees an idle panel; presses are NOT forwarded"
                   : "sees the real panel again");
}
bool     probeOn()    { return s_probe; }
uint8_t  lastBtn()    { return s_b1_last; }
uint32_t lastBtnAge() { return s_b1_at ? millis() - s_b1_at : 0xFFFFFFFFUL; }
uint32_t btnCount()   { return s_b1_n; }
void     clearBtn()   { s_b1_last = 0; s_b1_at = 0; s_b1_n = 0; }

void setBaseline() {
  for (uint8_t s2 = 0; s2 < 2; s2++) {
    memcpy(s_base[s2], s_done[s2], s_donen[s2]);
    s_basen[s2] = s_donen[s2];
  }
  s_dln = 0;
  Serial.println("[delta] baseline captured, table cleared");
}
void clearDeltas() { s_dln = 0; }
uint8_t deltaCount() { return s_dln; }

String deltaJson() {
  String o = "[";
  for (uint8_t i = 0; i < s_dln; i++) {
    if (i) o += ',';
    o += "{\"dir\":\"";
    o += (s_dl[i].side == FROM_BOARD) ? "CTRL" : "PANEL";
    o += "\",\"hex\":\"";
    char b[4];
    for (uint8_t k = 0; k < s_dl[i].n; k++) {
      snprintf(b, sizeof(b), "%02X", s_dl[i].b[k]);
      if (k) o += ' ';
      o += b;
    }
    o += "\",\"n\":" + String(s_dl[i].count);
    o += ",\"age\":" + String((millis() - s_dl[i].first_ms) / 1000) + "}";
  }
  return o + "]";
}
String histJson(uint8_t side) {
  if (side > 3) return String("[]");
  String o = "[";
  uint32_t now = millis();
  for (uint8_t i = 0; i < s_histn[side]; i++) {
    const Hist &h = s_hist[side][i];
    if (i) o += ',';
    o += "{\"hex\":\"";
    char b[4];
    for (uint8_t k = 0; k < h.n; k++) {
      snprintf(b, sizeof(b), "%02X", h.b[k]);
      if (k) o += ' ';
      o += b;
    }
    o += "\",\"n\":" + String(h.count);
    o += ",\"first\":" + String((now - h.first_ms) / 100);   // tenths of a sec
    o += ",\"last\":" + String((now - h.last_ms) / 100) + "}";
  }
  return o + "]";
}
void histClear() { for (uint8_t i = 0; i < 4; i++) s_histn[i] = 0; }

String baselineHex(uint8_t side) {
  if (side > 1 || !s_basen[side]) return String("");
  String o; char b[4];
  for (uint8_t i = 0; i < s_basen[side]; i++) {
    snprintf(b, sizeof(b), "%02X", s_base[side][i]);
    if (i) o += ' ';
    o += b;
  }
  return o;
}

bool     open()          { return s_open; }
uint32_t baud()          { return s_baud; }
uint32_t totalCaptured() { return s_head; }
uint32_t ringSize()      { return SNIFF_RING; }
uint32_t overflows()     { return s_overflow; }
uint32_t byteCount(uint8_t s) { return s < 2 ? s_count[s] : 0; }

uint32_t lastByteAgeMs(uint8_t s) {
  if (s > 1 || !s_last_ms[s]) return 0xFFFFFFFFUL;
  return millis() - s_last_ms[s];
}

void clear() {
  s_head = 0;
  s_count[0] = s_count[1] = 0;
  s_overflow = 0;
  s_capture = true;
}

// An idle gap of this many microseconds starts a new frame.
static uint32_t frameGapUs() {
  float charUs = 10.0f * 1000000.0f / (float)s_baud;  // 8N1 = 10 bits/char
  uint32_t g = (uint32_t)(charUs * FRAME_GAP_CHARS);
  return g < FRAME_GAP_MIN_US ? FRAME_GAP_MIN_US : g;
}

String dumpFrames(uint16_t maxFrames) {
  const uint32_t total = s_head;
  if (total == 0) return String("(nothing captured yet)\n");

  const uint32_t have  = total < SNIFF_RING ? total : SNIFF_RING;
  const uint32_t first = total - have;
  const uint32_t gap   = frameGapUs();

  String out;
  out.reserve(6144);
  out += "# baud=" + String(s_baud) +
         "  framegap>" + String(gap) + "us  captured=" + String(total) + "\n";
  out += "# B>P = main board -> front panel      P>B = front panel -> main board\n";
  out += "# t(ms)        dir  bytes\n";

  struct FrameRef { uint32_t start, end; uint8_t side; };
  static FrameRef frames[256];
  const uint16_t cap = maxFrames > 256 ? 256 : maxFrames;
  uint16_t n = 0;

  auto emit = [&](uint32_t s, uint32_t e, uint8_t side) {
    if (n < cap) {
      frames[n++] = {s, e, side};
    } else {
      memmove(&frames[0], &frames[1], sizeof(FrameRef) * (cap - 1));
      frames[cap - 1] = {s, e, side};
    }
  };

  // Frames are tracked per direction: a byte on one line does not split a frame
  // on the other, because the two directions are independent streams.
  uint32_t prevIdx[2]  = {UINT32_MAX, UINT32_MAX};
  uint32_t curStart[2] = {0, 0};

  for (uint32_t i = first; i < total; i++) {
    const Ev &e = s_ring[i % SNIFF_RING];
    uint8_t S = e.side & 1;
    if (prevIdx[S] == UINT32_MAX) {
      curStart[S] = i;
    } else {
      const Ev &p = s_ring[prevIdx[S] % SNIFF_RING];
      if ((uint32_t)(e.t_us - p.t_us) > gap) {
        emit(curStart[S], prevIdx[S], S);
        curStart[S] = i;
      }
    }
    prevIdx[S] = i;
  }
  for (uint8_t S = 0; S < 2; S++)
    if (prevIdx[S] != UINT32_MAX) emit(curStart[S], prevIdx[S], S);

  char buf[16];
  for (uint16_t f = 0; f < n; f++) {
    const FrameRef &fr = frames[f];
    const Ev &e0 = s_ring[fr.start % SNIFF_RING];

    snprintf(buf, sizeof(buf), "%11.3f", e0.t_us / 1000.0);
    out += buf;
    out += (fr.side == FROM_BOARD) ? "  B>P  " : "  P>B  ";

    // fr.start/fr.end are ring indices for THIS side only. Bytes from the other
    // direction can sit between them when both lines transmit at once, so the
    // side tag has to be checked — without it the two streams get spliced
    // together and the dump invents corrupt frames that were never on the wire.
    uint16_t sum = 0, x = 0, len = 0;
    uint8_t  last = 0;
    for (uint32_t i = fr.start; i <= fr.end; i++) {
      const Ev &e = s_ring[i % SNIFF_RING];
      if ((e.side & 1) != fr.side) continue;
      uint8_t b = e.b;
      snprintf(buf, sizeof(buf), "%02X ", b);
      out += buf;
      sum += b; x ^= b;
      last = b;
      len++;
    }
    // the checksum is over everything except the final byte
    sum -= last; x ^= last;
    out += " (" + String(len) + ")";
    if (len > 1) {
      if ((sum & 0xFF) == last) out += " sum-ck";
      if (x == last)            out += " xor-ck";
    }
    out += "\n";
  }
  return out;
}

}  // namespace cn2
