#pragma once
#include <Arduino.h>

// Status/observation API, plus a second independent OTA path over plain HTTP.
// Having two ways in matters: espota needs a reverse connection back to the
// uploading host, which is precisely the thing that breaks across subnets.
// `curl -F` needs nothing but an outbound TCP connection to the board.
namespace web {

void begin();
void loop();

}  // namespace web
