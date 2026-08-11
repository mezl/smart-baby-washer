#!/usr/bin/env bash
# Run the host-side tests and report coverage for lib/cn2core/cn2core.h.
#
# Only cn2core is measured. The rest of the firmware talks to UARTs, NVS and
# FreeRTOS and cannot be reached from a host test, so folding it in would give a
# number that says nothing useful. cn2.cpp calls into this same header, so the
# lines measured here are the lines that run on the machine.
#
# gcov must run from the project root or it cannot resolve the source paths
# recorded in the .gcno, and silently emits an empty report.
set -euo pipefail
cd "$(dirname "$0")/.."
PIO="${PIO:-$HOME/.platformio/penv/bin/pio}"

rm -rf .pio/build/native
"$PIO" test -e native

GCNO=$(find .pio/build/native -name 'test_main.gcno' | head -1)
[ -n "$GCNO" ] || { echo "no coverage data — did --coverage reach the linker?"; exit 1; }
OBJDIR=$(dirname "$GCNO")

OUT=$(gcov -b -o "$OBJDIR" "$GCNO" 2>/dev/null)
mv -f cn2core.h.gcov "$OBJDIR/" 2>/dev/null || true
rm -f ./*.gcov

echo
echo "  ── coverage ──────────────────────────────────────────"
python3 - <<PY
import re
out = """$OUT"""
blocks = re.split(r"File '", out)
for b in blocks:
    if not b.startswith('lib/cn2core/cn2core.h'): continue
    lines  = re.search(r'Lines executed:([\d.]+)% of (\d+)', b)
    brs    = re.search(r'Branches executed:([\d.]+)% of (\d+)', b)
    taken  = re.search(r'Taken at least once:([\d.]+)% of (\d+)', b)
    print("  lib/cn2core/cn2core.h")
    if lines: print("    lines     %6s%%   (%s executable)" % (lines.group(1), lines.group(2)))
    if brs:   print("    branches  %6s%%   (%s branches)"   % (brs.group(1),   brs.group(2)))
    if taken: print("    taken     %6s%%   at least once"   % taken.group(1))
PY
echo "  ──────────────────────────────────────────────────────"
echo

G="$OBJDIR/cn2core.h.gcov"
if [ -f "$G" ] && grep -q '#####' "$G"; then
  echo "  uncovered lines:"
  grep -n '#####' "$G" | sed 's/^/    /'
fi
