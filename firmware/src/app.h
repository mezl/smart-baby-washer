#pragma once
#include <Arduino.h>

// Small window onto main.cpp's boot state, so web.cpp can report it without
// main.cpp having to know about the web server.
namespace app {

bool     safeMode();
uint32_t bootCount();
bool     imageMarkedGood();

}  // namespace app
