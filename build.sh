#!/bin/bash
# Build the Zephyr smartwatch in docker (container "esp32").
#   ./build.sh [release] [-p]
# Default: debug build (UART shell + test commands) in build/.
# release: adds release.conf (no shell) and builds in build-release/.
# -p = pristine.  Prints only errors and the memory summary; the full log is
# in /tmp/smartwatch-build.log.
DIR=build; EXTRA=; P=
for a in "$@"; do
  case $a in
    release) DIR=build-release; EXTRA="-- -Dsmartwatch_EXTRA_CONF_FILE=release.conf" ;;
    -p) P=-p ;;
  esac
done
docker exec esp32 bash -lc "cd /root/build/zephyr/zephyr && source /root/build/zephyr/.venv/bin/activate && west build --sysbuild $P -b esp32c6_touch_lcd_1_69/esp32c6/hpcore -d /root/build/smartwatch/$DIR /root/build/smartwatch $EXTRA" > /tmp/smartwatch-build.log 2>&1
rc=$?
grep -E "error:|undefined reference|multiple definition|region .* overflowed|Error [0-9]|CMake Error|devicetree error|warning: implicit declaration" /tmp/smartwatch-build.log | sed -E 's#/root/build/smartwatch/##; s#/root/build/zephyr/##' | awk '!seen[$0]++' | head -40
grep -E "^\s+(FLASH|RAM|ROM|iram|dram|irom|drom)[A-Za-z0-9_ ]*:" /tmp/smartwatch-build.log | tail -6
echo "rc=$rc ($DIR)"
