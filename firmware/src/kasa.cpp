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

// A REAL power cycle -- possible only on the Shelly: toggle_after makes the
// PLUG restore its own relay after hold_s, while this board is dead. On the
// HS103 every mechanism for this was tested and failed (countdown cancelled
// by relay writes, schedules never fire, one-rule table); do not offer it
// there -- a Kasa "cycle" would strand the machine off.
bool powerCycle(uint16_t hold_s) {
  if (s_type != PLUG_SHELLY) { snprintf(s_err, sizeof(s_err), "cycle needs shelly"); return false; }
  if (hold_s < 5) hold_s = 5;
  if (hold_s > 600) hold_s = 600;
  char path[80];
  snprintf(path, sizeof(path), "/rpc/Switch.Set?id=0&on=false&toggle_after=%u", hold_s);
  Serial.printf("[plug ] SELF POWER CYCLE: off now, plug restores in %u s\n", hold_s);
  return shellyCall(path);
}

// ---- plug power telemetry (Shelly only) -----------------------------------
// Polled from the MAIN loop every few seconds; a 30-minute ring mirrors the
// temperature/flow trends on the dev page. Watts as uint16 (heater ~1.3 kW).
#define PW_N 600                       // 30 min at 3 s
static uint16_t s_pw[PW_N];
static uint16_t s_pw_head = 0, s_pw_len = 0;
static uint16_t s_pw_now = 0;
static uint32_t s_pw_last = 0;
static bool     s_pw_ok = false;

void powerPoll() {
  if (s_type != PLUG_SHELLY || !s_ip[0]) return;
  if (millis() - s_pw_last < 3000) return;
  s_pw_last = millis();
  if (WiFi.status() != WL_CONNECTED) return;
  WiFiClient c;
  c.setTimeout(1500);
  if (!c.connect(s_ip, 80, 1500)) { s_pw_ok = false; return; }
  c.printf("GET /rpc/Switch.GetStatus?id=0 HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n", s_ip);
  uint32_t t0 = millis();
  String body;
  while ((c.connected() || c.available()) && millis() - t0 < 1500) {
    while (c.available()) body += (char)c.read();
    delay(5);
  }
  c.stop();
  int k = body.indexOf("\"apower\":");
  if (k < 0) { s_pw_ok = false; return; }
  float w = body.substring(k + 9).toFloat();
  if (w < 0) w = 0;
  s_pw_now = (uint16_t)(w + 0.5f);
  s_pw_ok = true;
  s_pw[s_pw_head] = s_pw_now;
  s_pw_head = (uint16_t)((s_pw_head + 1) % PW_N);
  if (s_pw_len < PW_N) s_pw_len++;
}
uint16_t plugWatts()  { return s_pw_now; }
bool     plugWattsOk(){ return s_pw_ok; }
String   plugPowerHex() {
  String o; o.reserve(s_pw_len * 4);
  static const char *H = "0123456789ABCDEF";
  uint16_t start = (uint16_t)((s_pw_head + PW_N - s_pw_len) % PW_N);
  for (uint16_t k = 0; k < s_pw_len; k++) {
    uint16_t v = s_pw[(start + k) % PW_N];
    o += H[(v >> 12) & 15]; o += H[(v >> 8) & 15]; o += H[(v >> 4) & 15]; o += H[v & 15];
  }
  return o;
}

uint8_t plugType() { return s_type; }
void setPlugType(uint8_t t) {
  s_type = t;
  Preferences p; p.begin("d8link", false); p.putUChar("plugt", t); p.end();
  Serial.printf("[plug ] type = %s\n", t == PLUG_SHELLY ? "shelly" : "kasa");
}

}  // namespace kasa
