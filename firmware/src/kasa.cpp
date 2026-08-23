#include "kasa.h"

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <cn2core.h>

namespace kasa {

static char     s_ip[20]   = {0};
static uint32_t s_last     = 0;
static char     s_err[64]  = "never used";
static Preferences s_prefs;

const char *plugIp()    { return s_ip; }
bool        configured(){ return s_ip[0] != 0; }
uint32_t    lastResultMs(){ return s_last; }
const char *lastError() { return s_err; }

void setPlug(const char *ip) {
  strncpy(s_ip, ip ? ip : "", sizeof(s_ip) - 1);
  s_ip[sizeof(s_ip) - 1] = 0;
  s_prefs.begin("d8link", false);
  s_prefs.putString("kasa", s_ip);
  s_prefs.end();
  Serial.printf("[kasa ] plug = %s\n", s_ip[0] ? s_ip : "(none)");
}

void begin() {
  s_prefs.begin("d8link", true);
  String v = s_prefs.getString("kasa", "");
  s_prefs.end();
  strncpy(s_ip, v.c_str(), sizeof(s_ip) - 1);
}

// One request/response on port 9999. Returns true if the reply contains
// err_code":0 -- the plug's own acknowledgement.
static bool call(const char *json, uint32_t timeout_ms = 3000) {
  if (!s_ip[0]) { snprintf(s_err, sizeof(s_err), "no plug configured"); return false; }
  if (WiFi.status() != WL_CONNECTED) {
    snprintf(s_err, sizeof(s_err), "wifi down"); return false;
  }
  WiFiClient c;
  c.setTimeout(timeout_ms);
  if (!c.connect(s_ip, 9999, timeout_ms)) {
    snprintf(s_err, sizeof(s_err), "connect failed"); return false;
  }
  const uint16_t n = (uint16_t)strlen(json);
  uint8_t buf[256];
  if (n + 4 > sizeof(buf)) { c.stop(); snprintf(s_err, sizeof(s_err), "payload too big"); return false; }
  buf[0] = 0; buf[1] = 0; buf[2] = (uint8_t)(n >> 8); buf[3] = (uint8_t)n;
  cn2core::kasaEncrypt(json, buf + 4, n);
  c.write(buf, n + 4);
  c.flush();

  uint32_t t0 = millis();
  uint8_t hdr[4]; size_t got = 0;
  while (got < 4 && millis() - t0 < timeout_ms) {
    int r = c.read(hdr + got, 4 - got);
    if (r > 0) got += r; else delay(5);
  }
  if (got < 4) { c.stop(); snprintf(s_err, sizeof(s_err), "no reply header"); return false; }
  uint32_t len = ((uint32_t)hdr[2] << 8) | hdr[3];
  if (len > sizeof(buf)) len = sizeof(buf);
  got = 0;
  while (got < len && millis() - t0 < timeout_ms) {
    int r = c.read(buf + got, len - got);
    if (r > 0) got += r; else delay(5);
  }
  c.stop();
  char out[257];
  cn2core::kasaDecrypt(buf, out, (uint16_t)got);
  out[got] = 0;
  s_last = millis();
  // get_sysinfo is longer than the buffer, so its trailing err_code never
  // arrives. Any recognisable field from it proves the round trip just as well,
  // and the commands that matter (add_rule, set_relay_state) reply short enough
  // to carry err_code.
  if (strstr(out, "\"err_code\":0") || strstr(out, "\"sw_ver\"")) {
    snprintf(s_err, sizeof(s_err), "ok");
    return true;
  }
  snprintf(s_err, sizeof(s_err), "plug said: %.50s", out);
  return false;
}

bool reachable() { return call("{\"system\":{\"get_sysinfo\":{}}}"); }

bool powerOff() {
  // Leave no stale countdown behind that could re-energise the machine later.
  call("{\"count_down\":{\"delete_all_rules\":{}}}");
  Serial.println("[kasa ] opening the relay — machine will STAY OFF until "
                 "someone restores it");
  return call("{\"system\":{\"set_relay_state\":{\"state\":0}}}");
}

}  // namespace kasa
