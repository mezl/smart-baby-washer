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
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 1500) delay(10);

  bootGuard();
  g_boot_ms = millis();

  Serial.printf("\n=== %s %s (%s %s) ===\n", FW_NAME, FW_VERSION, __DATE__,
                __TIME__);
  Serial.printf("[boot ] reset reason %d   boot #%lu%s\n",
                (int)esp_reset_reason(), (unsigned long)g_boot_count,
                g_safe_mode ? "   ** SAFE MODE **" : "");

  // Network first, always. If the payload is going to take the board down, it
  // should at least take it down with OTA already listening.
  net::begin();
  web::begin();

  if (g_safe_mode) {
    Serial.println("[boot ] SAFE MODE — CN2 link disabled, OTA only. "
                   "Power-cycle to clear.");
  } else {
    cn2::begin();
  }

  armWatchdog();
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
