# D8 CN2 panel-link sniffer / relay

ESP32-C3 firmware: a transparent man-in-the-middle on the CN2 link that forwards
every byte and can rewrite any of them in flight.

```bash
cp include/secrets.h.example include/secrets.h    # then edit it
pio run -e c3 -t upload                           # USB
OTA_PASSWORD=... pio run -e c3_ota -t upload      # WiFi, by mDNS
pio test -e native                                # 45 host-side cases
./tools/coverage.sh                               # + line/branch coverage
```

PlatformIO, Arduino framework, no external library dependencies. The pure frame
logic — framing, checksums, byte rewriting, the streaming checksum, thinning and
the autodetect decisions — lives in [`lib/cn2core/cn2core.h`](lib/cn2core/cn2core.h)
with no Arduino dependency, so it runs on a host at 100 % line and branch
coverage. `src/cn2.cpp` calls into that same header, which is what stops the tests
drifting into a parallel copy of the logic.

Then open `http://d8-sniffer.local/` — the user app — or `/dev` for the
engineering page.

⚠️ **Start in LISTEN mode.** It cannot drive a CN2 line at all. See
[`docs/wiring.md`](../docs/build.md#3-tap-it--listen-first).

## Everything else

| | |
|---|---|
| wiring, pin choice, level shifting | [`docs/wiring.md`](../docs/build.md) |
| the protocol | [`docs/protocol.md`](../docs/protocol.md) |
| HTTP API | [`docs/api.md`](../docs/api.md) |
| web UI | [`docs/webui.md`](../docs/use.md) |
| OTA design and its guards | [`docs/ota.md`](../docs/flash.md) |
| diagnosis | [`docs/troubleshooting.md`](../docs/troubleshooting.md) |
| ⚠️ what the machine does not protect you from | [`docs/safety.md`](../docs/safety.md) |
