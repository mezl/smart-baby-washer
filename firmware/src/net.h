#pragma once
#include <Arduino.h>

// WiFi + mDNS + ArduinoOTA. Everything that has to keep working even when the
// payload does not.
namespace net {

void begin();      // joins WiFi (bounded wait), starts mDNS and espota
void loop();       // ArduinoOTA.handle() + the link watchdog

bool     connected();
String   ip();
int      rssi();
uint32_t downMs();     // 0 when up, else how long the link has been down

}  // namespace net
