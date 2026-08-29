// Momcozy D8 — CN2 panel-link sniffer / relay on a Seeed XIAO ESP32-C3.
//
// Sits on the 4-pin CN2 header between the D8 main board (FMD BF7612 8051) and
// its front panel, logs every byte in both directions, and — in RELAY mode —
// forwards them so the machine keeps working while we decode the protocol.
//
// See ../D8_REPAIR_SSOT.md §7b for the CN2 pinout and the wiring rationale.
//
// Design priority, in order:
//   1. WiFi and OTA must ALWAYS come back, whatever the payload does.
//   2. Never drive a CN2 line until the directions are known (/api/detect).
//   3. Then capture everything.

#include <Arduino.h>
#include <WiFi.h>
#include <esp_ota_ops.h>
#include <esp_task_wdt.h>
#include <Preferences.h>

#include "app.h"
#include "config.h"
#include "cn2.h"
#include "kasa.h"
#include "net.h"
#include "web.h"

// ---------------------------------------------------------------------------
// Boot-loop guard.
//
// The failure a "robust OTA" setup actually has to survive is flashing something
// that crashes before WiFi comes up. Without a guard, that board is bricked until
// it goes back on the USB cable — and this one will be inside an appliance.
//
// RTC_NOINIT memory survives a reset, including a panic or watchdog reset, but
// not a power cycle. That is exactly the right scope: three resets without ever
// reaching a healthy uptime and we come up in SAFE MODE with the payload
// disabled and nothing running but WiFi, mDNS and both OTA paths. Pull the power
// and it starts clean.
// ---------------------------------------------------------------------------
static const uint32_t BOOT_MAGIC = 0xD8C2A11EUL;

RTC_NOINIT_ATTR static uint32_t rtc_magic;
RTC_NOINIT_ATTR static uint32_t rtc_boots;

static bool     g_safe_mode = false;
static uint32_t g_boot_count = 0;
static uint32_t g_boot_ms = 0;
static bool     g_marked_ok = false;

namespace app {
bool     safeMode()        { return g_safe_mode; }
uint32_t bootCount()       { return g_boot_count; }
bool     imageMarkedGood() { return g_marked_ok; }
}  // namespace app


// ---------------------------------------------------------------------------
// USB serial console + radio-off mode.
//
// When the module is on a USB cable there is a channel that costs the machine
// nothing: no association, no DHCP, no ~300 mA transmit bursts on the CN2 5 V
// net. That matters here because the radio is the last unexonerated load in
// the E5 investigation -- WiFi is precisely what we want to be able to switch
// OFF while keeping full telemetry.
//
// "nowifi" in NVS skips net/web/kasa entirely; everything is then driven from
// this console. Read and written through a LOCAL Preferences handle -- the
// long-lived one belongs to cn2 and begin()/end() on it breaks that owner.
// ---------------------------------------------------------------------------
static bool     g_nowifi  = false;
static bool     g_mon     = false;   // periodic one-line telemetry
static uint32_t g_mon_ms  = 0;
static char     g_cbuf[48];
static uint8_t  g_clen    = 0;

static bool nowifiStored() {
  Preferences p;
  if (!p.begin("d8link", true)) return false;
  const bool v = p.getBool("nowifi", false);
  p.end();
  return v;
}

static void nowifiStore(bool v) {
  Preferences p;
  if (!p.begin("d8link", false)) return;
  p.putBool("nowifi", v);
  p.end();
}

static void monLine() {
  const uint8_t st = cn2::statusReal();
  Serial.printf("[mon  ] t=%6lus st=0x%02X%s%s temp=%u ok_c=%lu ok_p=%lu "
                "bad=%lu/%lu bb=%lu pb=%lu age_c=%ldms\n",
                (unsigned long)(millis() / 1000), st,
                (st & 0x40) ? " LATCH" : "",
                (st & 0x82) ? " LID"   : "",
                (unsigned)cn2::tempReal(),
                (unsigned long)cn2::frameOk(0), (unsigned long)cn2::frameOk(1),
                (unsigned long)cn2::frameBad(0), (unsigned long)cn2::frameBad(1),
                (unsigned long)cn2::byteCount(cn2::FROM_BOARD),
                (unsigned long)cn2::byteCount(cn2::FROM_PANEL),
                (long)cn2::lastByteAgeMs(cn2::FROM_BOARD));
  if (cn2::wire()) Serial.print(F("[mode ] WIRE — E5 mask inert (pad bridge)\n"));
  Serial.printf("[mask ] mode=%u on=%d masked=%lu leaks=%lu why=%s\n",
                (unsigned)cn2::e5FilterMode(), (int)cn2::e5FilterMasking(),
                (unsigned long)cn2::e5FilterFrames(),
                (unsigned long)cn2::e5FilterLeaks(), cn2::e5FilterWhy());
  // The whole controller frame, because byte 5 is the earliest divergence
  // known: the 2026-08-26 latch capture shows it settle to 0x0A at t=710 ms,
  // 4.4 s BEFORE the status bit -- healthy idle on this unit reads 0x09.
  Serial.printf("[frame] B>P %s   P>B %s\n",
                cn2::lastFrameHex(cn2::FROM_BOARD).c_str(),
                cn2::lastFrameHex(cn2::FROM_PANEL).c_str());
}

static void consoleHelp() {
  Serial.println(F("[con  ] s=status m=monitor c=CPU-relay w=wire "
                   "e/E=E5 mask on/off r=reboot W/X=radio on/off ?=help"));
}

static void consoleExec(const char *c) {
  switch (c[0]) {
    case 's': monLine(); break;
    case 'm':
      g_mon = !g_mon;
      Serial.printf("[con  ] monitor %s\n", g_mon ? "ON (1 Hz)" : "off");
      break;
    case 'W':
      nowifiStore(false);
      Serial.println(F("[con  ] radio ENABLED on next boot — rebooting"));
      delay(120); ESP.restart();
      break;
    case 'X':
      nowifiStore(true);
      Serial.println(F("[con  ] radio DISABLED on next boot — rebooting"));
      delay(120); ESP.restart();
      break;
    case 'c':
      // CPU relay. Bit 6 can only be masked here: wire mode is a pad bridge
      // in the GPIO matrix, so no byte ever passes through software and the
      // E5 filter is inert however it is set.
      cn2::wireSet(false);
      Serial.printf("[con  ] CPU RELAY mode (wire=%d)\n", (int)cn2::wire());
      break;
    case 'w':
      // Manual only. Never switch to wire automatically -- on trouble in CPU
      // relay the recovery is a power cycle and a fix, not a mode change.
      cn2::wireSet(true);
      Serial.printf("[con  ] WIRE mode (wire=%d)\n", (int)cn2::wire());
      break;
    case 'e':
      cn2::setE5Filter(cn2::E5F_FORCE);
      Serial.println(F("[con  ] E5 filter FORCE — bit 6 masked before the panel"));
      break;
    case 'E':
      cn2::setE5Filter(cn2::E5F_OFF);
      Serial.println(F("[con  ] E5 filter OFF — panel sees bit 6 raw"));
      break;
    case 'r':
      Serial.println(F("[con  ] rebooting"));
      delay(120); ESP.restart();
      break;
    default: consoleHelp(); break;
  }
}

static void consoleLoop() {
  while (Serial.available()) {
    const char ch = (char)Serial.read();
    if (ch == '\n' || ch == '\r') {
      if (g_clen) { g_cbuf[g_clen] = 0; consoleExec(g_cbuf); g_clen = 0; }
    } else if (g_clen < sizeof(g_cbuf) - 1) {
      g_cbuf[g_clen++] = ch;
    }
  }
  if (g_mon && (uint32_t)(millis() - g_mon_ms) >= 1000) {
    g_mon_ms = millis();
    monLine();
  }
}

static void bootGuard() {
  if (rtc_magic != BOOT_MAGIC) {   // cold boot — RTC RAM is garbage
    rtc_magic = BOOT_MAGIC;
    rtc_boots = 0;
  }
  rtc_boots++;
  g_boot_count = rtc_boots;
  g_safe_mode  = (rtc_boots >= SAFE_MODE_AFTER_BOOTS);
}

static void armWatchdog() {
  esp_task_wdt_config_t cfg = {
      .timeout_ms = (uint32_t)WDT_TIMEOUT_MS,
      .idle_core_mask = 0,
      .trigger_panic = true,
  };
  esp_err_t err = esp_task_wdt_init(&cfg);
  if (err == ESP_ERR_INVALID_STATE) err = esp_task_wdt_reconfigure(&cfg);
  if (err == ESP_OK) {
    esp_err_t add = esp_task_wdt_add(NULL);
    if (add == ESP_OK || add == ESP_ERR_INVALID_ARG) {
      Serial.printf("[boot ] task watchdog armed at %lu ms\n",
                    (unsigned long)WDT_TIMEOUT_MS);
      return;
    }
  }
  Serial.printf("[boot ] task watchdog NOT armed (err %d)\n", (int)err);
}

void setup() {
  // ABSOLUTE FIRST: bridge the CN2 pads, hardcoded, before Serial, before
  // NVS, before everything. The bare-machine test proved the controller
  // boots clean when the link is alive from its first millisecond and
  // latches when it is not -- its panel-init window is shorter than our
  // boot. Every microsecond of dark here risks the latch. Pins are the
  // as-built map (rxb=5 txb=6 txp=3 rxp=4), now also the config.h defaults;
  // from NVS moments later for non-default builds.
  cn2::instantBridge();
  Serial.begin(115200);
  cn2::earlyBridge();   // NVS-aware re-apply; harmless repeat
  bootGuard();
  g_boot_ms = millis();
  g_nowifi = nowifiStored();

  // ---- WATCHDOG BEFORE THE PAYLOAD, NOT AFTER ----------------------------
  //
  // It used to be armed at the end of setup(), which was fine while the
  // payload started last: WiFi and OTA were already up, so a hang still left a
  // way in. Starting the CN2 link first inverted that. Everything from
  // cn2::begin() to net::begin() now runs BEFORE the radio, and a hang anywhere
  // in there would sit forever with no watchdog to reset it and no OTA to
  // recover through.
  //
  // This board is screwed inside an appliance. There is no serial port to
  // attach and no button to hold, so "reachable over WiFi" is the only recovery
  // path that exists and nothing may be allowed to run ahead of it unguarded.
  //
  // Armed here, any hang panics, resets, and costs a boot-loop strike. Three
  // strikes and safe mode skips the payload entirely, leaving WiFi and OTA.
  armWatchdog();

  // ---- THE PANEL LINK COMES UP FIRST -------------------------------------
  //
  // This module is powered from CN2 pin 4, so it boots at the same instant as
  // the panel and the controller -- and until cn2::begin() runs, the UARTs are
  // shut and both TX pins float. Everything either end says in that window goes
  // into a disconnected wire.
  //
  // Two waits used to sit in front of it and together they cost 4.0 s, measured
  // on a live machine from the forwarded byte counts:
  //
  //   * `while (!Serial ...)` -- 1.5 s. Inside an appliance there is no USB
  //     host, so it always ran to full timeout.
  //   * net::begin() -- blocks on association for up to
  //     WIFI_CONNECT_TIMEOUT_MS. Nominally ~2.5 s; with the AP down, 20 s.
  //
  // Four seconds is long enough for a newer D8's controller to decide the panel
  // is not there. It raises the comms-failure bit, the panel LATCHES E5, and no
  // amount of perfect relaying afterwards clears it -- only a power cycle does.
  //
  // The old order existed so a crashing payload still left OTA listening. The
  // boot-loop guard already covers that: three boots without a healthy uptime
  // and safe mode disables the payload entirely. Opening two UARTs is not the
  // risky part; relayTask is, and the guard is what catches it.
  //
  // relayTask runs at priority 10 against the Arduino loop task's 1, and both
  // waits below yield, so the link keeps relaying all the way through them.
  if (g_safe_mode) {
    Serial.println("[boot ] SAFE MODE — CN2 link disabled, OTA only. "
                   "Power-cycle to clear.");
  } else {
    if (!g_nowifi) kasa::begin();
    cn2::begin();
    Serial.printf("[cn2  ] link open %lu ms after boot\n",
                  (unsigned long)millis());
  }

  // Let the machine's own startup handshake complete on a link that nothing is
  // stalling, THEN bring up the radio. See LINK_SETTLE_* in config.h.
  if (!g_safe_mode) cn2::waitLinkSettled(LINK_SETTLE_FRAMES, LINK_SETTLE_TIMEOUT_MS);

  // Optional hold before the radio. See cn2::wifiDelayMs().
  if (!g_safe_mode && cn2::wifiDelayMs()) {
    const uint32_t d = cn2::wifiDelayMs();
    Serial.printf("[wifi] holding the radio off for %lu ms; link is forwarding\n",
                  (unsigned long)d);
    const uint32_t h0 = millis();
    while ((uint32_t)(millis() - h0) < d) { esp_task_wdt_reset(); delay(50); }
  }

  // Now the slow parts, with the link already carrying traffic.
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 1500) { esp_task_wdt_reset(); delay(10); }

  Serial.printf("\n=== %s %s (%s %s) ===\n", FW_NAME, FW_VERSION, __DATE__,
                __TIME__);
  Serial.printf("[boot ] reset reason %d   boot #%lu%s\n",
                (int)esp_reset_reason(), (unsigned long)g_boot_count,
                g_safe_mode ? "   ** SAFE MODE **" : "");

  if (g_nowifi) {
    Serial.println(F("[wifi ] RADIO OFF (NVS nowifi) — USB console only. "
                     "'W' re-enables, 'm' starts the 1 Hz monitor."));
    g_mon = true;
    consoleHelp();
  } else {
    net::begin();
    web::begin();
  }

}

void loop() {
  esp_task_wdt_reset();

  consoleLoop();
  if (!g_nowifi) {
    net::loop();     // ArduinoOTA.handle() + link watchdog
    kasa::powerPoll();
    web::loop();
  }
  if (!g_safe_mode) cn2::loop();

  // Declare this boot healthy once we have been up a while WITH working WiFi.
  // Gating on WiFi is the point: an image that runs but cannot be reached is
  // not a good image, and should be rolled back rather than kept.
  if (!g_marked_ok && (g_nowifi || WiFi.status() == WL_CONNECTED) &&
      millis() - g_boot_ms > HEALTHY_UPTIME_MS) {
    g_marked_ok = true;
    rtc_boots = 0;   // clears the boot-loop guard

    // Best-effort. Arduino-ESP32 ships a bootloader without
    // CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE, in which case images are already
    // marked valid and this is a no-op. Costs nothing, and works if the
    // bootloader ever does support it. The boot-loop guard above is what
    // actually protects us either way.
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (run && esp_ota_get_state_partition(run, &st) == ESP_OK &&
        st == ESP_OTA_IMG_PENDING_VERIFY) {
      esp_ota_mark_app_valid_cancel_rollback();
      Serial.println("[ota  ] image verified good — rollback cancelled");
    }
    Serial.printf("[boot ] healthy after %lu ms — boot counter cleared\n",
                  (unsigned long)HEALTHY_UPTIME_MS);
  }

  delay(1);
}
