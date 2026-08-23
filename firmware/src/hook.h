// Summon the HA lockout watchdog the moment the controller locks, instead of
// waiting out its 5-minute poll. One HTTP POST to a Home Assistant webhook --
// unauthenticated by design (HA webhooks are bearer-by-URL, LAN-only here),
// fire-and-forget, rate-limited. The watchdog re-verifies before it cuts
// anything, so a spurious call costs one status read on the HA side.
#pragma once
#include <stdint.h>

namespace hook {

void        begin();                 // loads the URL from NVS
void        setUrl(const char *url); // empty string disables; persisted
const char *url();
bool        configured();
// Call from the MAIN loop, never from relayTask: fires when the lockout bit
// has been held LOCK_HOOK_HOLD_MS, at most once per LOCK_HOOK_REPEAT_MS.
void        tick();
uint32_t    fired();                 // count, for /api/status
const char *lastResult();

}  // namespace hook
