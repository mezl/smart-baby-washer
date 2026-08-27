#include "kasa.h"

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <cn2core.h>

namespace kasa {

static char     s_ip[20]   = {0};
static uint8_t  s_type     = 0;     // PLUG_KASA=0, PLUG_SHELLY=1
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
  s_type = s_prefs.getUChar("plugt", PLUG_KASA);
  s_prefs.end();
  strncpy(s_ip, v.c_str(), sizeof(s_ip) - 1);
}

// ---- Shelly Gen2+ (the plug since 2026-08-26): one HTTP GET ---------------
// GET /rpc/Switch.Set?id=0&on=false -- no framing, no cipher. The device is
// configured with initial_state=on, so mains restoration re-energises by
// itself; a one-way protective cut can never strand the machine the way the
// HS103's last-state behaviour could.
static bool shellyCall(const char *path, uint32_t timeout_ms = 4000) {
  if (!s_ip[0]) { snprintf(s_err, sizeof(s_err), "no plug configured"); return false; }
  if (WiFi.status() != WL_CONNECTED) { snprintf(s_err, sizeof(s_err), "wifi down"); return false; }
  WiFiClient c;
  c.setTimeout(timeout_ms);
  if (!c.connect(s_ip, 80, timeout_ms)) { snprintf(s_err, sizeof(s_err), "connect failed"); return false; }
  c.printf("GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n", path, s_ip);
  uint32_t t0 = millis();
  while (c.connected() && !c.available() && millis() - t0 < timeout_ms) delay(10);
  String line = c.readStringUntil('\n');
  c.stop();
  s_last = millis();
  const bool ok = line.indexOf("200") > 0;
  snprintf(s_err, sizeof(s_err), ok ? "ok" : "http: %s", line.c_str());
  return ok;
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

bool reachable() {
  return s_type == PLUG_SHELLY ? shellyCall("/rpc/Shelly.GetDeviceInfo")
                               : call("{\"system\":{\"get_sysinfo\":{}}}");
}

bool powerOff() {
  if (s_type == PLUG_SHELLY) {
    Serial.println("[plug ] opening the Shelly relay — initial_state=on means "
                   "the next mains event restores it, but until then: OFF");
    return shellyCall("/rpc/Switch.Set?id=0&on=false");
  }
  // Kasa: leave no stale countdown behind that could re-energise it later.
  call("{\"count_down\":{\"delete_all_rules\":{}}}");
  Serial.println("[kasa ] opening the relay — machine will STAY OFF until "
                 "someone restores it");
  return call("{\"system\":{\"set_relay_state\":{\"state\":0}}}");
}

uint8_t plugType() { return s_type; }
void setPlugType(uint8_t t) {
  s_type = t;
  Preferences p; p.begin("d8link", false); p.putUChar("plugt", t); p.end();
  Serial.printf("[plug ] type = %s\n", t == PLUG_SHELLY ? "shelly" : "kasa");
}

}  // namespace kasa
