// MINIMAL BOOT EXPERIMENT — tests the hypothesis that firmware size/boot
// time causes the E5 latch. Contains ONLY: the pad bridge (first line),
// WiFi, and an OTA endpoint to escape. If this image's boots still latch,
// image size and app boot speed are exonerated by construction.
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include "esp_rom_gpio.h"
#include "soc/gpio_sig_map.h"
#include "soc/gpio_struct.h"
#include "hal/gpio_ll.h"
#include "../include/secrets.h"

static WebServer srv(80);

void setup() {
  // pads bridged before ANYTHING else — as-wired map rxb=5 txb=6 txp=3 rxp=4
  gpio_ll_input_enable(&GPIO, 5);
  gpio_ll_input_enable(&GPIO, 4);
  esp_rom_gpio_connect_in_signal(5, SIG_IN_FUNC_97_IDX, false);
  esp_rom_gpio_connect_out_signal(3, SIG_IN_FUNC_97_IDX, false, false);
  esp_rom_gpio_connect_in_signal(4, SIG_IN_FUNC_98_IDX, false);
  esp_rom_gpio_connect_out_signal(6, SIG_IN_FUNC_98_IDX, false, false);
  GPIO.func_out_sel_cfg[3].oen_sel = 1;
  GPIO.func_out_sel_cfg[6].oen_sel = 1;
  GPIO.enable_w1ts.enable_w1ts = (1UL << 3) | (1UL << 6);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  WiFi.setSleep(false);
  srv.on("/api/version", HTTP_GET, []() {
    srv.send(200, "application/json", "{\"version\":\"minimal-1\",\"uptime_s\":" + String(millis()/1000) + "}");
  });
  srv.on("/update", HTTP_POST, []() {
    srv.send(200, "text/plain", Update.hasError() ? "FAIL" : "OK");
    delay(300); ESP.restart();
  }, []() {
    HTTPUpload &up = srv.upload();
    if (srv.arg("key") != OTA_PASSWORD) return;
    if (up.status == UPLOAD_FILE_START) Update.begin(UPDATE_SIZE_UNKNOWN);
    else if (up.status == UPLOAD_FILE_WRITE) Update.write(up.buf, up.currentSize);
    else if (up.status == UPLOAD_FILE_END) Update.end(true);
  });
  srv.begin();
}
void loop() { srv.handleClient(); delay(2); }
