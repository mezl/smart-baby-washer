#include "hook.h"

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>

#include "cn2.h"
#include "config.h"

namespace hook {

static char     s_url[96]  = {0};
static uint32_t s_fired    = 0;
static uint32_t s_last_ms  = 0;
static char     s_res[48]  = "never fired";
static Preferences s_prefs;

const char *url()        { return s_url; }
bool        configured() { return s_url[0] != 0; }
uint32_t    fired()      { return s_fired; }
const char *lastResult() { return s_res; }

void setUrl(const char *u) {
  strncpy(s_url, u ? u : "", sizeof(s_url) - 1);
  s_url[sizeof(s_url) - 1] = 0;
  s_prefs.begin("d8link", false);
  s_prefs.putString("hook", s_url);
  s_prefs.end();
  Serial.printf("[hook ] url = %s\n", s_url[0] ? s_url : "(none)");
}

void begin() {
  s_prefs.begin("d8link", true);
  s_prefs.getString("hook", s_url, sizeof(s_url));
  s_prefs.end();
}

// http://host:port/path only. No TLS on purpose: it keeps this a two-second
// WiFiClient call in the main loop instead of a 20 KB handshake, and the HA
// box is on the same LAN.
static bool post() {
  char host[40]; uint16_t port = 80; const char *path;
  const char *p = s_url;
  if (strncmp(p, "http://", 7) != 0) { snprintf(s_res, sizeof(s_res), "url not http://"); return false; }
  p += 7;
  const char *slash = strchr(p, '/');
  path = slash ? slash : "/";
  size_t hl = slash ? (size_t)(slash - p) : strlen(p);
  if (hl >= sizeof(host)) { snprintf(s_res, sizeof(s_res), "host too long"); return false; }
  memcpy(host, p, hl); host[hl] = 0;
  char *colon = strchr(host, ':');
  if (colon) { *colon = 0; port = (uint16_t)atoi(colon + 1); }

  WiFiClient c;
  c.setTimeout(2000);
  if (!c.connect(host, port, 2000)) { snprintf(s_res, sizeof(s_res), "connect failed"); return false; }
  c.printf("POST %s HTTP/1.1\r\nHost: %s\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", path, host);
  uint32_t t0 = millis();
  while (c.connected() && !c.available() && millis() - t0 < 2000) delay(10);
  String line = c.readStringUntil('\n');
  c.stop();
  snprintf(s_res, sizeof(s_res), "%s", line.length() ? line.c_str() : "no response");
  return line.indexOf("200") > 0;
}

void tick() {
  if (!configured() || WiFi.status() != WL_CONNECTED) return;
  if (cn2::lockedForMs() < LOCK_HOOK_HOLD_MS) return;
  if (s_last_ms && millis() - s_last_ms < LOCK_HOOK_REPEAT_MS) return;
  s_last_ms = millis() | 1;
  s_fired++;
  const bool ok = post();
  Serial.printf("[hook ] lockout webhook -> %s\n", s_res);
  (void)ok;
}

}  // namespace hook
