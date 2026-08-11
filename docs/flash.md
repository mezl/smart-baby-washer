# Flash it

PlatformIO, Arduino framework, no external library dependencies.

```bash
cp firmware/include/secrets.h.example firmware/include/secrets.h   # WiFi + OTA password
cd firmware
pio run -e c3 -t upload          # over USB
pio test -e native               # optional: 45 host-side tests
```

Open `http://baby-washer.local/`. If `.local` does not resolve on your network,
find the board's IP with `/api/version` or scan the subnet.

**Leave it in LISTEN mode until the wiring is confirmed** — see
[build.md](build.md).

## Over the air

Once the board is sealed inside the machine, USB means disassembly. Two paths:

```bash
# espota — needs a reverse connection back to your machine
OTA_PASSWORD=... pio run -e c3_ota -t upload                 # by mDNS
WASHER_IP=<board-ip> OTA_PASSWORD=... \
    pio run -e c3_ota_ip -t upload                           # by IP

# plain HTTP — needs only outbound TCP, so it crosses subnets and firewalls
curl -f -F "firmware=@.pio/build/c3/firmware.bin" \
     "http://baby-washer.local/update?key=$OTA_PASSWORD"
```

The HTTP path exists because `espota` has the board connect *back* to your
machine on a random high port. Across a subnet boundary that gets dropped, and it
presents as "No response from device" immediately after a **successful** auth —
which is a confusing failure to debug. The port is pinned to `45123` so it can at
least be allowed through a firewall.

⚠️ **Never flash mid-cycle.** An update interrupts forwarding for 15–20 s, and
while the ESP32 is down the panel link is down with it.

## Verify, don't assume

```bash
python3 tools/ota_smoke_test.py
```

Checks reachability, that the board is not in safe mode, that a **wrong key is
rejected**, then uploads and confirms the board came back running an image whose
MD5 matches what was sent, with the payload still alive. An OTA path that is never
exercised is one that has quietly rotted.

## What protects you

A failed or interrupted upload **never touches the running image** — the new one
goes to the inactive slot and the boot partition only switches after it verifies.

Beyond that, the firmware is deliberately defensive about not bricking itself out
of reach:

| Guard | The failure it prevents |
|---|---|
| **Boot-loop guard** (`RTC_NOINIT`, 3 strikes → safe mode) | flashing something that crashes before WiFi comes up. Three resets without a healthy uptime and the payload is disabled, leaving only WiFi and OTA. Power-cycle clears it |
| **Healthy-uptime gate** — 30 s **with WiFi** | an image that runs but cannot be reached is not a good image |
| **Two independent OTA paths** | `espota`'s reverse connection is a single point of failure |
| **mDNS hostname, not IP** | the DHCP lease is not yours to depend on |
| **Watchdog fed during the flash write** | the write blocks for seconds and would otherwise trip the watchdog mid-flash |
| **Failed OTA → restart** | the previous image is intact and bootable, so a clean restart beats limping on |
| **WiFi re-kick every 10 s, reboot after 60 s** | auto-reconnect alone wedges after the *access point* reboots, leaving the board alive but unreachable |
| **1.9 MB app slots** | running out of headroom is a miserable way to discover you can no longer OTA |
| **UARTs left open during the flash** | a closed UART lets TX float, and a floating RX at the panel reads as a break condition |

⚠️ **Automatic image rollback is not available.** Arduino-ESP32 ships a bootloader
without `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`, so the rollback call is usually a
no-op. **The boot-loop guard is what actually protects you.**

⚠️ **The HTTP `/update` endpoint is reachable by anything on your LAN.** The
password is not optional. `secrets.h` is gitignored — keep it that way, and treat
the OTA password as a real credential.
