#!/bin/bash
# Flash MCUboot at 0x0 (the sysbuild runner puts it at 0x20000) and the app.
#   ./flash.sh [release]
DIR=build; [ "$1" = release ] && DIR=build-release
docker exec esp32 bash -lc "cd /root/build/zephyr/zephyr && source /root/build/zephyr/.venv/bin/activate && \
  esptool --chip esp32c6 --port /dev/ttyACM0 --after no-reset write-flash 0x0 /root/build/smartwatch/$DIR/mcuboot/zephyr/zephyr.bin 2>&1 | grep -E 'Wrote|rror' ; \
  timeout 300 west flash -d /root/build/smartwatch/$DIR --domain smartwatch --esp-device /dev/ttyACM0 2>&1 | grep -E 'Wrote|ERROR|rror'"
