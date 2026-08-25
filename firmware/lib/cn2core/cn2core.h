#pragma once
// Pure CN2 frame logic — no Arduino, no FreeRTOS, no I/O.
//
// This is the part of the relay that can be reasoned about and tested on a host.
// cn2.cpp calls straight into it, so these are the functions that actually run on
// the machine, not a parallel implementation kept in sync by hand.
//
// Everything here is a value transform or a small state machine. Anything that
// touches a UART, a timer, NVS or a task stays in cn2.cpp.

#include <stdint.h>
#include <stddef.h>

namespace cn2core {

// ---------------------------------------------------------------------------
// Framing
// ---------------------------------------------------------------------------

// Both frame types are fixed-length and identified by their first byte. This is
// what makes header+length delimiting possible; gap-based delimiting splits a
// frame whenever the relay task is preempted for longer than the gap threshold.
inline uint8_t frameLenFor(uint8_t header) {
  if (header == 0xA2) return 8;
  if (header == 0xAA) return 5;
  return 0;                      // not a header — junk between frames
}

// Every frame ends in an XOR of the bytes before it.
inline uint8_t xorOf(const uint8_t *p, size_t n) {
  uint8_t x = 0;
  for (size_t i = 0; i < n; i++) x ^= p[i];
  return x;
}

// True when the final byte matches the XOR of everything before it.
inline bool checksumOk(const uint8_t *frame, uint8_t len) {
  if (len < 2) return false;
  return xorOf(frame, len - 1u) == frame[len - 1u];
}

// Assembles bytes into frames and tallies checksum results. A byte lost on the
// wire desyncs us until the next recognised header, which is at most one frame.
struct Assembler {
  uint8_t  buf[8] = {0};
  uint8_t  n = 0, expect = 0;
  uint32_t ok = 0, bad = 0;

  void reset() { n = 0; expect = 0; }
  void clearCounts() { ok = bad = 0; }

  // Returns the completed frame length, or 0 if this byte did not finish one.
  uint8_t feed(uint8_t b) {
    if (n == 0) {
      expect = frameLenFor(b);
      if (!expect) return 0;                 // junk — skip it
    }
    if (n < sizeof(buf)) buf[n++] = b;
    if (n < expect) return 0;
    uint8_t len = n;
    if (checksumOk(buf, len)) ok++; else bad++;
    n = 0;
    return len;
  }
};

// ---------------------------------------------------------------------------
// TX margin — forward only 1 frame in N toward one far end
// ---------------------------------------------------------------------------

// The decision is taken once per frame, at its first byte, so a frame is never
// half-sent. A half-frame would desync the receiver rather than simply starve it.
struct Thinner {
  uint16_t every = 1;            // 1 = forward everything
  uint32_t idx   = 0;
  bool     fwd   = true;

  void setEvery(uint16_t v) { every = v ? v : 1; idx = 0; }
  bool atFrameStart() { fwd = (idx++ % every) == 0; return fwd; }
};

// ---------------------------------------------------------------------------
// Byte rewriting
// ---------------------------------------------------------------------------

inline uint8_t applyMask(uint8_t b, uint8_t clr, uint8_t set) {
  return (uint8_t)((b & (uint8_t)~clr) | set);
}

// Controller -> panel, status byte 3.
//
// The lid has TWO sensors and both report the same way round: bit 1 is a
// reed/magnet switch, bit 7 a micro switch, and SET means lid off on each. The
// machine only treats the lid as closed when both are clear, so an override that
// moved one and left the other would leave it seeing half an open lid. The lid
// override therefore drives both bits together.
static const uint8_t LID_BITS = 0x02 | 0x80;

// Panel byte 1, the load bitmap. Only the two the flush cap needs are named
// here; the full map lives in docs/protocol.md.
static const uint8_t LOAD_DRAIN  = 0x02;
static const uint8_t LOAD_INTAKE = 0x20;

struct StatusOvr {
  uint8_t lid_mode = 0;          // 0 pass, 1 force LID ON (closed), 2 force LID OFF
  uint8_t clr = 0, set = 0;
  bool active() const { return lid_mode || clr || set; }
};

inline uint8_t rewriteStatus(uint8_t b, const StatusOvr &o) {
  uint8_t out = b;
  if (o.lid_mode == 1)      out = (uint8_t)(out & (uint8_t)~LID_BITS);
  else if (o.lid_mode == 2) out = (uint8_t)(out | LID_BITS);
  return applyMask(out, o.clr, o.set);
}

// True only when both lid sensors read closed.
inline bool lidClosed(uint8_t status) { return (status & LID_BITS) == 0; }

// Panel -> controller. Byte 1 is the load bitmap, bytes 2-3 the latched mode word.
struct PanelOvr {
  uint8_t press_mask = 0;
  bool    pressing   = false;
  uint8_t p1_clr = 0, p1_set = 0;
  int16_t p2 = -1, p3 = -1;      // -1 = pass through
  bool    probe = false;         // feed the controller a permanently idle frame

  bool active() const {
    return (pressing && press_mask) || probe || p1_clr || p1_set || p2 >= 0 || p3 >= 0;
  }
};

// ---------------------------------------------------------------------------
// Kasa (TP-Link Smart Home) payload encryption
// ---------------------------------------------------------------------------
// Autokey XOR seeded with 0xAB: each ciphertext byte becomes the key for the
// next. Trivial, and the whole reason an ESP32 can drive one of these plugs
// without a library.
inline void kasaEncrypt(const char *in, uint8_t *out, uint16_t n) {
  uint8_t k = 0xAB;
  for (uint16_t i = 0; i < n; i++) { k ^= (uint8_t)in[i]; out[i] = k; }
}
inline void kasaDecrypt(const uint8_t *in, char *out, uint16_t n) {
  uint8_t k = 0xAB;
  for (uint16_t i = 0; i < n; i++) { out[i] = (char)(k ^ in[i]); k = in[i]; }
}

// ---------------------------------------------------------------------------
// Stuck-load watchdog
// ---------------------------------------------------------------------------
// Why this exists, stated plainly because it cuts mains to an appliance:
//
// This controller raises status bit 6 when a heater is commanded and the
// temperature does not respond -- measured six times across four captures, at
// a dead stall on 44 C during wash+heat and on a flat or falling sump during
// dry. Once it is set the controller stops acting on load commands, INCLUDING
// the panel's end-of-cycle release. The panel says finished and drops every
// bit; the locked controller never processes it, and the blower and air heater
// keep running. Observed holding 52-55 C for 88 minutes.
//
// Nothing on the CN2 link can stop that -- the panel is already commanding
// nothing. The only remaining lever is mains.
//
// The trigger is deliberately narrow. All three must hold, continuously, for
// the full dwell:
//   * the panel is commanding NOTHING. If any load bit is set a cycle is in
//     progress and mains must not be touched, whatever else is true.
//   * the controller is asserting bit 6.
//   * the sump is not cooling. A machine that is merely warm and settling is
//     not stuck, and must not trip this.
struct StuckLoad {
  uint32_t dwell_ms = 0;        // 0 disables
  uint8_t  hot_c    = 40;       // below this, nothing worth cutting power over
  uint32_t since    = 0;
  bool     armed    = false;
  // Latched once fired. Without it the dwell simply restarts and it cuts mains
  // again every dwell_ms for as long as the fault lasts -- a power-cycle loop
  // on an appliance. It only rearms when the trigger conditions genuinely clear.
  bool     latched  = false;
  uint8_t  ref_temp = 0;      // PEAK seen while armed, not the arming value
  uint32_t fires    = 0;

  void reset() { armed = false; latched = false; }

  // Call once per checksum-valid controller frame. Returns true exactly once,
  // on the frame that should cut power.
  bool frame(uint8_t panel_b1, uint8_t status, uint8_t temp_c, uint32_t now) {
    const bool idle    = (panel_b1 == 0);
    const bool locked  = (status & 0x40) != 0;
    const bool hot     = temp_c >= hot_c;
    if (!dwell_ms || !idle || !locked || !hot) {
      armed = false; latched = false; return false;
    }
    if (!armed) { armed = true; latched = false; since = now; ref_temp = temp_c; return false; }
    if (temp_c > ref_temp) ref_temp = temp_c;
    // Cooling means it is settling on its own -- not stuck. Compare against the
    // PEAK, so a sensor dithering by a degree does not read as cooling and a
    // genuine decline does.
    if (temp_c + 2 <= ref_temp) { armed = false; return false; }
    if (!latched && (uint32_t)(now - since) >= dwell_ms) {
      latched = true; fires++; return true;
    }
    return false;
  }
};

// ---------------------------------------------------------------------------
// Over-temperature cutout for PANEL-run cycles
// ---------------------------------------------------------------------------
// This exists because of a trade, and the trade should be written down.
//
// The controller on this machine raises status bit 6 with no cause visible on
// the wire, and the PANEL is what acts on it: measured, the controller set the
// bit and the panel dropped every load 0.2 s later. Masking the bit therefore
// stops the panel aborting -- but it also removes whatever protection that bit
// represents, and nobody knows what that is.
//
// So do not remove a guard without replacing it. If the panel is going to be
// deaf to the controller's objection, something must still be watching the one
// hazard a wash can present: heat with no upper bound. This strips the heater
// bits -- water heat and air heat -- from the forwarded panel frame above a
// ceiling, and holds them off until the sump has dropped clear of it.
//
// The ceiling is above anything this machine has been SEEN to do: its steam
// phase reached 99 C in a captured cycle, so a limit below that would break
// normal operation. This is a backstop against runaway, not a regulator.
struct HeatCeiling {
  uint8_t ceiling_c = 0;      // 0 disables
  uint8_t release_c = 0;      // hysteresis: resume below this
  bool    cut       = false;
  uint32_t cuts     = 0;

  static const uint8_t HEAT_BITS = 0x04 | 0x08;   // water heat | air heat

  // Returns true on the frame that trips it, so the caller logs once.
  bool frame(uint8_t b1_want, uint8_t temp_c) {
    if (!ceiling_c) return false;
    if (cut) { if (temp_c < release_c) cut = false; return false; }
    if ((b1_want & HEAT_BITS) && temp_c >= ceiling_c) {
      cut = true; cuts++; return true;
    }
    return false;
  }
  uint8_t apply(uint8_t b1) const {
    return cut ? (uint8_t)(b1 & (uint8_t)~HEAT_BITS) : b1;
  }
};

// ---------------------------------------------------------------------------
// Fill-stall cutout for PANEL-run cycles
// ---------------------------------------------------------------------------
// Neither end of this link has a fill timeout. Measured on this machine with a
// flow meter that had stopped counting: the panel held the intake bit for
// 1029 s -- seventeen minutes -- waiting for a count that never arrived, and
// raised nothing. The ESP32 cycle runner has had a guard for this since the
// start; a cycle the PANEL runs had none at all, which is the more dangerous
// case because it is the one people actually use.
//
// The dangerous failure is not a dry tank. It is a DEAD FLOW METER with water
// still flowing: the count never advances, the target is never reached, and
// the machine fills until something overflows. From the wire those two look
// identical -- no pulses -- so treat them the same and stop.
//
// Cutting means clearing the intake bit from the forwarded panel frame and
// latching that until the panel releases intake by itself. The drain bit is
// left alone: on a flush that keeps the sump emptying, which is the direction
// you want when you have just decided you cannot measure the water.
struct FillStall {
  uint32_t stall_ms = 0;      // 0 disables
  uint32_t since    = 0;      // when the count last moved
  uint8_t  last     = 0;      // last flow count seen
  // `since` needs a separate armed flag, not a zero sentinel: millis() really
  // is 0 for the first millisecond after boot, and treating that as "unset"
  // silently skipped a frame -- caught by the release test below, which is
  // exactly the window a cycle started straight after power-on would land in.
  bool     armed    = false;
  bool     cut      = false;  // intake is being stripped
  uint32_t cuts     = 0;

  // Call once per checksum-valid panel frame. `intake` is byte 1 BEFORE the
  // cut is applied, or stripping the bit would clear the condition that set
  // it. Returns true on the frame that trips it, so the caller logs once.
  bool frame(bool intake, uint8_t flow, uint32_t now) {
    if (!intake) { cut = false; armed = false; last = flow; return false; }
    if (!armed || flow != last) { last = flow; since = now; armed = true; }
    if (stall_ms && !cut && (uint32_t)(now - since) >= stall_ms) {
      cut = true; cuts++; return true;
    }
    return false;
  }
  uint8_t apply(uint8_t b1) const {
    return cut ? (uint8_t)(b1 & (uint8_t)~LOAD_INTAKE) : b1;
  }
};

// ---------------------------------------------------------------------------
// Where are we in the frame?
// ---------------------------------------------------------------------------
// The rewrite path used to answer this from the WALL-CLOCK GAP between bytes:
// a gap longer than a few milliseconds meant "new frame, index 0". That is
// only true while the CPU keeps up with the wire.
//
// It does not. WiFi association stalls the forwarding task for ~96 ms (a flash
// write disables the instruction cache and no task priority helps). The bytes
// are not lost -- they queue in the UART FIFO -- but they are DRAINED LATE, so
// the gap between the last byte before the stall and the first byte after
// looks enormous. The index reset then fired MID-FRAME, the real byte 3 was
// counted as some other byte, every rewrite keyed to position silently skipped
// it, and the controller's raw status byte went out untouched. On this machine
// that meant one unmasked bit 6 per boot, always around t=3.6 s -- and one
// frame is enough, because the panel latches E5.
//
// So do not ask the clock. Frames here are self-describing: a known header
// gives the length, so count bytes. Nothing the scheduler does can move a
// count. An unrecognised byte at index 0 leaves us parked at 0, scanning for
// the next header, which is how a genuinely desynced stream re-locks without
// ever mis-attributing a position.
struct FramePos {
  uint8_t i = 0, len = 0;

  // Call with each byte as it is about to be processed; returns its index
  // within the frame, or 0xFF when we are not synced to a frame at all.
  uint8_t feed(uint8_t b) {
    if (i == 0) {
      len = frameLenFor(b);
      if (!len) return 0xFF;        // not a header: pass through, keep scanning
    }
    return i;
  }
  // Call after the byte has been emitted.
  void advance() {
    if (!len) return;               // never synced; stay at 0
    if (++i >= len) { i = 0; len = 0; }
  }
  void reset() { i = 0; len = 0; }
  bool insideFrame() const { return len != 0 && i != 0; }
};

// ---------------------------------------------------------------------------
// Frame transmit: the checksum decision, in ONE place
// ---------------------------------------------------------------------------
// Emits a frame byte by byte and fixes the trailing XOR if -- and only if --
// some byte of that frame was actually rewritten.
//
// This exists because the relay used to make that decision inline, from an
// ENUMERATED list of override flags: lid, status mask, flow spoof, temp
// override. When the false-E5 filter was added as a fifth rewrite and not
// added to the list, every masked frame went out carrying the original
// checksum over an edited byte. The far end failed all of them and reported a
// communication failure -- the filter caused the exact fault it was written to
// suppress, and no test could see it because the logic lived in the relay
// rather than here.
//
// The rule is now observational: `rewritten != original` means edited, so a
// rewrite added tomorrow is covered without anyone remembering to declare it.
// A rewrite that happens to produce the same value is NOT an edit, which keeps
// a pass-through relay byte-for-byte transparent.
struct FrameTx {
  uint8_t i = 0, x = 0, len = 0;
  bool    edit = false;

  void start(uint8_t header) { i = 0; x = 0; edit = false; len = frameLenFor(header); }

  // Feed the original byte and what the rewrites made of it; returns the byte
  // to put on the wire.
  uint8_t feed(uint8_t original, uint8_t rewritten) {
    if (rewritten != original) edit = true;
    uint8_t out = rewritten;
    // Unknown header (len 0) never gets a synthesised checksum: we do not know
    // where its frame ends, so we must stay transparent.
    if (edit && len && i + 1 == len) out = x;
    else x ^= out;
    i++;
    return out;
  }
};

// ---------------------------------------------------------------------------
// False-E5 filter
// ---------------------------------------------------------------------------
// This controller raises status bit 6 -- the bit the panel renders as E5,
// "communication failure" -- 2 s after every power-on while the machine is
// still warm, and again the instant a dry stage ends, in the SAME 200 ms
// window the panel goes idle. Neither is a comms failure: the link is
// byte-exact and the frame counters do not miss a beat across either event.
// On this board it reads as a state flag, but the panel has only one meaning
// for it, latches it, and needs a power cycle to clear.
//
// So do not hide the bit -- DISPROVE it. We are the link it is complaining
// about, so when we can show the link is healthy, the claim is false and must
// not be relayed. It is only maskable when every one of these holds:
//
//   * we are a transparent full-rate relay -- not thinning, not probing, not
//     spoofing, not running a virtual controller. Every one of those is us
//     deliberately breaking the link, and a fault raised then is EARNED.
//   * both ends have delivered a checksum-valid frame within fresh_ms
//   * neither direction has logged a bad checksum since the last reset
//
// Fail any of them and the bit passes through untouched, because then we
// cannot prove it wrong.
struct E5Filter {
  bool transparent = false;   // full rate, no probe/spoof/virtual
  bool board_fresh = false;
  bool panel_fresh = false;
  bool clean       = false;   // zero bad checksums both ways
  bool canDisprove() const {
    return transparent && board_fresh && panel_fresh && clean;
  }

  // ---- fail-open was wrong against a LATCHING signal --------------------
  // The first version passed bit 6 straight through the instant it could not
  // disprove it. Observed on the machine: 1152 frames masked, then ONE frame
  // where the check momentarily failed, and that single frame latched E5 on
  // the panel for good.
  //
  // Failing open also does not survive the argument for it. If the ESP->panel
  // link were really broken the panel would not be receiving our frames AT
  // ALL, so what we put in byte 3 could not reach it. The only case where
  // passing bit 6 through actually lands is one where the panel is provably
  // hearing us -- which is exactly when the claim is false.
  //
  // So require the doubt to PERSIST before surrendering the mask. A sustained
  // real fault still gets reported, a few frames later; a one-frame hiccup no
  // longer costs the owner a power cycle.
  uint16_t doubt = 0;
  bool mask(uint16_t need_consecutive) {
    if (canDisprove()) { doubt = 0; return true; }
    if (doubt < 0xFFFF) doubt++;
    return doubt < need_consecutive;
  }
};

// ---------------------------------------------------------------------------
// Panel-cycle program identification
// ---------------------------------------------------------------------------
// When a cycle is started FROM THE PANEL, nothing on the wire carries the
// remaining time -- the panel keeps its own timer and never transmits it. What
// the wire does carry is the load bitmap and the fill target of every stage,
// and the six stock programs differ in exactly those: first fill 0x20 is a
// wash, 0x07 is steam, 0x23 is self-clean, and Rapid only separates from
// Normal at the third fill it does not have. So the program can be identified
// by prefix-matching the observed stages against the known tables, and the
// countdown ESTIMATED from the reference durations. It is an estimate and the
// UI must say so -- the panel's own timer is the authority.
struct ObsStage { uint8_t loads; uint8_t t3; };
struct RefStage { uint8_t loads; uint8_t t3; uint32_t secs; };  // secs 0 = fill

// A fill runs until its target lands: target counts = t3/0.35 at the measured
// 2.24 counts/s, plus a little slack for the water to start moving.
inline uint32_t stageEstSecs(const RefStage &r) {
  if (r.secs) return r.secs;
  return (uint32_t)((r.t3 * 20u + 6u) / 7u * 100u / 224u) + 8u;
}

// How many observed stages match this program's prefix. -1 = ruled out.
// A fill stage must also match its target; a non-fill stage matches on the
// load bitmap alone.
inline int matchProgram(const ObsStage *obs, uint8_t n,
                        const RefStage *ref, uint8_t rn) {
  if (n > rn) return -1;
  for (uint8_t i = 0; i < n; i++) {
    if (obs[i].loads != ref[i].loads) return -1;
    if (ref[i].loads == 0x20 && ref[i].t3 && obs[i].t3 &&
        obs[i].t3 != ref[i].t3) return -1;
  }
  return n;
}

// Seconds left, assuming obs[n-1] is ref[n-1] and has run cur_elapsed_s.
inline uint32_t remainEstSecs(const RefStage *ref, uint8_t rn, uint8_t n,
                              uint32_t cur_elapsed_s) {
  if (n == 0 || n > rn) return 0;
  const uint32_t cur = stageEstSecs(ref[n - 1]);
  uint32_t left = (cur_elapsed_s < cur) ? cur - cur_elapsed_s : 0;
  for (uint8_t i = n; i < rn; i++) left += stageEstSecs(ref[i]);
  return left;
}

inline uint32_t totalEstSecs(const RefStage *ref, uint8_t rn) {
  uint32_t t = 0;
  for (uint8_t i = 0; i < rn; i++) t += stageEstSecs(ref[i]);
  return t;
}

// ---------------------------------------------------------------------------
// Frame-atomic transmit coalescing
// ---------------------------------------------------------------------------
// The relay used to emit each rewritten byte the moment it arrived. That has
// zero added latency, but it couples the WIRE to the CPU: anything that stalls
// the forwarding task mid-frame -- measured 96 ms during WiFi association,
// when a flash write disables the instruction cache and no task priority
// helps -- splits the frame ON THE WIRE, and a receiver with a short
// inter-byte timeout reads that as a comms failure.
//
// So coalesce: buffer the rewritten bytes and hand the UART the whole frame in
// one write. A frame is at most 8 bytes and the TX FIFO holds 128, so the
// hardware clocks it out back-to-back with no further CPU involvement. A stall
// can now only delay a frame (a one-off 296 ms beat instead of 200), never
// split one. Cost: the first byte leaves ~7 byte-times (~7.3 ms) later than it
// used to, against a 200 ms broadcast period.
//
// Transparency rule: a byte with an unknown header flushes immediately, so
// noise and foreign traffic still pass byte-for-byte as before.
struct TxCoalesce {
  uint8_t buf[16];
  uint8_t n    = 0;
  uint8_t want = 0;      // expected frame length; 0 = unknown header
  bool    emit = true;   // false once any byte of the frame was suppressed

  // Append one rewritten byte. Returns true when buf[0..n) must be written now
  // (frame complete, unknown header, or buffer full). Caller writes if emit,
  // then calls clear().
  bool feed(uint8_t b, bool fwd) {
    if (n == 0) { want = frameLenFor(b); emit = fwd; }
    if (!fwd) emit = false;
    buf[n++] = b;
    return want == 0 || n >= want || n >= (uint8_t)sizeof(buf);
  }
  void clear() { n = 0; want = 0; emit = true; }
};

// idx is the byte's position in the 5-byte frame (0 = header, 4 = checksum).
inline uint8_t rewritePanelByte(uint8_t idx, uint8_t b, const PanelOvr &o) {
  uint8_t out = b;
  if (idx == 1) {
    if (o.pressing) out = (uint8_t)(out | o.press_mask);
    out = applyMask(out, o.p1_clr, o.p1_set);
  } else if (idx == 2 && o.p2 >= 0) {
    out = (uint8_t)o.p2;
  } else if (idx == 3 && o.p3 >= 0) {
    out = (uint8_t)o.p3;
  }
  // PROBE wins over everything: nothing the user presses reaches the controller.
  if (o.probe && idx >= 1 && idx <= 3) out = 0x00;
  return out;
}

// ---------------------------------------------------------------------------
// Streaming checksum
// ---------------------------------------------------------------------------

// Recomputes the trailing XOR without ever buffering the frame: accumulate each
// emitted byte, and at the checksum position emit the accumulator instead. Zero
// added latency, which matters because the link is timing-sensitive.
//
// When nothing is being overridden the original checksum byte passes through
// untouched, so a pass-through relay is byte-for-byte transparent.
struct StreamRewriter {
  uint8_t i = 0, x = 0;

  void reset() { i = 0; x = 0; }

  // `last` is the index of the checksum byte: 7 for 0xA2, 4 for 0xAA.
  uint8_t step(uint8_t out, uint8_t last, bool overriding) {
    uint8_t emit = out;
    if (overriding && i == last) emit = x;
    else                         x ^= out;
    i++;
    return emit;
  }
};

// ---------------------------------------------------------------------------
// Pin autodetect
// ---------------------------------------------------------------------------

enum Device : uint8_t { DEV_UNKNOWN = 0, DEV_CONTROLLER = 1, DEV_PANEL = 2 };

inline Device deviceFromHeader(uint8_t header) {
  if (header == 0xA2) return DEV_CONTROLLER;
  if (header == 0xAA) return DEV_PANEL;
  return DEV_UNKNOWN;
}

// Only the two OEM *outputs* have edges on them; the OEM inputs sit idle-high
// because nothing is driving them yet.
inline bool isDriven(uint32_t edges, uint32_t threshold = 20) {
  return edges > threshold;
}

// Given the two sniffed headers and the pins they came from, decide which pin is
// the controller's and which is the panel's. Fails closed if the headers do not
// resolve — a wrong guess here is how you wire a TX pin to an output.
inline bool resolveRx(uint8_t h0, uint8_t h1, int8_t pin0, int8_t pin1,
                      int8_t &rx_ctrl, int8_t &rx_panel) {
  Device d0 = deviceFromHeader(h0), d1 = deviceFromHeader(h1);
  if (d0 == DEV_CONTROLLER && d1 == DEV_PANEL) { rx_ctrl = pin0; rx_panel = pin1; return true; }
  if (d0 == DEV_PANEL && d1 == DEV_CONTROLLER) { rx_ctrl = pin1; rx_panel = pin0; return true; }
  return false;
}

}  // namespace cn2core
