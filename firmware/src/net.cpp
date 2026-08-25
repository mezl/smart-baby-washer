#include "net.h"

#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <WiFi.h>
#include <esp_task_wdt.h>

#include "config.h"
#include "cn2.h"

namespace net {

static uint32_t s_lost_since = 0;
static uint32_t s_last_hold  = 0;   // rate-limits the "holding off" log line

// ---------------------------------------------------------------------------
// The OTA callbacks are what make this survivable.
//
// onStart quiesces the payload so nothing is touching the UARTs or the ring
// while the flash is being written. onProgress feeds the task watchdog, because
// the OTA write blocks loop() for several seconds and would otherwise trip it
// mid-flash — which is the one moment you really do not want a reset. onError
// reboots rather than limping on: a failed OTA leaves the previously running
// image intact and bootable, so a clean restart is strictly better than an
// unknown state.
// ---------------------------------------------------------------------------
static void beginOTA() {
  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.setPort(OTA_PORT);

  ArduinoOTA.onStart([]() {
    Serial.printf("\n[ota ] update starting (%s) — quiescing payload\n",
                  ArduinoOTA.getCommand() == U_FLASH ? "sketch" : "filesystem");
    cn2::quiesce();
    esp_task_wdt_reset();
  });

  ArduinoOTA.onProgress([](unsigned int done, unsigned int total) {
    esp_task_wdt_reset();
    static int last = -1;
    int pct = total ? (int)(done * 100 / total) : 0;
    if (pct != last && pct % 10 == 0) {
      Serial.printf("[ota ] %d%%\n", pct);
      last = pct;
    }
  });

  ArduinoOTA.onEnd([]() { Serial.println("[ota ] done — rebooting"); });

  ArduinoOTA.onError([](ota_error_t e) {
    Serial.printf("[ota ] FAILED (%u) — restarting to a clean state\n", e);
    delay(300);
    ESP.restart();
  });

  ArduinoOTA.begin();  // also advertises _arduino._tcp over mDNS
  MDNS.addService("http", "tcp", 80);
  Serial.printf("[ota ] ready: http://%s.local/   espota on :%d\n",
                OTA_HOSTNAME, OTA_PORT);
}

void begin() {
  WiFi.persistent(false);   // don't wear the flash rewriting creds every boot
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);     // modem sleep makes mDNS and espota flaky
  WiFi.setAutoReconnect(true);
  WiFi.setHostname(OTA_HOSTNAME);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  // TX power is NOT capped. It was, briefly (11 dBm, chasing the rail-draw
  // theory for the lockouts) -- but WIRE mode made the cap pointless (the
  // matrix bridge cannot be starved by anything the radio does) and at the
  // measured RSSI of -85 dBm inside the appliance the cap turned every HTTP
  // consumer flaky: HA rest polls timing out, entities never registering.
  // Full power is the difference between a usable link and a haunted one.

  Serial.printf("[wifi] joining \"%s\"", WIFI_SSID);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED &&
         millis() - t0 < WIFI_CONNECT_TIMEOUT_MS) {
    delay(400);
    Serial.print('.');
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[wifi] connected  ip=%s  rssi=%d dBm  mac=%s\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI(),
                  WiFi.macAddress().c_str());
  } else {
    // Deliberately not fatal. Autoreconnect stays armed and loop() will reboot
    // us after WIFI_REBOOT_AFTER_MS if it never comes up. Blocking forever here
    // would make a bad AP look identical to bricked firmware.
    Serial.println("[wifi] not up yet — continuing, autoreconnect armed");
    s_lost_since = millis();
  }

  if (!MDNS.begin(OTA_HOSTNAME)) {
    Serial.println("[mdns] begin FAILED");
  } else {
    Serial.printf("[mdns] %s.local\n", OTA_HOSTNAME);
  }

  beginOTA();
}

void loop() {
  ArduinoOTA.handle();

  static uint32_t last_retry = 0;
  uint32_t now = millis();

  if (WiFi.status() != WL_CONNECTED) {
    if (!s_lost_since) {
      s_lost_since = now;
      Serial.println("[wifi] link lost — waiting for reconnect");
    }
    // Re-kick the supplicant periodically. setAutoReconnect() alone sometimes
    // wedges after the AP itself reboots, and then the board sits there looking
    // alive but unreachable — the exact failure this whole file exists to avoid.
    if (now - last_retry > WIFI_RETRY_MS) {
      last_retry = now;
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
      Serial.println("[wifi] re-issuing join");
    }
    // Restarting to fix WiFi costs the CN2 link, and on a board sealed inside
    // an appliance that is the wrong way round: the link is the machine's
    // actual function, OTA is a convenience. A reboot drops forwarding for the
    // whole boot, which is long enough for a panel to latch E5 -- a fault the
    // owner then has to power-cycle the appliance to clear, to recover a radio
    // that a restart usually cannot fix anyway (a down AP stays down).
    //
    // So only restart when the link has nothing left to lose. While frames are
    // still flowing, keep re-kicking the supplicant forever instead; autoreconnect
    // will take it when the AP returns.
    const bool link_alive =
        cn2::lastByteAgeMs(cn2::FROM_PANEL) < WIFI_REBOOT_LINK_IDLE_MS ||
        cn2::lastByteAgeMs(cn2::FROM_BOARD) < WIFI_REBOOT_LINK_IDLE_MS;
    if (now - s_lost_since > WIFI_REBOOT_AFTER_MS) {
      if (link_alive) {
        if (now - s_last_hold > 60000UL) {
          s_last_hold = now;
          Serial.println("[wifi] still down, but the CN2 link is live — "
                         "holding off the restart");
        }
      } else {
        Serial.println("[wifi] still down and the CN2 link is idle — restarting");
        delay(200);
        ESP.restart();
      }
    }
  } else if (s_lost_since) {
    Serial.printf("[wifi] back after %lu ms  ip=%s\n",
                  (unsigned long)(now - s_lost_since),
                  WiFi.localIP().toString().c_str());
    s_lost_since = 0;
  }
}

bool     connected() { return WiFi.status() == WL_CONNECTED; }
String   ip()        { return WiFi.localIP().toString(); }
int      rssi()      { return WiFi.RSSI(); }
uint32_t downMs()    { return s_lost_since ? millis() - s_lost_since : 0; }

}  // namespace net
