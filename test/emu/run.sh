#!/usr/bin/env bash
# Boot a CrossDink x4-pro-debug build in x4prosim, walk test/emu/steps.txt and compare
# every shot with goldens/NAME.png (masks: goldens/NAME.mask). Fails on a failed step,
# a crash or [ERR] in the firmware log, a DC-unbalanced LUT, or a golden mismatch.
# usage: test/emu/run.sh <pio build dir> <x4prosim tree> <out dir>
#   pio build dir: bootloader.bin, partitions.bin, firmware.bin (.pio/build/x4-pro-debug)
#   x4prosim tree: a checkout with build/qemu-system-xtensa, or an install with bin/
set -uo pipefail
here=$(cd "$(dirname "$0")" && pwd)
fw=$1 sim=$2/x4prosim out=$3
rm -rf "$out" && mkdir -p "$out/sd/books" || exit 1
sh "$sim/mkflash.sh" "$fw" "$out/flash.bin" > /dev/null || exit 1
cp "$here/../epubs/test_reader_rendering_matrix.epub" "$out/sd/books/"
python3 "$sim/mksd.py" "$out/sd.img" 64 "$out/sd" || exit 1
QEMU_EXTRA="-d guest_errors -D $out/guest.log" \
  python3 "$sim/sim.py" --json "$out/flash.bin" "$out/sd.img" "$out/run" "@$here/steps.txt" | tee "$out/steps.jsonl"
rc=${PIPESTATUS[0]}
fail() { echo "FAIL: $*"; rc=1; }
grep -nE "Guru Meditation|panic'ed|abort\(\) was called|Backtrace:|\[ERR\]" "$out/run/log.txt" \
  && fail "crash or [ERR] in the firmware log"
# Display safety (x4prosim uc8179 DC ledger): every register LUT must be DC-balanced.
grep -n "DC-unbalanced" "$out/guest.log" && fail "DC-unbalanced LUT"
for s in "$out"/run/*.png; do
  n=$(basename "$s")
  case $n in diff-*) continue ;; esac
  g=$here/goldens/$n
  if [ -f "$g" ]; then
    python3 "$sim/shotdiff.py" --diff "$out/run/diff-$n" "$s" "$g" || fail "$n differs from goldens/$n"
  else
    echo "NEW: $n has no golden (copy it to test/emu/goldens/ to pin it)"
  fi
done
for g in "$here"/goldens/*.png; do
  [ -f "$g" ] && [ ! -f "$out/run/$(basename "$g")" ] && fail "no shot for goldens/$(basename "$g")"
done
exit $rc
