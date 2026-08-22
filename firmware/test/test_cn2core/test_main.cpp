// Host-side tests for cn2core — the pure CN2 frame logic that cn2.cpp calls.
//
// Frames used here are real captures from the machine, not invented:
//   A2 23 00 02 04 0D 02 88   controller, idle, lid off
//   A2 18 00 03 04 0D 02 B2   controller, running
//   A2 18 00 42 04 0E 02 F0   controller, lid off + E5
//   AA 00 00 00 AA            panel, idle
//   AA 20 40 20 EA            panel, intake motor + mode word
#include <unity.h>
#include <cn2core.h>
#include <string.h>

using namespace cn2core;

static const uint8_t CTRL_IDLE[8] = {0xA2,0x23,0x00,0x02,0x04,0x0D,0x02,0x88};
static const uint8_t CTRL_RUN [8] = {0xA2,0x18,0x00,0x03,0x04,0x0D,0x02,0xB2};
static const uint8_t CTRL_E5  [8] = {0xA2,0x18,0x00,0x42,0x04,0x0E,0x02,0xF0};
static const uint8_t PANEL_IDLE[5] = {0xAA,0x00,0x00,0x00,0xAA};
static const uint8_t PANEL_RUN [5] = {0xAA,0x20,0x40,0x20,0xEA};

// ---------------------------------------------------------------- framing ---

void test_frameLenFor_known_headers(void) {
  TEST_ASSERT_EQUAL_UINT8(8, frameLenFor(0xA2));
  TEST_ASSERT_EQUAL_UINT8(5, frameLenFor(0xAA));
}

void test_frameLenFor_rejects_junk(void) {
  TEST_ASSERT_EQUAL_UINT8(0, frameLenFor(0x00));
  TEST_ASSERT_EQUAL_UINT8(0, frameLenFor(0xFF));
  TEST_ASSERT_EQUAL_UINT8(0, frameLenFor(0xA3));   // one bit off a real header
}

void test_xorOf_matches_captured_checksums(void) {
  TEST_ASSERT_EQUAL_UINT8(0x88, xorOf(CTRL_IDLE, 7));
  TEST_ASSERT_EQUAL_UINT8(0xF0, xorOf(CTRL_E5,   7));
  TEST_ASSERT_EQUAL_UINT8(0xEA, xorOf(PANEL_RUN, 4));
}

// The idle panel frame satisfies both a sum and an XOR check because every
// payload byte is zero. Guards the documented trap of identifying the algorithm
// from all-zero captures.
void test_idle_panel_frame_is_ambiguous(void) {
  uint8_t sum = 0;
  for (int i = 0; i < 4; i++) sum += PANEL_IDLE[i];
  TEST_ASSERT_EQUAL_UINT8(PANEL_IDLE[4], sum);
  TEST_ASSERT_EQUAL_UINT8(PANEL_IDLE[4], xorOf(PANEL_IDLE, 4));
}

void test_checksumOk_accepts_real_frames(void) {
  TEST_ASSERT_TRUE(checksumOk(CTRL_IDLE, 8));
  TEST_ASSERT_TRUE(checksumOk(CTRL_RUN,  8));
  TEST_ASSERT_TRUE(checksumOk(PANEL_IDLE,5));
  TEST_ASSERT_TRUE(checksumOk(PANEL_RUN, 5));
}

void test_checksumOk_rejects_single_bit_flip(void) {
  uint8_t bad[8]; memcpy(bad, CTRL_IDLE, 8);
  bad[3] ^= 0x01;                                  // corrupt the status byte
  TEST_ASSERT_FALSE(checksumOk(bad, 8));
}

void test_checksumOk_rejects_runt(void) {
  TEST_ASSERT_FALSE(checksumOk(CTRL_IDLE, 1));
  TEST_ASSERT_FALSE(checksumOk(CTRL_IDLE, 0));
}

// --------------------------------------------------------------- assembler ---

void test_assembler_completes_a_frame(void) {
  Assembler a;
  for (int i = 0; i < 7; i++) TEST_ASSERT_EQUAL_UINT8(0, a.feed(CTRL_IDLE[i]));
  TEST_ASSERT_EQUAL_UINT8(8, a.feed(CTRL_IDLE[7]));
  TEST_ASSERT_EQUAL_UINT32(1, a.ok);
  TEST_ASSERT_EQUAL_UINT32(0, a.bad);
}

void test_assembler_counts_a_corrupt_frame(void) {
  Assembler a;
  uint8_t bad[8]; memcpy(bad, CTRL_IDLE, 8); bad[5] ^= 0x20;
  for (int i = 0; i < 8; i++) a.feed(bad[i]);
  TEST_ASSERT_EQUAL_UINT32(0, a.ok);
  TEST_ASSERT_EQUAL_UINT32(1, a.bad);
}

void test_assembler_skips_junk_before_a_header(void) {
  Assembler a;
  a.feed(0x00); a.feed(0x7F); a.feed(0xA3);        // none are headers
  TEST_ASSERT_EQUAL_UINT8(0, a.n);
  for (int i = 0; i < 5; i++) a.feed(PANEL_RUN[i]);
  TEST_ASSERT_EQUAL_UINT32(1, a.ok);
}

// A byte lost on the wire must cost at most one frame, then resync.
void test_assembler_resyncs_after_a_dropped_byte(void) {
  Assembler a;
  for (int i = 1; i < 8; i++) a.feed(CTRL_IDLE[i]);   // header dropped
  for (int i = 0; i < 5; i++) a.feed(PANEL_RUN[i]);
  for (int i = 0; i < 5; i++) a.feed(PANEL_RUN[i]);
  TEST_ASSERT_TRUE(a.ok >= 1);                        // recovered
}

void test_assembler_handles_both_frame_types_interleaved(void) {
  Assembler a;
  for (int i = 0; i < 8; i++) a.feed(CTRL_IDLE[i]);
  for (int i = 0; i < 5; i++) a.feed(PANEL_RUN[i]);
  for (int i = 0; i < 8; i++) a.feed(CTRL_RUN[i]);
  TEST_ASSERT_EQUAL_UINT32(3, a.ok);
  TEST_ASSERT_EQUAL_UINT32(0, a.bad);
}

void test_assembler_reset_and_clear(void) {
  Assembler a;
  a.feed(0xA2); a.feed(0x23);
  a.reset();
  TEST_ASSERT_EQUAL_UINT8(0, a.n);
  for (int i = 0; i < 5; i++) a.feed(PANEL_IDLE[i]);
  TEST_ASSERT_EQUAL_UINT32(1, a.ok);
  a.clearCounts();
  TEST_ASSERT_EQUAL_UINT32(0, a.ok);
  TEST_ASSERT_EQUAL_UINT32(0, a.bad);
}

// ----------------------------------------------------------------- thinner ---

void test_thinner_passes_everything_by_default(void) {
  Thinner t;
  for (int i = 0; i < 10; i++) TEST_ASSERT_TRUE(t.atFrameStart());
}

void test_thinner_forwards_one_in_four(void) {
  Thinner t; t.setEvery(4);
  int fwd = 0;
  for (int i = 0; i < 40; i++) if (t.atFrameStart()) fwd++;
  TEST_ASSERT_EQUAL_INT(10, fwd);
}

void test_thinner_first_frame_always_forwards(void) {
  Thinner t; t.setEvery(7);
  TEST_ASSERT_TRUE(t.atFrameStart());
}

// setEvery(0) must not divide by zero.
void test_thinner_zero_is_treated_as_one(void) {
  Thinner t; t.setEvery(0);
  TEST_ASSERT_EQUAL_UINT16(1, t.every);
  for (int i = 0; i < 5; i++) TEST_ASSERT_TRUE(t.atFrameStart());
}

void test_thinner_setEvery_restarts_the_phase(void) {
  Thinner t; t.setEvery(3);
  t.atFrameStart(); t.atFrameStart();
  t.setEvery(3);
  TEST_ASSERT_TRUE(t.atFrameStart());
}

// ---------------------------------------------------------------- rewriting ---

void test_applyMask_clear_then_set(void) {
  TEST_ASSERT_EQUAL_UINT8(0x20, applyMask(0x00, 0x00, 0x20));
  TEST_ASSERT_EQUAL_UINT8(0x00, applyMask(0x20, 0x20, 0x00));
  TEST_ASSERT_EQUAL_UINT8(0x02, applyMask(0x22, 0x20, 0x00));
  TEST_ASSERT_EQUAL_UINT8(0x01, applyMask(0x01, 0x00, 0x00));   // pass-through
}

// set wins over clr when both name the same bit.
void test_applyMask_set_beats_clear(void) {
  TEST_ASSERT_EQUAL_UINT8(0x08, applyMask(0x00, 0x08, 0x08));
}

void test_rewriteStatus_passthrough(void) {
  StatusOvr o;
  TEST_ASSERT_FALSE(o.active());
  TEST_ASSERT_EQUAL_UINT8(0x02, rewriteStatus(0x02, o));
}

// The lid override must move BOTH sensors. Driving only the reed leaves the
// machine seeing a closed reed and an open micro switch, which is not "lid shut".
void test_rewriteStatus_forces_lid_on_clears_both(void) {
  StatusOvr o; o.lid_mode = 1;
  TEST_ASSERT_TRUE(o.active());
  TEST_ASSERT_EQUAL_UINT8(0x00, rewriteStatus(0x02, o));       // reed only
  TEST_ASSERT_EQUAL_UINT8(0x00, rewriteStatus(0x80, o));       // micro only
  TEST_ASSERT_EQUAL_UINT8(0x00, rewriteStatus(0x82, o));       // both
  TEST_ASSERT_EQUAL_UINT8(0x01, rewriteStatus(0x83, o));       // bit 0 survives
}

void test_rewriteStatus_forces_lid_off_sets_both(void) {
  StatusOvr o; o.lid_mode = 2;
  TEST_ASSERT_EQUAL_UINT8(0x82, rewriteStatus(0x00, o));
  TEST_ASSERT_EQUAL_UINT8(0x82, rewriteStatus(0x02, o));       // already half set
  TEST_ASSERT_EQUAL_UINT8(0xC3, rewriteStatus(0x41, o));       // E5 + bit 0 survive
}

void test_lidClosed_needs_both_sensors(void) {
  TEST_ASSERT_TRUE (lidClosed(0x00));
  TEST_ASSERT_FALSE(lidClosed(0x02));                          // reed says off
  TEST_ASSERT_FALSE(lidClosed(0x80));                          // micro says off
  TEST_ASSERT_FALSE(lidClosed(0x82));
  TEST_ASSERT_TRUE (lidClosed(0x41));                          // E5 set, lid still shut
}

void test_rewriteStatus_can_clear_e5(void) {
  StatusOvr o; o.clr = 0x40;
  TEST_ASSERT_EQUAL_UINT8(0x02, rewriteStatus(0x42, o));
}

void test_rewriteStatus_mask_applies_after_lid(void) {
  StatusOvr o; o.lid_mode = 2; o.clr = 0x82;                   // contradictory
  TEST_ASSERT_EQUAL_UINT8(0x00, rewriteStatus(0x00, o));       // clr wins, it is last
}

void test_rewritePanelByte_passthrough(void) {
  PanelOvr o;
  TEST_ASSERT_FALSE(o.active());
  for (uint8_t i = 0; i < 5; i++)
    TEST_ASSERT_EQUAL_UINT8(PANEL_RUN[i], rewritePanelByte(i, PANEL_RUN[i], o));
}

void test_rewritePanelByte_injects_a_press(void) {
  PanelOvr o; o.pressing = true; o.press_mask = 0x20;
  TEST_ASSERT_TRUE(o.active());
  TEST_ASSERT_EQUAL_UINT8(0x20, rewritePanelByte(1, 0x00, o));
  TEST_ASSERT_EQUAL_UINT8(0x22, rewritePanelByte(1, 0x02, o));  // OR, not replace
}

void test_rewritePanelByte_forces_a_load_bit_off(void) {
  PanelOvr o; o.p1_clr = 0x20;
  TEST_ASSERT_EQUAL_UINT8(0x00, rewritePanelByte(1, 0x20, o));
}

void test_rewritePanelByte_only_touches_byte_1(void) {
  PanelOvr o; o.p1_set = 0xFF;
  TEST_ASSERT_EQUAL_UINT8(0xAA, rewritePanelByte(0, 0xAA, o));  // header
  TEST_ASSERT_EQUAL_UINT8(0x40, rewritePanelByte(2, 0x40, o));  // mode word
  TEST_ASSERT_EQUAL_UINT8(0xEA, rewritePanelByte(4, 0xEA, o));  // checksum slot
}

void test_rewritePanelByte_forces_the_mode_word(void) {
  PanelOvr o; o.p2 = 0x40; o.p3 = 0x20;
  TEST_ASSERT_TRUE(o.active());
  TEST_ASSERT_EQUAL_UINT8(0x40, rewritePanelByte(2, 0x00, o));
  TEST_ASSERT_EQUAL_UINT8(0x20, rewritePanelByte(3, 0x00, o));
}

// p2 = 0 must force zero, not read as "pass through" — that is why it is int16_t.
void test_rewritePanelByte_mode_zero_is_a_real_value(void) {
  PanelOvr o; o.p2 = 0;
  TEST_ASSERT_TRUE(o.active());
  TEST_ASSERT_EQUAL_UINT8(0x00, rewritePanelByte(2, 0x40, o));
}

void test_probe_blanks_the_payload_and_wins(void) {
  PanelOvr o; o.probe = true; o.pressing = true; o.press_mask = 0x20; o.p1_set = 0xFF;
  TEST_ASSERT_EQUAL_UINT8(0xAA, rewritePanelByte(0, 0xAA, o));
  TEST_ASSERT_EQUAL_UINT8(0x00, rewritePanelByte(1, 0x20, o));
  TEST_ASSERT_EQUAL_UINT8(0x00, rewritePanelByte(2, 0x40, o));
  TEST_ASSERT_EQUAL_UINT8(0x00, rewritePanelByte(3, 0x20, o));
}

void test_pressing_without_a_mask_is_not_active(void) {
  PanelOvr o; o.pressing = true; o.press_mask = 0x00;
  TEST_ASSERT_FALSE(o.active());
}

// --------------------------------------------------------- stream rewriter ---

void test_stream_passthrough_is_byte_identical(void) {
  StreamRewriter r;
  for (uint8_t i = 0; i < 8; i++)
    TEST_ASSERT_EQUAL_UINT8(CTRL_IDLE[i], r.step(CTRL_IDLE[i], 7, false));
}

void test_stream_recomputes_checksum_when_overriding(void) {
  StreamRewriter r;
  StatusOvr o; o.lid_mode = 1;                     // 0x02 -> 0x00
  uint8_t out[8];
  for (uint8_t i = 0; i < 8; i++) {
    uint8_t b = CTRL_IDLE[i];
    if (i == 3) b = rewriteStatus(b, o);
    out[i] = r.step(b, 7, true);
  }
  TEST_ASSERT_EQUAL_UINT8(0x00, out[3]);
  TEST_ASSERT_TRUE(checksumOk(out, 8));            // still a valid frame
  TEST_ASSERT_EQUAL_UINT8(0x8A, out[7]);           // 0x88 ^ 0x02
}

void test_stream_recomputes_panel_checksum(void) {
  StreamRewriter r;
  PanelOvr o; o.p1_set = 0x20;
  uint8_t out[5];
  for (uint8_t i = 0; i < 5; i++)
    out[i] = r.step(rewritePanelByte(i, PANEL_IDLE[i], o), 4, true);
  TEST_ASSERT_EQUAL_UINT8(0x20, out[1]);
  TEST_ASSERT_TRUE(checksumOk(out, 5));
}

void test_stream_reset_between_frames(void) {
  StreamRewriter r;
  for (uint8_t i = 0; i < 8; i++) r.step(CTRL_IDLE[i], 7, true);
  r.reset();
  TEST_ASSERT_EQUAL_UINT8(0, r.i);
  TEST_ASSERT_EQUAL_UINT8(0, r.x);
  uint8_t out[5];
  for (uint8_t i = 0; i < 5; i++) out[i] = r.step(PANEL_RUN[i], 4, true);
  TEST_ASSERT_TRUE(checksumOk(out, 5));
}

// Rewriting a byte to the value it already had must leave the frame untouched.
void test_stream_noop_override_is_identity(void) {
  StreamRewriter r;
  uint8_t out[8];
  for (uint8_t i = 0; i < 8; i++) out[i] = r.step(CTRL_IDLE[i], 7, true);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(CTRL_IDLE, out, 8);
}

// ---------------------------------------------------------------- autodetect ---

void test_deviceFromHeader(void) {
  TEST_ASSERT_EQUAL_INT(DEV_CONTROLLER, deviceFromHeader(0xA2));
  TEST_ASSERT_EQUAL_INT(DEV_PANEL,      deviceFromHeader(0xAA));
  TEST_ASSERT_EQUAL_INT(DEV_UNKNOWN,    deviceFromHeader(0x00));
}

void test_isDriven_threshold(void) {
  TEST_ASSERT_FALSE(isDriven(0));
  TEST_ASSERT_FALSE(isDriven(20));                 // boundary: not driven
  TEST_ASSERT_TRUE (isDriven(21));
  TEST_ASSERT_TRUE (isDriven(108));                // real measurement, GPIO3
  TEST_ASSERT_TRUE (isDriven(66));                 // real measurement, GPIO6
}

void test_resolveRx_both_orders(void) {
  int8_t c = -1, p = -1;
  TEST_ASSERT_TRUE(resolveRx(0xA2, 0xAA, 3, 6, c, p));
  TEST_ASSERT_EQUAL_INT8(3, c);
  TEST_ASSERT_EQUAL_INT8(6, p);

  c = p = -1;
  TEST_ASSERT_TRUE(resolveRx(0xAA, 0xA2, 3, 6, c, p));
  TEST_ASSERT_EQUAL_INT8(6, c);
  TEST_ASSERT_EQUAL_INT8(3, p);
}

// Fails closed rather than guessing — a wrong guess wires a TX pin to an output.
void test_resolveRx_refuses_ambiguous_input(void) {
  int8_t c = -1, p = -1;
  TEST_ASSERT_FALSE(resolveRx(0xA2, 0xA2, 3, 6, c, p));   // both controller
  TEST_ASSERT_FALSE(resolveRx(0xAA, 0xAA, 3, 6, c, p));   // both panel
  TEST_ASSERT_FALSE(resolveRx(0x00, 0xAA, 3, 6, c, p));   // one silent
  TEST_ASSERT_EQUAL_INT8(-1, c);
  TEST_ASSERT_EQUAL_INT8(-1, p);
}

// ---------------------------------------------------- end-to-end relay path ---

// One full frame through assemble -> rewrite -> stream, the way pump() runs it.
void test_end_to_end_forced_intake_motor(void) {
  Assembler in;
  StreamRewriter r;
  PanelOvr o; o.p1_set = 0x20;                     // force the intake motor on
  uint8_t out[5];

  for (uint8_t i = 0; i < 5; i++) {
    in.feed(PANEL_IDLE[i]);
    out[i] = r.step(rewritePanelByte(i, PANEL_IDLE[i], o), 4, o.active());
  }
  TEST_ASSERT_EQUAL_UINT32(1, in.ok);              // input was valid
  TEST_ASSERT_EQUAL_UINT8(0xAA, out[0]);
  TEST_ASSERT_EQUAL_UINT8(0x20, out[1]);
  TEST_ASSERT_TRUE(checksumOk(out, 5));            // output is valid too

  Assembler back;                                   // the controller's view
  for (uint8_t i = 0; i < 5; i++) back.feed(out[i]);
  TEST_ASSERT_EQUAL_UINT32(1, back.ok);
  TEST_ASSERT_EQUAL_UINT32(0, back.bad);
}

// A thinned stream must emit whole frames only — never a partial one.
void test_thinned_stream_emits_whole_frames(void) {
  Thinner t; t.setEvery(3);
  int emitted = 0, frames = 0;
  Assembler rx;
  for (int f = 0; f < 9; f++) {
    bool fwd = t.atFrameStart();
    for (uint8_t i = 0; i < 5; i++)
      if (fwd) { emitted++; if (rx.feed(PANEL_RUN[i])) frames++; }
  }
  TEST_ASSERT_EQUAL_INT(15, emitted);              // 3 frames x 5 bytes
  TEST_ASSERT_EQUAL_INT(3, frames);
  TEST_ASSERT_EQUAL_UINT32(0, rx.bad);
}


// ---------------------------------------------------------------------------
// Untargeted-flush cap
// ---------------------------------------------------------------------------
// A flush frame: intake commanded, fill target 0xFF. Anything else is not.
static const uint8_t FLUSH_B1 = cn2core::LOAD_DRAIN | cn2core::LOAD_INTAKE; // 0x22

static void test_flushcap_ignores_a_targeted_fill(void) {
  cn2core::FlushCap f; f.cap_ms = 1000;
  // 0x20 with a real target is an ordinary metered fill and must never be
  // capped -- the controller ends those itself on the flow count.
  for (uint32_t t = 0; t < 10000; t += 200)
    TEST_ASSERT_FALSE(f.frame(cn2core::LOAD_INTAKE, 0x20, t));
  TEST_ASSERT_FALSE(f.on);
  TEST_ASSERT_FALSE(f.hold);
  TEST_ASSERT_EQUAL_UINT32(0, f.fired);
}

static void test_flushcap_ignores_a_drain_without_intake(void) {
  cn2core::FlushCap f; f.cap_ms = 1000;
  for (uint32_t t = 0; t < 10000; t += 200)
    TEST_ASSERT_FALSE(f.frame(cn2core::LOAD_DRAIN, 0xFF, t));
  TEST_ASSERT_FALSE(f.on);
}

static void test_flushcap_fires_once_at_the_cap(void) {
  cn2core::FlushCap f; f.cap_ms = 1000;
  uint32_t trips = 0;
  for (uint32_t t = 0; t <= 3000; t += 200)
    if (f.frame(FLUSH_B1, 0xFF, t)) trips++;
  TEST_ASSERT_EQUAL_UINT32(1, trips);        // exactly one log line
  TEST_ASSERT_EQUAL_UINT32(1, f.fired);
  TEST_ASSERT_TRUE(f.hold);
  TEST_ASSERT_TRUE(f.on);
}

static void test_flushcap_does_not_fire_early(void) {
  cn2core::FlushCap f; f.cap_ms = 1000;
  for (uint32_t t = 0; t < 1000; t += 200)
    TEST_ASSERT_FALSE(f.frame(FLUSH_B1, 0xFF, t));
  TEST_ASSERT_FALSE(f.hold);
  TEST_ASSERT_TRUE(f.frame(FLUSH_B1, 0xFF, 1000));   // exactly at the cap
}

static void test_flushcap_strips_intake_and_keeps_the_drain(void) {
  cn2core::FlushCap f; f.cap_ms = 1000;
  TEST_ASSERT_EQUAL_HEX8(FLUSH_B1, f.apply(FLUSH_B1));   // before the cap
  for (uint32_t t = 0; t <= 1000; t += 200) f.frame(FLUSH_B1, 0xFF, t);
  // This is the whole point: the drain keeps running, so the sump empties and
  // the controller ends the stage the way it always has.
  TEST_ASSERT_EQUAL_HEX8(cn2core::LOAD_DRAIN, f.apply(FLUSH_B1));
}

static void test_flushcap_stays_latched_on_its_own_output(void) {
  // The trap this struct exists to avoid: fed the POST-cap byte, the trigger
  // condition reads false, the state clears, the intake bit comes back, and the
  // pair oscillates at the frame rate. b1_want must be the pre-cap value.
  cn2core::FlushCap f; f.cap_ms = 1000;
  for (uint32_t t = 0; t <= 1000; t += 200) f.frame(FLUSH_B1, 0xFF, t);
  TEST_ASSERT_TRUE(f.hold);
  for (uint32_t t = 1200; t <= 5000; t += 200) f.frame(FLUSH_B1, 0xFF, t);
  TEST_ASSERT_TRUE(f.hold);                 // still held, never re-armed
  TEST_ASSERT_EQUAL_UINT32(1, f.fired);     // and only counted once
}

static void test_flushcap_survives_one_dropped_frame(void) {
  // One non-flush frame mid-flush must NOT restart the clock. That is the one
  // error that would make the cap useless in exactly the case it exists for.
  cn2core::FlushCap f; f.cap_ms = 1000;
  f.frame(FLUSH_B1, 0xFF, 0);
  f.frame(FLUSH_B1, 0xFF, 200);
  f.frame(0x00, 0x00, 400);                 // one bad/idle frame
  TEST_ASSERT_TRUE(f.on);                   // still counting
  f.frame(FLUSH_B1, 0xFF, 600);
  TEST_ASSERT_TRUE(f.frame(FLUSH_B1, 0xFF, 1000));   // capped on the original t0
}

static void test_flushcap_releases_after_two_clear_frames(void) {
  cn2core::FlushCap f; f.cap_ms = 1000;
  for (uint32_t t = 0; t <= 1000; t += 200) f.frame(FLUSH_B1, 0xFF, t);
  TEST_ASSERT_TRUE(f.hold);
  f.frame(0x00, 0x00, 1200);
  TEST_ASSERT_TRUE(f.on);                   // one is not enough
  f.frame(0x00, 0x00, 1400);
  TEST_ASSERT_FALSE(f.on);
  TEST_ASSERT_FALSE(f.hold);
  TEST_ASSERT_EQUAL_HEX8(FLUSH_B1, f.apply(FLUSH_B1));
}

static void test_flushcap_rearms_for_the_next_flush(void) {
  cn2core::FlushCap f; f.cap_ms = 1000;
  for (uint32_t t = 0; t <= 1000; t += 200) f.frame(FLUSH_B1, 0xFF, t);
  f.frame(0x00, 0x00, 1200); f.frame(0x00, 0x00, 1400);
  uint32_t trips = 0;
  for (uint32_t t = 2000; t <= 3200; t += 200)
    if (f.frame(FLUSH_B1, 0xFF, t)) trips++;
  TEST_ASSERT_EQUAL_UINT32(1, trips);
  TEST_ASSERT_EQUAL_UINT32(2, f.fired);
}

static void test_flushcap_zero_disables(void) {
  cn2core::FlushCap f; f.cap_ms = 0;
  for (uint32_t t = 0; t <= 600000; t += 1000) f.frame(FLUSH_B1, 0xFF, t);
  TEST_ASSERT_TRUE(f.on);                   // still tracked, for the UI
  TEST_ASSERT_FALSE(f.hold);                // but never acted on
  TEST_ASSERT_EQUAL_UINT32(0, f.fired);
  TEST_ASSERT_EQUAL_HEX8(FLUSH_B1, f.apply(FLUSH_B1));
}

static void test_flushcap_survives_millis_rollover(void) {
  // millis() wraps every 49.7 days and the machine is expected to sit powered.
  cn2core::FlushCap f; f.cap_ms = 1000;
  const uint32_t t0 = 0xFFFFFF00u;          // 256 ms before the wrap
  f.frame(FLUSH_B1, 0xFF, t0);
  TEST_ASSERT_FALSE(f.hold);
  TEST_ASSERT_TRUE(f.frame(FLUSH_B1, 0xFF, (uint32_t)(t0 + 1000)));   // wrapped
}

static void test_flushcap_default_clears_a_real_flush(void) {
  // The longest flush ever measured on this machine is 114.6 s (Self-Clean),
  // and the cycle runner's own longest is 116 s. The 180 s default must not
  // truncate either.
  cn2core::FlushCap f; f.cap_ms = 180000;
  for (uint32_t t = 0; t <= 116000; t += 200)
    TEST_ASSERT_FALSE(f.frame(FLUSH_B1, 0xFF, t));
  TEST_ASSERT_FALSE(f.hold);
}


// ---------------------------------------------------------------------------
// Frame-atomic transmit coalescing
// ---------------------------------------------------------------------------
static void test_txq_holds_a_ctrl_frame_until_complete(void) {
  cn2core::TxCoalesce q;
  const uint8_t f[8] = {0xA2,0x19,0x00,0x02,0x04,0x0D,0x02,0xB2};
  for (int i = 0; i < 7; i++)
    TEST_ASSERT_FALSE(q.feed(f[i], true));      // nothing leaves early
  TEST_ASSERT_TRUE(q.feed(f[7], true));         // whole frame, in one write
  TEST_ASSERT_EQUAL_UINT8(8, q.n);
  TEST_ASSERT_TRUE(q.emit);
  TEST_ASSERT_EQUAL_MEMORY(f, q.buf, 8);
}

static void test_txq_panel_frame_is_five_bytes(void) {
  cn2core::TxCoalesce q;
  const uint8_t f[5] = {0xAA,0x00,0x40,0x20,0xCA};
  for (int i = 0; i < 4; i++) TEST_ASSERT_FALSE(q.feed(f[i], true));
  TEST_ASSERT_TRUE(q.feed(f[4], true));
  TEST_ASSERT_EQUAL_UINT8(5, q.n);
}

static void test_txq_unknown_header_passes_through_at_once(void) {
  // Transparency: noise and foreign traffic must not be held hostage to a
  // frame length nobody knows.
  cn2core::TxCoalesce q;
  TEST_ASSERT_TRUE(q.feed(0x55, true));
  TEST_ASSERT_EQUAL_UINT8(1, q.n);
}

static void test_txq_suppression_drops_the_whole_frame(void) {
  // Thinning decides per frame; a frame with any suppressed byte must vanish
  // entirely rather than reach the wire as a fragment.
  cn2core::TxCoalesce q;
  const uint8_t f[5] = {0xAA,0x00,0x40,0x20,0xCA};
  for (int i = 0; i < 4; i++) q.feed(f[i], i != 2);   // one byte suppressed
  TEST_ASSERT_TRUE(q.feed(f[4], true));
  TEST_ASSERT_FALSE(q.emit);
}

static void test_txq_clear_rearms(void) {
  cn2core::TxCoalesce q;
  q.feed(0xAA, false); q.clear();
  TEST_ASSERT_TRUE(q.emit);
  TEST_ASSERT_EQUAL_UINT8(0, q.n);
  TEST_ASSERT_TRUE(q.feed(0x99, true));   // unknown header again flushes at once
}

static void test_txq_never_overruns_its_buffer(void) {
  cn2core::TxCoalesce q;
  q.feed(0xA2, true);                     // wants 8...
  int flushes = 0;
  for (int i = 0; i < 40; i++) if (q.feed(0x00, true)) { flushes++; q.clear(); }
  TEST_ASSERT_GREATER_THAN(0, flushes);   // ...but a liar of a sender still flushes
}


// ---------------------------------------------------------------------------
// Panel-cycle program identification
// ---------------------------------------------------------------------------
// Reference tables trimmed to what the tests exercise: Normal and Rapid share
// their entire opening -- drain, fill 0x20, wash, drain, fill 0x1C, rinse --
// and only separate where Normal has a third fill.
static const cn2core::RefStage R_NORMAL[] = {
  {0x02,0,20},{0x20,0x20,0},{0x05,0,600},{0x02,0,20},{0x20,0x1C,0},
  {0x05,0,480},{0x02,0,20},{0x20,0x1C,0},{0x05,0,480},{0x02,0,20}};
static const cn2core::RefStage R_RAPID[] = {
  {0x02,0,20},{0x20,0x20,0},{0x05,0,600},{0x02,0,20},{0x20,0x1C,0},
  {0x05,0,380},{0x02,0,20}};
static const cn2core::RefStage R_STEAM[] = {
  {0x02,0,20},{0x20,0x07,0},{0x04,0,400},{0x22,0xFF,70}};

static void test_pcycle_first_fill_separates_steam(void) {
  const cn2core::ObsStage obs[] = {{0x02,0},{0x20,0x07}};
  TEST_ASSERT_TRUE(cn2core::matchProgram(obs,2,R_STEAM,4) >= 0);
  TEST_ASSERT_EQUAL_INT(-1, cn2core::matchProgram(obs,2,R_NORMAL,10));
  TEST_ASSERT_EQUAL_INT(-1, cn2core::matchProgram(obs,2,R_RAPID,7));
}

static void test_pcycle_normal_and_rapid_stay_ambiguous_early(void) {
  // Six stages in, both still match -- the tracker must NOT guess yet.
  const cn2core::ObsStage obs[] =
    {{0x02,0},{0x20,0x20},{0x05,0},{0x02,0},{0x20,0x1C},{0x05,0}};
  TEST_ASSERT_TRUE(cn2core::matchProgram(obs,6,R_NORMAL,10) >= 0);
  TEST_ASSERT_TRUE(cn2core::matchProgram(obs,6,R_RAPID,7)  >= 0);
}

static void test_pcycle_eighth_stage_disambiguates(void) {
  // Rapid ends at its seventh stage (a drain); an eighth stage that is a FILL
  // can only be Normal.
  const cn2core::ObsStage obs[] =
    {{0x02,0},{0x20,0x20},{0x05,0},{0x02,0},{0x20,0x1C},{0x05,0},
     {0x02,0},{0x20,0x1C}};
  TEST_ASSERT_TRUE(cn2core::matchProgram(obs,8,R_NORMAL,10) >= 0);
  TEST_ASSERT_EQUAL_INT(-1, cn2core::matchProgram(obs,8,R_RAPID,7));
}

static void test_pcycle_longer_than_program_rules_it_out(void) {
  const cn2core::ObsStage obs[] =
    {{0x02,0},{0x20,0x07},{0x04,0},{0x22,0xFF},{0x02,0}};
  TEST_ASSERT_EQUAL_INT(-1, cn2core::matchProgram(obs,5,R_STEAM,4));
}

static void test_pcycle_missing_target_still_matches(void) {
  // b3 can arrive a beat after the load bit; an observed fill with t3 still 0
  // must not rule anything out.
  const cn2core::ObsStage obs[] = {{0x02,0},{0x20,0x00}};
  TEST_ASSERT_TRUE(cn2core::matchProgram(obs,2,R_NORMAL,10) >= 0);
  TEST_ASSERT_TRUE(cn2core::matchProgram(obs,2,R_STEAM,4)  >= 0);
}

static void test_pcycle_totals_reproduce_the_manual(void) {
  // The estimate must land on the manual's stated durations, or the countdown
  // is fiction: Normal 29 min, Rapid 19 min.
  const uint32_t tn = cn2core::totalEstSecs(R_NORMAL,10);
  const uint32_t tr = cn2core::totalEstSecs(R_RAPID,7);
  TEST_ASSERT_UINT32_WITHIN(90, 29*60, tn);
  TEST_ASSERT_UINT32_WITHIN(90, 19*60, tr);
}

static void test_pcycle_remaining_counts_down(void) {
  // Mid-wash (stage 3 of Normal, 100 s in): remaining = 500 left of the wash
  // + everything after it.
  uint32_t after = 0;
  for (int i = 3; i < 10; i++) after += cn2core::stageEstSecs(R_NORMAL[i]);
  TEST_ASSERT_EQUAL_UINT32(500 + after,
                           cn2core::remainEstSecs(R_NORMAL,10,3,100));
  // Overrun of the current stage clamps to the tail, never wraps.
  TEST_ASSERT_EQUAL_UINT32(after, cn2core::remainEstSecs(R_NORMAL,10,3,9999));
}


// ---------------------------------------------------------------------------
// False-E5 filter
// ---------------------------------------------------------------------------
static cn2core::E5Filter healthy(void) {
  cn2core::E5Filter f;
  f.transparent = f.board_fresh = f.panel_fresh = f.clean = true;
  return f;
}

static void test_e5f_disproves_only_when_all_four_hold(void) {
  TEST_ASSERT_TRUE(healthy().canDisprove());
}

static void test_e5f_will_not_mask_a_fault_it_earned(void) {
  // Thinning, probe, spoof and virtual are us deliberately breaking the link.
  // A comms fault raised then is TRUE and must reach the panel.
  cn2core::E5Filter f = healthy(); f.transparent = false;
  TEST_ASSERT_FALSE(f.canDisprove());
}

static void test_e5f_will_not_mask_on_a_stale_link(void) {
  cn2core::E5Filter a = healthy(); a.board_fresh = false;
  cn2core::E5Filter b = healthy(); b.panel_fresh = false;
  TEST_ASSERT_FALSE(a.canDisprove());
  TEST_ASSERT_FALSE(b.canDisprove());   // stale frames = cannot prove it wrong
}

static void test_e5f_will_not_mask_with_bad_checksums(void) {
  cn2core::E5Filter f = healthy(); f.clean = false;
  TEST_ASSERT_FALSE(f.canDisprove());   // corruption is real evidence FOR it
}


// ---------------------------------------------------------------------------
// FrameTx — the checksum decision
// ---------------------------------------------------------------------------
// These exist because of a shipped bug: the relay recomputed the trailing XOR
// only when one of an ENUMERATED list of overrides was set, a new rewrite was
// added without joining that list, and every edited frame went out with a
// stale checksum. The far end discarded all of them and reported a comms
// failure. The tests below are written so that ANY rewrite -- including ones
// nobody has thought of yet -- is covered, because they assert on the emitted
// frame rather than on which feature did the editing.

// Emit a whole frame through FrameTx, applying `edits` (index -> value).
// Returns the bytes actually put on the wire.
static void txEmit(const uint8_t *in, uint8_t n,
                   const int *edits, uint8_t out[16]) {
  cn2core::FrameTx tx;
  tx.start(in[0]);
  for (uint8_t i = 0; i < n; i++) {
    const uint8_t rew = (edits && edits[i] >= 0) ? (uint8_t)edits[i] : in[i];
    out[i] = tx.feed(in[i], rew);
  }
}
static bool xorValid(const uint8_t *f, uint8_t n) {
  uint8_t x = 0;
  for (uint8_t i = 0; i + 1 < n; i++) x ^= f[i];
  return x == f[n - 1];
}

static const uint8_t CTRL_FRAME[8]  = {0xA2,0x19,0x00,0x02,0x04,0x0D,0x02,0xB2};
static const uint8_t PANEL_FRAME[5] = {0xAA,0x00,0x40,0x20,0xCA};

static void test_frametx_source_frames_are_themselves_valid(void) {
  // If the fixtures were wrong every assertion below would be meaningless.
  TEST_ASSERT_TRUE(xorValid(CTRL_FRAME, 8));
  TEST_ASSERT_TRUE(xorValid(PANEL_FRAME, 5));
}

static void test_frametx_passthrough_is_byte_identical(void) {
  // No edit: the ORIGINAL checksum byte must survive untouched, so a relay
  // that rewrites nothing is transparent on the wire.
  uint8_t out[16];
  txEmit(CTRL_FRAME, 8, nullptr, out);
  TEST_ASSERT_EQUAL_MEMORY(CTRL_FRAME, out, 8);
  txEmit(PANEL_FRAME, 5, nullptr, out);
  TEST_ASSERT_EQUAL_MEMORY(PANEL_FRAME, out, 5);
}

// THE REGRESSION TEST. This is the exact frame that shipped broken: byte 3
// masked from 0x40 to 0x00 by the false-E5 filter, checksum left at 0xDE.
static void test_frametx_masking_bit6_recomputes_the_checksum(void) {
  const uint8_t in[8] = {0xA2,0x32,0x00,0x40,0x04,0x09,0x03,0xDE};
  TEST_ASSERT_TRUE(xorValid(in, 8));
  int edits[8] = {-1,-1,-1,0x00,-1,-1,-1,-1};
  uint8_t out[16];
  txEmit(in, 8, edits, out);
  TEST_ASSERT_EQUAL_HEX8(0x00, out[3]);
  TEST_ASSERT_NOT_EQUAL(0xDE, out[7]);      // the shipped bug: DE survived
  TEST_ASSERT_EQUAL_HEX8(0x9E, out[7]);
  TEST_ASSERT_TRUE(xorValid(out, 8));
}

// EXHAUSTIVE. Every payload position, every possible value: if the emitted
// frame differs from the original anywhere, it must carry a valid checksum.
// A rewrite added in future cannot escape this without also escaping FrameTx.
static void test_frametx_any_single_byte_edit_stays_valid(void) {
  uint8_t out[16];
  for (uint8_t pos = 1; pos < 7; pos++) {
    for (int v = 0; v < 256; v++) {
      int edits[8] = {-1,-1,-1,-1,-1,-1,-1,-1};
      edits[pos] = v;
      txEmit(CTRL_FRAME, 8, edits, out);
      TEST_ASSERT_EQUAL_HEX8((uint8_t)v, out[pos]);
      TEST_ASSERT_TRUE_MESSAGE(xorValid(out, 8), "edited frame has a bad XOR");
    }
  }
  for (uint8_t pos = 1; pos < 4; pos++) {
    for (int v = 0; v < 256; v++) {
      int edits[5] = {-1,-1,-1,-1,-1};
      edits[pos] = v;
      txEmit(PANEL_FRAME, 5, edits, out);
      TEST_ASSERT_TRUE_MESSAGE(xorValid(out, 5), "edited panel frame bad XOR");
    }
  }
}

// Every SIMULTANEOUS pair too -- the lid override and the E5 filter both touch
// byte 3, temp touches byte 1, flow byte 2, and they combine.
static void test_frametx_multi_byte_edits_stay_valid(void) {
  uint8_t out[16];
  for (uint8_t a = 1; a < 7; a++)
    for (uint8_t b = 1; b < 7; b++) {
      int edits[8] = {-1,-1,-1,-1,-1,-1,-1,-1};
      edits[a] = 0x5A; edits[b] = 0xA5;
      txEmit(CTRL_FRAME, 8, edits, out);
      TEST_ASSERT_TRUE(xorValid(out, 8));
    }
}

static void test_frametx_rewrite_to_the_same_value_is_not_an_edit(void) {
  // An override that writes back what was already there must leave the frame
  // byte-identical, checksum included -- otherwise "transparent" pass-through
  // would silently start regenerating checksums.
  int edits[8] = {-1,-1,-1,CTRL_FRAME[3],-1,-1,-1,-1};
  uint8_t out[16];
  txEmit(CTRL_FRAME, 8, edits, out);
  TEST_ASSERT_EQUAL_MEMORY(CTRL_FRAME, out, 8);
}

static void test_frametx_unknown_header_never_synthesises_a_checksum(void) {
  // Noise and foreign traffic must pass through untouched: we do not know
  // where such a frame ends, so no byte may be replaced by an accumulator.
  const uint8_t junk[6] = {0x55,0x11,0x22,0x33,0x44,0x99};
  int edits[6] = {-1,0xEE,-1,-1,-1,-1};
  uint8_t out[16];
  txEmit(junk, 6, edits, out);
  TEST_ASSERT_EQUAL_HEX8(0xEE, out[1]);
  for (uint8_t i = 2; i < 6; i++) TEST_ASSERT_EQUAL_HEX8(junk[i], out[i]);
}

static void test_frametx_restarts_cleanly_between_frames(void) {
  // An edited frame must not leave `edit` set and make the NEXT, untouched
  // frame regenerate its checksum.
  cn2core::FrameTx tx;
  tx.start(CTRL_FRAME[0]);
  for (uint8_t i = 0; i < 8; i++)
    tx.feed(CTRL_FRAME[i], i == 3 ? 0x00 : CTRL_FRAME[i]);
  TEST_ASSERT_TRUE(tx.edit);
  tx.start(CTRL_FRAME[0]);
  TEST_ASSERT_FALSE(tx.edit);
  uint8_t out[8];
  for (uint8_t i = 0; i < 8; i++) out[i] = tx.feed(CTRL_FRAME[i], CTRL_FRAME[i]);
  TEST_ASSERT_EQUAL_MEMORY(CTRL_FRAME, out, 8);
}


// ---- E5 filter: one bad frame must not latch E5 forever -------------------
// Observed on the machine: 1152 frames masked, then ONE frame where the health
// check momentarily failed. That single frame reached the panel, which latched
// E5 permanently. Against a LATCHING signal, fail-open is the wrong default.
static void test_e5f_a_single_doubtful_frame_still_masks(void) {
  cn2core::E5Filter f = healthy();
  for (int i = 0; i < 50; i++) TEST_ASSERT_TRUE(f.mask(25));
  f.panel_fresh = false;                 // one hiccup
  TEST_ASSERT_TRUE_MESSAGE(f.mask(25), "one doubtful frame leaked bit 6");
  f.panel_fresh = true;
  TEST_ASSERT_TRUE(f.mask(25));
  TEST_ASSERT_EQUAL_UINT16(0, f.doubt);  // recovery rearms the budget
}

static void test_e5f_brief_doubt_never_leaks(void) {
  // Anything shorter than the threshold, at any point, must hold the mask.
  for (uint16_t burst = 1; burst < 25; burst++) {
    cn2core::E5Filter f = healthy();
    f.mask(25);
    f.clean = false;
    bool leaked = false;
    for (uint16_t i = 0; i < burst; i++) if (!f.mask(25)) leaked = true;
    TEST_ASSERT_FALSE_MESSAGE(leaked, "a sub-threshold burst leaked bit 6");
  }
}

static void test_e5f_sustained_doubt_does_surrender(void) {
  // A real, lasting fault must still reach the panel -- late, but it must.
  cn2core::E5Filter f = healthy();
  f.mask(25);
  f.transparent = false;
  for (int i = 0; i < 24; i++) TEST_ASSERT_TRUE(f.mask(25));
  TEST_ASSERT_FALSE(f.mask(25));         // the 25th consecutive gives up
  TEST_ASSERT_FALSE(f.mask(25));         // and stays given up
}

static void test_e5f_doubt_counter_resets_on_recovery(void) {
  // Doubt must not accumulate across unrelated hiccups minutes apart, or a
  // healthy machine eventually leaks anyway.
  cn2core::E5Filter f = healthy();
  for (int round = 0; round < 10; round++) {
    f.board_fresh = false;
    for (int i = 0; i < 20; i++) TEST_ASSERT_TRUE(f.mask(25));
    f.board_fresh = true;
    TEST_ASSERT_TRUE(f.mask(25));
    TEST_ASSERT_EQUAL_UINT16(0, f.doubt);
  }
}


// ---------------------------------------------------------------------------
// FramePos — byte index must survive a CPU stall
// ---------------------------------------------------------------------------
// The bug this replaces: frame position was derived from the wall-clock gap
// between bytes. A 96 ms stall (WiFi association disabling the instruction
// cache) made the relay drain the UART FIFO late, the gap looked like a frame
// boundary, the index reset MID-FRAME, and byte 3 was never recognised as
// byte 3 -- so every position-keyed rewrite silently skipped it and the raw
// status byte reached the panel. One frame per boot, and the panel latches.
//
// Counting bytes cannot be perturbed by scheduling, which is the whole point:
// there is no timing input to these tests because there is no timing input to
// the code any more.
static void test_framepos_walks_a_controller_frame(void) {
  cn2core::FramePos p;
  const uint8_t f[8] = {0xA2,0x19,0x00,0x02,0x04,0x0D,0x02,0xB2};
  for (uint8_t i = 0; i < 8; i++) {
    TEST_ASSERT_EQUAL_UINT8(i, p.feed(f[i]));
    p.advance();
  }
  TEST_ASSERT_EQUAL_UINT8(0, p.feed(f[0]));   // wrapped, ready for the next
}

static void test_framepos_finds_byte3_in_every_frame(void) {
  // Byte 3 is the status byte -- the one the E5 filter must see. Ten frames
  // back to back, no gaps, no timing: index 3 must land on it every time.
  cn2core::FramePos p;
  const uint8_t f[8] = {0xA2,0x19,0x00,0x40,0x04,0x0D,0x02,0xF0};
  int seen = 0;
  for (int frame = 0; frame < 10; frame++)
    for (uint8_t i = 0; i < 8; i++) {
      if (p.feed(f[i]) == 3) { TEST_ASSERT_EQUAL_HEX8(0x40, f[i]); seen++; }
      p.advance();
    }
  TEST_ASSERT_EQUAL_INT(10, seen);
}

static void test_framepos_panel_frame_is_five(void) {
  cn2core::FramePos p;
  const uint8_t f[5] = {0xAA,0x05,0x00,0x00,0xAF};
  for (int frame = 0; frame < 3; frame++)
    for (uint8_t i = 0; i < 5; i++) {
      TEST_ASSERT_EQUAL_UINT8(i, p.feed(f[i]));
      p.advance();
    }
}

static void test_framepos_reports_unsynced_on_a_non_header(void) {
  // Garbage must not be counted as a frame, or the next real header lands at
  // a nonzero index and every rewrite is off by that much.
  cn2core::FramePos p;
  for (int i = 0; i < 5; i++) {
    TEST_ASSERT_EQUAL_UINT8(0xFF, p.feed(0x55));
    p.advance();
    TEST_ASSERT_EQUAL_UINT8(0, p.i);          // parked, still scanning
  }
  TEST_ASSERT_EQUAL_UINT8(0, p.feed(0xA2));   // locks on at the real header
}

static void test_framepos_relocks_after_a_truncated_frame(void) {
  // A frame cut short by a real break: the next header must re-lock cleanly
  // rather than inherit the leftover index.
  cn2core::FramePos p;
  const uint8_t f[8] = {0xA2,0x19,0x00,0x40,0x04,0x0D,0x02,0xF0};
  for (uint8_t i = 0; i < 4; i++) { p.feed(f[i]); p.advance(); }
  TEST_ASSERT_TRUE(p.insideFrame());          // a clock gap MAY NOT reset here
  p.reset();                                  // ...but a real break can
  int seen = 0;
  for (uint8_t i = 0; i < 8; i++) { if (p.feed(f[i]) == 3) seen++; p.advance(); }
  TEST_ASSERT_EQUAL_INT(1, seen);
}

static void test_framepos_insideframe_guards_the_clock_reset(void) {
  // insideFrame() is what stops a stall-induced "gap" from resyncing us
  // mid-frame. It must be false only at a genuine boundary.
  cn2core::FramePos p;
  const uint8_t f[8] = {0xA2,0x19,0x00,0x40,0x04,0x0D,0x02,0xF0};
  TEST_ASSERT_FALSE(p.insideFrame());         // boundary before the header
  for (uint8_t i = 0; i < 8; i++) {
    p.feed(f[i]); p.advance();
    TEST_ASSERT_EQUAL(i < 7, p.insideFrame());
  }
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_frameLenFor_known_headers);
  RUN_TEST(test_frameLenFor_rejects_junk);
  RUN_TEST(test_xorOf_matches_captured_checksums);
  RUN_TEST(test_idle_panel_frame_is_ambiguous);
  RUN_TEST(test_checksumOk_accepts_real_frames);
  RUN_TEST(test_checksumOk_rejects_single_bit_flip);
  RUN_TEST(test_checksumOk_rejects_runt);

  RUN_TEST(test_assembler_completes_a_frame);
  RUN_TEST(test_assembler_counts_a_corrupt_frame);
  RUN_TEST(test_assembler_skips_junk_before_a_header);
  RUN_TEST(test_assembler_resyncs_after_a_dropped_byte);
  RUN_TEST(test_assembler_handles_both_frame_types_interleaved);
  RUN_TEST(test_assembler_reset_and_clear);

  RUN_TEST(test_thinner_passes_everything_by_default);
  RUN_TEST(test_thinner_forwards_one_in_four);
  RUN_TEST(test_thinner_first_frame_always_forwards);
  RUN_TEST(test_thinner_zero_is_treated_as_one);
  RUN_TEST(test_thinner_setEvery_restarts_the_phase);

  RUN_TEST(test_applyMask_clear_then_set);
  RUN_TEST(test_applyMask_set_beats_clear);
  RUN_TEST(test_rewriteStatus_passthrough);
  RUN_TEST(test_rewriteStatus_forces_lid_on_clears_both);
  RUN_TEST(test_rewriteStatus_forces_lid_off_sets_both);
  RUN_TEST(test_lidClosed_needs_both_sensors);
  RUN_TEST(test_rewriteStatus_can_clear_e5);
  RUN_TEST(test_rewriteStatus_mask_applies_after_lid);
  RUN_TEST(test_rewritePanelByte_passthrough);
  RUN_TEST(test_rewritePanelByte_injects_a_press);
  RUN_TEST(test_rewritePanelByte_forces_a_load_bit_off);
  RUN_TEST(test_rewritePanelByte_only_touches_byte_1);
  RUN_TEST(test_rewritePanelByte_forces_the_mode_word);
  RUN_TEST(test_rewritePanelByte_mode_zero_is_a_real_value);
  RUN_TEST(test_probe_blanks_the_payload_and_wins);
  RUN_TEST(test_pressing_without_a_mask_is_not_active);

  RUN_TEST(test_stream_passthrough_is_byte_identical);
  RUN_TEST(test_stream_recomputes_checksum_when_overriding);
  RUN_TEST(test_stream_recomputes_panel_checksum);
  RUN_TEST(test_stream_reset_between_frames);
  RUN_TEST(test_stream_noop_override_is_identity);

  RUN_TEST(test_deviceFromHeader);
  RUN_TEST(test_isDriven_threshold);
  RUN_TEST(test_resolveRx_both_orders);
  RUN_TEST(test_resolveRx_refuses_ambiguous_input);

  RUN_TEST(test_flushcap_ignores_a_targeted_fill);
  RUN_TEST(test_flushcap_ignores_a_drain_without_intake);
  RUN_TEST(test_flushcap_fires_once_at_the_cap);
  RUN_TEST(test_flushcap_does_not_fire_early);
  RUN_TEST(test_flushcap_strips_intake_and_keeps_the_drain);
  RUN_TEST(test_flushcap_stays_latched_on_its_own_output);
  RUN_TEST(test_flushcap_survives_one_dropped_frame);
  RUN_TEST(test_flushcap_releases_after_two_clear_frames);
  RUN_TEST(test_flushcap_rearms_for_the_next_flush);
  RUN_TEST(test_flushcap_zero_disables);
  RUN_TEST(test_flushcap_survives_millis_rollover);
  RUN_TEST(test_flushcap_default_clears_a_real_flush);

  RUN_TEST(test_txq_holds_a_ctrl_frame_until_complete);
  RUN_TEST(test_txq_panel_frame_is_five_bytes);
  RUN_TEST(test_txq_unknown_header_passes_through_at_once);
  RUN_TEST(test_txq_suppression_drops_the_whole_frame);
  RUN_TEST(test_txq_clear_rearms);
  RUN_TEST(test_txq_never_overruns_its_buffer);

  RUN_TEST(test_pcycle_first_fill_separates_steam);
  RUN_TEST(test_pcycle_normal_and_rapid_stay_ambiguous_early);
  RUN_TEST(test_pcycle_eighth_stage_disambiguates);
  RUN_TEST(test_pcycle_longer_than_program_rules_it_out);
  RUN_TEST(test_pcycle_missing_target_still_matches);
  RUN_TEST(test_pcycle_totals_reproduce_the_manual);
  RUN_TEST(test_pcycle_remaining_counts_down);

  RUN_TEST(test_e5f_disproves_only_when_all_four_hold);
  RUN_TEST(test_e5f_will_not_mask_a_fault_it_earned);
  RUN_TEST(test_e5f_will_not_mask_on_a_stale_link);
  RUN_TEST(test_e5f_will_not_mask_with_bad_checksums);
  RUN_TEST(test_e5f_a_single_doubtful_frame_still_masks);
  RUN_TEST(test_e5f_brief_doubt_never_leaks);
  RUN_TEST(test_e5f_sustained_doubt_does_surrender);
  RUN_TEST(test_e5f_doubt_counter_resets_on_recovery);

  RUN_TEST(test_framepos_walks_a_controller_frame);
  RUN_TEST(test_framepos_finds_byte3_in_every_frame);
  RUN_TEST(test_framepos_panel_frame_is_five);
  RUN_TEST(test_framepos_reports_unsynced_on_a_non_header);
  RUN_TEST(test_framepos_relocks_after_a_truncated_frame);
  RUN_TEST(test_framepos_insideframe_guards_the_clock_reset);

  RUN_TEST(test_frametx_source_frames_are_themselves_valid);
  RUN_TEST(test_frametx_passthrough_is_byte_identical);
  RUN_TEST(test_frametx_masking_bit6_recomputes_the_checksum);
  RUN_TEST(test_frametx_any_single_byte_edit_stays_valid);
  RUN_TEST(test_frametx_multi_byte_edits_stay_valid);
  RUN_TEST(test_frametx_rewrite_to_the_same_value_is_not_an_edit);
  RUN_TEST(test_frametx_unknown_header_never_synthesises_a_checksum);
  RUN_TEST(test_frametx_restarts_cleanly_between_frames);

  RUN_TEST(test_end_to_end_forced_intake_motor);
  RUN_TEST(test_thinned_stream_emits_whole_frames);
  return UNITY_END();
}
