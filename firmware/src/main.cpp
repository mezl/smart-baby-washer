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
  Serial.begin(115200);
  cn2::earlyBridge();   // machine link first; every ms dark risks a latch
  bootGuard();
  g_boot_ms = millis();

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
    kasa::begin();
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

  net::begin();
  web::begin();

}

void loop() {
  esp_task_wdt_reset();

  net::loop();     // ArduinoOTA.handle() + link watchdog
  web::loop();
  if (!g_safe_mode) cn2::loop();

  // Declare this boot healthy once we have been up a while WITH working WiFi.
  // Gating on WiFi is the point: an image that runs but cannot be reached is
  // not a good image, and should be rolled back rather than kept.
  if (!g_marked_ok && WiFi.status() == WL_CONNECTED &&
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
