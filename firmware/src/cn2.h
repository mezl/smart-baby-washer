#pragma once
#include <Arduino.h>

// The CN2 panel link: sits between the D8 main board and its front panel,
// forwards every byte both ways, rewrites bytes in flight, and logs everything.
//
// Man-in-the-middle only. Both CN2 signal wires are cut and all four stubs come
// to the ESP32, so each wire has exactly one driver.
//
// A passive LISTEN mode used to exist here, opening both UARTs with TX = -1 so
// the firmware could not drive a line while the pin directions were still
// unknown. It was removed: with the wires cut there is nothing to eavesdrop on,
// so selecting it only made both OEM boards raise E5. /api/detect now covers the
// unknown-wiring case properly, and its scan is passive.
namespace cn2 {

// Ring/dump line indices.
enum Side : uint8_t { FROM_BOARD = 0, FROM_PANEL = 1 };

void begin();
void loop();

// Stop filling the ring but leave both UARTs open, so TX stays driven idle-high.
// Closing them would let the panel's RX line float, which reads as a break.
void quiesce();

void     setBaud(uint32_t baud);
uint32_t baud();

bool     open();
uint32_t byteCount(uint8_t side);
uint32_t lastByteAgeMs(uint8_t side);
uint32_t totalCaptured();
uint32_t ringSize();
uint32_t overflows();
void     clear();

// Newest `maxFrames` frames, grouped on an idle gap and annotated with sum and
// XOR checksum candidates — appliance protocols nearly always use one of the two.
String dumpFrames(uint16_t maxFrames);

// ---- transmit --------------------------------------------------------------
enum Dest : uint8_t { TO_BOARD = 0, TO_PANEL = 1 };
size_t sendTo(uint8_t dest, const uint8_t *data, size_t n);

// Impersonate the panel's idle frame toward the main board. This is the
// diagnostic that separates "our TX path is broken" from "the panel is silent":
// if the board's E5 flag clears while we spoof, D3 -> board RX works.
void     setSpoof(bool on, uint32_t period_ms);
// The frame sent while spoofing. Defaults to the panel's idle frame. While
// spoof is ON the real panel's traffic is NOT forwarded to the board.
void     setSpoofFrame(const uint8_t *d, size_t n);
String   spoofFrameHex();
bool     spoofOn();
uint32_t spoofPeriod();
uint32_t spoofCount();

// Worst observed interval between forwarding passes. If this climbs into the
// milliseconds the relay is being starved and the board will flag E5.
// ---- transmit-stub pin test ----------------------------------------------
// Localises a one-way link failure without a meter. Only the two TRANSMIT pins
// may be tested: the receive pins have an OEM output on them and driving those
// is how boards die.
struct PinProbe {
  int8_t   pin      = -1;
  bool     ran      = false;
  int8_t   pullup   = -1;  // level with the ESP32's own pull-UP engaged
  int8_t   pulldown = -1;  // ...and its pull-DOWN. The shifter's 10k beats it.
  int8_t   drive_hi = -1;  // level read back while driving HIGH
  int8_t   drive_lo = -1;  // ...and LOW
  uint32_t toggles  = 0;
  uint32_t edges    = 0;   // edges the pin's own input path saw us make
  char     verdict[160] = {0};
};
bool     pinProbe(int8_t pin, PinProbe &out);

uint32_t worstGapUs();
// millis() at which that worst gap was recorded. A stall during WiFi
// association is harmless once the machine's startup handshake is already
// done, and dangerous before it -- the size alone cannot tell you which.
uint32_t worstGapAtMs();
// Bytes actually written out by write(): TO_BOARD / TO_PANEL. If this is not
// climbing, the firmware is not transmitting and no amount of rewiring helps.
uint32_t txCount(uint8_t dest);

// ---- flow-meter simulator -------------------------------------------------
// hz = 0 stops it. Frequency is runtime-settable because the board's expected
// K-factor is unknown — sweep it and watch the board's status byte.
void     flowSet(uint32_t hz);
uint32_t flowHz();
uint32_t flowPulses();

// ---- switch simulators ----------------------------------------------------
// Open-drain: asserting pulls the board's input to ground, i.e. "switch closed".
// Releasing hands control back to the real switch.
//
// SW1 no longer has a pin. The carrier board's flow-relay port takes the two
// GPIOs on the XIAO's right-hand column (10 and 20), which leaves only GPIO7 --
// and the lid switch is worth more than the unidentified one. SW1 is still
// reachable on the XIAO's own header pin if you socket the module.
void simSet(bool on);
bool simGet();

// ---- wash-pump relay, external --------------------------------------------
// The control board asserts 24 V on the WS PUMP header permanently and switches
// the low side. That low-side switch does not close when panel byte 1 b0 is
// commanded -- verified with a byte-identical frame to the one the machine
// sends during its own wash phase. So the pump gets its own relay, hung off a
// spare GPIO, and this is the driver for it.
//
// AUTO is the mode that matters: the relay follows b0 in the panel frame *as
// forwarded*, so the machine's own cycles and the ESP32 cycle runner both drive
// the pump without either of them knowing the relay exists.
enum : uint8_t { WSR_OFF = 0, WSR_ON = 1, WSR_AUTO = 2 };
void        setWsRelayMode(uint8_t m);
uint8_t     wsRelayMode();
bool        wsRelayOn();          // true = contacts closed, pump energised
const char *wsRelayWhy();         // why it is in that state, for the UI
uint32_t    wsRelayOnMs();        // how long the current close has lasted
uint32_t    wsRelayCloses();      // lifetime count, for contact wear
bool        wsRelayLocked();      // tripped the runtime cap, waiting for release
// Wiring, persisted in NVS. Changing the pin re-idles the old one first.
void    setWsRelayPolarity(bool active_low);
bool    wsRelayActiveLow();
void    setWsRelayPin(int8_t pin);
int8_t  wsRelayPin();

// ---- untargeted-flush cap -------------------------------------------------
// A 0xFF flush has no volume target and no timeout at either end; see
// FLUSH_CAP_MS_DEFAULT in config.h. After the cap the intake bit is stripped
// from the forwarded panel byte 1 and the drain bit is left set.
void     setFlushCap(uint32_t ms);   // 0 disables, persisted to NVS
uint32_t flushCapMs();
bool     flushActive();              // a 0xFF flush is being forwarded now
uint32_t flushMs();                  // how long it has run, 0 when not flushing
bool     flushCapped();              // intake is currently being held down
uint32_t flushCaps();                // lifetime count of caps fired

// Last complete frame seen in each direction, as a hex string.
String lastFrameHex(uint8_t side);

// Rewrite the board->panel frame so the PANEL is told the lid is on. This only
// changes what the panel DISPLAYS — the controller still reads the real switch
// and will still refuse to run with the lid off.
// 0 = forward the controller's real lid bit unchanged
// 1 = tell the panel LID ON   (bit 1 cleared)
// 2 = tell the panel LID OFF  (bit 1 set)
// Only the PANEL is affected. The controller reads the real switch regardless.
void    setLidMode(uint8_t m);
uint8_t lidMode();
bool    lidReal();   // lid-off bit as received from the controller
bool    lidFwd();    // lid-off bit as forwarded to the panel

// General rewrite of the status byte (byte 3) on its way to the panel:
//     forwarded = (real & ~clear) | set
// Errors travel controller -> panel, so this can suppress or fabricate any of
// them on the display. It does NOT change what the controller believes.
void    setStatusMask(uint8_t clr, uint8_t set);
uint8_t statusClr();
uint8_t statusSet();
uint8_t statusReal();   // byte 3 as received

// ---- trend buffers for the page's graphs -----------------------------------
// Temperature: 1800 samples at 1 Hz. Bit 7 of each byte is "water heater
// commanded" -- the temperature has never exceeded 97 so the bit is free.
// Flow: 60 samples of the pulse count at 1 Hz.
String  trendTempHex();   // bit 7 = water heater commanded
String  trendAirHex();    // 01 per sample = air heater or dry commanded
String  trendFlowHex();
uint8_t trendCutout();        // temperature when the heater was last released
bool    trendFlowActive();    // pulse count moved within the last 10 s

// Every change of the flow count with a timestamp, captured as frames arrive so
// no count can be missed by a slow HTTP poll. 256 events, oldest first.
String   flowLogJson();
void     flowLogClear();
uint16_t flowLogLen();
uint8_t statusFwd();    // byte 3 as forwarded

// Hold the flow-meter pulse count seen by the PANEL at zero. E1 "no water" is a
// panel-side verdict: it watches this counter while it is commanding the intake
// motor. Starving it reproduces a genuine no-water fault.
//
// Only the starve direction exists. Faking the count upward would convince the
// machine it had filled when it had not, which is the dry-fire path.
void    setFlowSpoof(bool on);
bool    flowSpoof();
uint8_t flowCountReal();   // byte 2 as received
uint8_t flowCountFwd();    // byte 2 as forwarded

// Force byte 1, the temperature the panel sees. -1 restores pass-through.
// The manual's sensor faults are the two ends of an NTC divider, so an
// out-of-range value here is the way to provoke them without unplugging a
// thermistor.
// ---- virtual controller ---------------------------------------------------
// Synthesise the controller->panel frame here and drop the real controller's.
// The panel sees a machine reporting exactly what you set; the real controller
// is put in PROBE at the same time, so it keeps hearing an idle panel and stays
// E5-free while being unable to receive a single command.
//
// The mirror of PROBE mode. Together the machine is electrically inert while the
// panel still behaves as though everything is normal.
void     setVirtual(bool on);
bool     virtualOn();
uint32_t virtualCount();
void     setVirtualFrame(uint8_t temp, uint8_t flow, uint8_t st, uint8_t b5);
// AUTO runs a measured model of the machine -- filling raises the flow count,
// heating raises the temperature -- so the panel can complete a whole cycle
// against nothing. Most error codes are only evaluated mid-cycle, so this is
// what makes them reachable with the machine inert.
void     setVirtualAuto(bool on);
bool     virtualAuto();
uint8_t  virtTemp();
uint8_t  virtFlow();
uint8_t  virtStatus();
uint8_t  virtB5();

// ---- cycle runner ---------------------------------------------------------
// Drives a whole cycle from the ESP32: the panel is held idle and this decides
// every load, every fill target and every stage boundary.
//
// The machine protects none of it. The controller gates nothing except the
// intake motor, the heaters fire with no water and the lid open, and neither end
// has a fill timeout. Every interlock lives in the runner:
//
//   a heater stage will not start unless its fill completed
//   a fill aborts if the flow count stalls
//   over temperature, lid open, controller fault bit or a dead link all abort
//
// Abort clears every override and releases every load.
void        cycleStart();
void        cycleStop();
// Resume a PAUSED cycle at the stage and elapsed time it stopped at.
//
// A lid pause resumes itself once the lid shuts, as the machine does. A no-water
// pause waits for this, because the lid is observable from here and a refilled
// tank is not.
void        cycleResume();
bool        cycleRunning();
uint8_t     cycleState();      // 0 idle, 1 running, 2 done, 3 aborted, 4 paused
uint8_t     cycleStage();
uint8_t     cycleCount();
uint32_t    cycleElapsed();    // seconds in the current stage
const char *cycleWhy();        // abort reason, empty otherwise
const char *cycleName(uint8_t i);
uint32_t    cycleSecs(uint8_t i);
uint8_t     cycleLoads(uint8_t i);
uint8_t     cycleTgt(uint8_t i);
void        cycleSetSecs(uint8_t i, uint32_t s);   // persisted to NVS, per program
// The manual's six programs (p.25-26). Each carries its own stage list, its own
// saved durations, and the manual's maximum water temperature as an extra abort
// ceiling on top of the global one.
uint8_t     cycleModeCount();
uint8_t     cycleMode();
void        cycleSetMode(uint8_t m);               // ignored while running

// ---- user-defined programs -------------------------------------------------
// Two editable slots, appended after the six built-ins. The spec is
// "loads:target:seconds" per stage, comma separated, loads and target in hex.
//
// Every list is validated before it is accepted. The rules encode what the
// captures established -- intake needs a fill target, 0xFF needs the drain open,
// the water heater needs a fill since the last drain -- because a hand-written
// stage list is where this machine's missing interlocks would bite.
//
// Returns nullptr on success, or the reason it was rejected.
const char *cycleSetCustom(uint8_t slot, const char *name, const char *spec);
uint8_t     cycleCustomSlots();
uint8_t     cycleCustomFirst();    // program index of the first custom slot
bool        cycleModeEmpty(uint8_t m);        // a free slot is not a program
const char *cycleDelCustom(uint8_t slot);     // built-ins cannot be deleted
const char *cycleStageSpec(uint8_t i);
const char *cycleModeName(uint8_t m);
uint8_t     cycleModeMaxC(uint8_t m);
// Temperature targets, per program, persisted.
//
// There is ONE sensor on this machine: byte 1, the sump NTC. There is no air
// probe, which is also why the manual lists no maximum temperature for Drying.
// So `water` is a real setpoint -- a heat stage ends when it is reached -- and
// `dry` can only be a CEILING on that same reading, which aborts.
uint8_t     cycleWaterC();
uint8_t     cycleDryC();
void        cycleSetTemps(int16_t water, int16_t dry);   // -1 leaves one alone
void        cycleLoad();                           // restore saved durations
void        cycleResetSecs();                      // back to the compiled defaults

void    setTempOvr(int16_t v);
int16_t tempOvr();
uint8_t tempReal();
uint8_t tempFwd();

// Inject a momentary button press: OR `mask` into byte 1 of the panel->board
// frame for `ms`, recomputing that frame's checksum. The panel's own selected
// modes (bytes 2,3) pass through untouched.
void    pressButton(uint8_t mask, uint32_t ms);
bool    pressActive();
uint8_t panelB1();   // live panel frame bytes, for learning button codes
// Byte 1 as actually FORWARDED. The UI used to predict this from pb1 and the
// override masks, which stopped being true the moment the flush cap could
// subtract a bit the masks know nothing about.
uint8_t panelB1Fwd();
uint8_t panelB2();
uint8_t panelB3();
// Per-direction frame checksum tally. side 0 = controller, 1 = panel.
uint32_t frameOk(uint8_t side);
uint32_t frameBad(uint8_t side);
void     qualityClear();

// ---- pin autodetect -------------------------------------------------------
// Phase 1 is passive (the ESP32 never drives a line) and identifies the two
// receive pins from frame headers. Phase 2 identifies the two transmit stubs by
// forwarding on a guess and reading the controller's E5 bit back.
struct Detect {
  bool     running = false;
  bool     done    = false;
  uint8_t  phase   = 0;
  uint32_t edge[4] = {0, 0, 0, 0};
  int8_t   cand[4] = {-1, -1, -1, -1};
  int8_t   rx_ctrl = -1, rx_panel = -1;
  int8_t   tx_a    = -1, tx_b     = -1;   // the two silent stubs
  int8_t   tx_ctrl = -1, tx_panel = -1;   // resolved by phase 2
  bool     applied = false;
  char     note[200] = "";
};
bool          detectStart(bool phase2);
const Detect &detectResult();
void          pinMapNow(int8_t &rxb, int8_t &txb, int8_t &txp, int8_t &rxp);
// Set the map directly when it is already known, instead of trialling for it.
void          setPinMap(int8_t rxb, int8_t txb, int8_t txp, int8_t rxp);
// TX margin test: forward only 1 frame in N toward each far end. 1 = normal.
void     setThin(uint16_t to_panel, uint16_t to_ctrl);
uint16_t thinPanel();
uint16_t thinCtrl();

// Rewrite byte 1 on its way panel->controller:  out = (b & ~clr) | set
// Bit 5 (0x20) is RUN: set while the cycle proceeds, cleared when the panel
// gives up and waits for the user (E1 / water icon). Holding it SET is the
// auto-retry override; clearing it forces a pause.
void    setPanelMask(uint8_t clr, uint8_t set);
// Bytes 2 and 3 of the panel frame — the latched mode word. -1 forwards the
// panel's own value; 0..255 substitutes a literal. Setting 0x40/0x20 is what
// makes the controller assert RUN.
void    setModeOvr(int16_t b2, int16_t b3);
int16_t modeOvr2();
int16_t modeOvr3();
uint8_t panelClr();
uint8_t panelSet();

// PROBE: feed the controller a permanently idle panel frame while still logging
// what the real panel sends. Press any button safely to learn its code — nothing
// reaches the controller, so no cycle can start.
void     setProbe(bool on);
bool     probeOn();
uint8_t  lastBtn();      // most recent non-zero byte 1
uint32_t lastBtnAge();   // ms since it was seen
uint32_t btnCount();     // distinct codes seen
void     clearBtn();

// ---- delta detector -------------------------------------------------------
// Snapshot the current frames as "idle", then every distinct frame that differs
// is recorded with a first-seen time and a hit count. The direct way to ask
// "did anything change at all while I did X?"
void    setBaseline();
void    clearDeltas();
uint8_t deltaCount();
String  deltaJson();
String  baselineHex(uint8_t side);

// Per-direction run-length history. Repeats tick a counter; changes append a row
// that persists, so the session's distinct frames stay visible.
String  histJson(uint8_t side);
void    histClear();
void     resetGap();   // clears the worst-gap high-water mark

// Block until BOTH ends have been heard from with good checksums, or until the
// timeout. Yields throughout, so relayTask keeps forwarding while we wait.
// Returns false on timeout, which is not fatal -- it only means the link was
// quiet, e.g. on a bench with no machine attached.
bool     waitLinkSettled(uint16_t frames, uint32_t timeout_ms);

// Hold the radio off for this long after the link settles, forwarding all the
// while. Exists to test one thing: WiFi association stalls relayTask for ~96 ms
// and no task priority beats it, so if a far end faults on a gap that size, the
// fault must move with the delay. Persisted, so it is settable without a
// reflash -- which matters when the only way in is the radio you are delaying.
// 0 = normal.
uint32_t wifiDelayMs();
void     setWifiDelayMs(uint32_t ms);

}  // namespace cn2
