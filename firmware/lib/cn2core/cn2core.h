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
