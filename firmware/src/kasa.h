#pragma once
#include <stdint.h>

// Minimal TP-Link Kasa client — enough to power-cycle a plug from the ESP32.
//
// The plug is the ONLY way to stop a load the controller has latched on, and
// the ESP32 is powered from the machine it is switching. So a naive "off" is a
// suicide note: the board dies before it can send "on".
//
// The plug's own count_down rule is the way out. Arm "turn on in N seconds"
// FIRST, then switch off. The plug restores itself with no help from us.
namespace kasa {
void        begin();                  // loads the plug address from NVS
void        setPlug(const char *ip);
const char *plugIp();
bool        configured();

// Opens the relay and LEAVES IT OPEN. Returns false if the plug did not
// acknowledge, in which case nothing was switched -- the safe direction.
//
// Not a power cycle, deliberately, and it took a live failure to learn why:
// setting the relay manually CANCELS any pending count_down rule, so arming
// "turn back on in N s" and then switching off silently disarms the timer and
// the appliance stays dead. Arming it while already off does work -- but this
// board is powered by what it is switching, so by then it no longer exists.
//
// Staying off is also the correct behaviour. This fires because a heater is
// energised that nothing is commanding and the controller has stopped
// listening. Restoring power unattended just re-enters that state.
bool powerOff();

bool     reachable();          // probes get_sysinfo
uint32_t lastResultMs();       // millis() of the last attempt
const char *lastError();
}  // namespace kasa
