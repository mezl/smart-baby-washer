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

// Arms the plug's count_down to re-energise after off_s, then opens the relay.
// Returns false if the plug did not acknowledge -- in which case nothing was
// switched, which is the safe direction.
bool powerCycle(uint16_t off_s);

bool     reachable();          // probes get_sysinfo
uint32_t lastResultMs();       // millis() of the last attempt
const char *lastError();
}  // namespace kasa
