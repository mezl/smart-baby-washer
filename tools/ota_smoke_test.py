#!/usr/bin/env python3
"""End-to-end check that the D8 CN2 sniffer's OTA path still works.

Run this after touching anything in net.cpp, web.cpp or platformio.ini. An OTA
setup that is never exercised is an OTA setup that has quietly rotted, and the
board is going to live inside an appliance.

    python3 tools/ota_smoke_test.py                # full run, uploads firmware
    python3 tools/ota_smoke_test.py --no-upload    # reachability + auth only

Env:
    WASHER_HOST         default "baby-washer.local"
    OTA_PASSWORD    required — must match include/secrets.h
"""
import argparse
import hashlib
import os
import sys
import time
import urllib.error
import urllib.request

HOST = os.environ.get("WASHER_HOST", "baby-washer.local")
KEY = os.environ.get("OTA_PASSWORD", "change-me")
BASE = f"http://{HOST}"

results = []


def check(name, ok, detail=""):
    results.append((name, ok, detail))
    print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"  — {detail}" if detail else ""))
    return ok


def get_json(path, timeout=6):
    import json

    with urllib.request.urlopen(BASE + path, timeout=timeout) as r:
        return json.load(r)


def post_multipart(path, field, filename, blob, timeout=90):
    """Minimal multipart POST — no requests dependency."""
    boundary = "----d8smoke" + hashlib.md5(filename.encode()).hexdigest()[:12]
    body = b"".join(
        [
            f"--{boundary}\r\n".encode(),
            f'Content-Disposition: form-data; name="{field}"; '
            f'filename="{filename}"\r\n'.encode(),
            b"Content-Type: application/octet-stream\r\n\r\n",
            blob,
            f"\r\n--{boundary}--\r\n".encode(),
        ]
    )
    req = urllib.request.Request(
        BASE + path,
        data=body,
        headers={"Content-Type": f"multipart/form-data; boundary={boundary}"},
        method="POST",
    )
    return urllib.request.urlopen(req, timeout=timeout)


def wait_for_board(want_md5=None, timeout=60):
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            v = get_json("/api/version", timeout=4)
            if want_md5 is None or v.get("md5") == want_md5:
                return v
        except Exception:
            pass
        time.sleep(2)
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--no-upload", action="store_true")
    ap.add_argument("--firmware", default=".pio/build/c3/firmware.bin")
    args = ap.parse_args()

    print(f"\nD8 CN2 sniffer OTA smoke test — {BASE}\n")

    # 1. reachable at all
    try:
        v = get_json("/api/version")
        check(
            "board reachable",
            True,
            f"{v['name']} {v['version']} on {v['partition']}, "
            f"up {v['uptime_s']}s, heap {v['heap']}",
        )
    except Exception as e:
        check("board reachable", False, str(e))
        return finish()

    if v.get("safe_mode"):
        check("not in safe mode", False, "board is in SAFE MODE — power-cycle it")
    else:
        check("not in safe mode", True)

    # 2. status endpoint sane
    try:
        s = get_json("/api/status")
        check("status endpoint", True, f"mode={s['mode']} baud={s['baud']} rssi={s['rssi']}")
    except Exception as e:
        check("status endpoint", False, str(e))

    # 3. firmware present
    if not os.path.exists(args.firmware):
        check("firmware image present", False, args.firmware)
        return finish()
    blob = open(args.firmware, "rb").read()
    md5 = hashlib.md5(blob).hexdigest()
    check("firmware image present", True, f"{len(blob)} bytes, md5 {md5[:12]}…")

    # 4. a bad key must be refused. This is the check that matters most: the
    #    HTTP OTA path is reachable by anything on the LAN.
    try:
        post_multipart("/update?key=definitely-wrong-key", "firmware", "fw.bin", blob)
        check("bad key rejected", False, "upload was ACCEPTED with a wrong key")
    except urllib.error.HTTPError as e:
        check("bad key rejected", e.code == 401, f"HTTP {e.code}")
    except Exception as e:
        check("bad key rejected", False, str(e))
    time.sleep(3)

    if args.no_upload:
        return finish()

    # 5. real upload over the HTTP path
    try:
        r = post_multipart(f"/update?key={KEY}", "firmware", "fw.bin", blob)
        check("http upload accepted", r.status == 200, f"HTTP {r.status}")
    except Exception as e:
        check("http upload accepted", False, str(e))
        return finish()

    # 6. it came back running the image we just sent
    got = wait_for_board(want_md5=md5, timeout=60)
    if got:
        check("rebooted into new image", True, f"{got['version']} on {got['partition']}")
    else:
        cur = wait_for_board(timeout=10)
        check(
            "rebooted into new image",
            False,
            f"running md5 {cur['md5'][:12]}… expected {md5[:12]}…" if cur else "unreachable",
        )

    # 7. and the payload still runs
    try:
        s = get_json("/api/status")
        check("cn2 link running after update", s.get("open") is True, f"mode={s['mode']}")
    except Exception as e:
        check("cn2 link running after update", False, str(e))

    return finish()


def finish():
    bad = [n for n, ok, _ in results if not ok]
    print()
    if bad:
        print(f"FAILED: {len(bad)}/{len(results)} — {', '.join(bad)}")
        sys.exit(1)
    print(f"All {len(results)} checks passed.")
    sys.exit(0)


if __name__ == "__main__":
    main()
