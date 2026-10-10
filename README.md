# C6 Watch on Zephyr RTOS

This is a Zephyr port of the ESP-IDF watch firmware in `../idf/displaytest` for
the Waveshare ESP32-C6-Touch-LCD-1.69. Every feature in that project's
README is here: watchfaces (including custom JSON faces uploaded over Wi-Fi),
activity, music, notifications and Gadgetbridge, phone, navigation,
weather, calendar, alarms and notify alarms, timer, stopwatch, recorder, sound
meter, sensors, camera shutter, calculator, torch, quick panel, Wi-Fi and
internet time, Bluetooth watch and HID modes, OTA updates, configurable
PWR/BOOT buttons, theme, display, sound, factory reset and power off.

The application code (`src/app`, `src/ui`, `src/hal`) is the ESP-IDF code,
mostly unchanged. The pieces that differ live in `compat/` and in a few
ported modules.

## How the port works

| Area | Zephyr implementation |
|------|-----------------------|
| FreeRTOS tasks, queues, semaphores, event groups, stream buffers, timers | `compat/frt.c` on `k_thread`/`k_sem`/`k_msgq`/`k_event`… |
| `esp_err`, `esp_log`, `esp_timer`, `esp_mac`, … | headers in `compat/`, `compat/esp_misc.c` |
| NVS | `compat/nvs_settings.c` on the Zephyr settings subsystem (`storage` partition) |
| Files (`fopen`, `opendir`, …) | `compat/wfs.c` on LittleFS (`data` partition), force-included via `compat/watch_fs.h` |
| LVGL | Zephyr's LVGL module (v9) + `src/lvgl_port.c` |
| Display, touch, IMU, RTC, PMIC, audio codec | Zephyr drivers / I2C, `src/hal/` |
| Wi-Fi | `src/app/wifi_mgr.c` on `net_mgmt` (scan, connect, DHCP, SNTP) |
| Bluetooth LE | `src/app/ble_mgr.c` on the Zephyr host: NUS for Gadgetbridge, HID over GATT, two identities (watch / HID) |
| Web server (face upload, `/update`) | `compat/http_server.c`, the `esp_http_server` API over Zephyr sockets |
| OTA | MCUboot (sysbuild, swap-using-move), `compat/ota_zephyr.c` |
| Heap | `malloc` & co. are link-wrapped onto the kernel heap so the app, LVGL, Wi-Fi and BT share one pool; the C library's leftover arena is an overflow (`compat/heap_wrap.c`) |

The flash layout is in `partitions.dtsi`, which both the app and MCUboot use:

| Partition | Address | Size |
|-----------|---------|------|
| MCUboot   | 0x0     | 128 K |
| slot0 (running image) | 0x20000 | 4096 K |
| slot1 (OTA download)  | 0x420000 | 4092 K (one sector smaller, needed by swap-using-move) |
| LP core   | 0x820000 | |
| storage (settings/NVS) | 0x830000 | 192 K |
| data (LittleFS) | 0x860000 | 7680 K |

## Building and flashing

The scripts run inside the `esp32` docker container, where this directory
is `/root/build/smartwatch`:

```sh
./build.sh            # debug build in build/: UART shell + `watch` test commands
./build.sh release    # release build in build-release/: no shell, more RAM
./flash.sh [release]  # writes MCUboot at 0x0 with esptool, then the app at 0x20000
```

Add `-p` to `build.sh` for a pristine build. The release build is
`prj.conf` plus `release.conf`. It logs the free heap once start-up is done
(`main: Smartwatch running, heap free …`).

`west flash` alone is not enough. The sysbuild ESP32 runner writes MCUboot
to 0x20000 instead of 0x0, so the old bootloader keeps running, and it
ignores the OTA trailer. `flash.sh` works around this.

### Zephyr patch

`patches/zephyr-i2s_esp32-slab-leak.patch` fixes the ESP32 I2S driver, and it
is applied to the Zephyr tree in the container. When RX overflowed (the mic
task was starved for a moment), the driver kept the block being filled. TX did
the same when stopped mid-transfer. After four overflows every block was
gone, so the mic only returned silence and the audio task blocked forever.
Re-apply it after updating Zephyr:

```sh
cd /root/build/zephyr/zephyr && git apply /root/build/smartwatch/patches/zephyr-i2s_esp32-slab-leak.patch
```

## Shell test commands

The UART shell has a `watch` command for testing without touching the
watch:

```
watch tap X Y | swipe X1 Y1 X2 Y2 | btn pwr|boot click|double|hold|long
watch shot                         screenshot as hex lines
watch heap                         kernel heap used/free/peak
watch audio | mic [Hz] | beep [Hz]
watch wifi [scan|connect SSID [PASS]|on|off|sync|disconnect|forget]
watch ble [watch [nomedia]|hid|off|forget|name NAME]
watch http METHOD PATH [BODY]      request to the face server over loopback
watch ota [self [noreboot]|trailer] OTA state / copy the running image to slot1 and boot it
```

## Status

Tested on the board:

- **Every screen** renders: watchface, quick panel, Music, app menu and all apps, all Settings pages, About (shows Zephyr and the running slot).
- **Audio**
  - The sound meter reads the mic.
  - The recorder records (6 s, 0 samples dropped), plays back and deletes.
  - Beeps work.
- **Face server** over loopback: list, upload, download, delete, select, and the upload page. A custom face was uploaded and rendered.
- **OTA**
  - A full MCUboot test swap works: the image is written to slot1, swapped, and confirmed on the next boot.
  - Non-firmware uploads are rejected.
- **Wi-Fi**: scanning works, and a missing network is reported.
- **Bluetooth watch mode**, from a PC: Gadgetbridge protocol (time, notifications, music, weather), pairing and bonding, and the media-key service.
- **Bluetooth HID mode**: separate address, pairing works, and the HID service is ready.

Not yet tested on hardware:

- A real Wi-Fi connection: no credentials were available. That also leaves SNTP time sync and face upload from a browser untested.
- Gadgetbridge on a real phone, the camera shutter on a phone, and HID typing on a phone or PC.
- Physical button presses, raise-to-wake on the wrist, power off on battery, and factory reset.

## Known limits

- **RAM.** Wi-Fi, Bluetooth, LVGL and the app share one kernel heap: 180 KB in the debug build, 192 KB in the release build. A ~20 KB C-library arena is overflow for `malloc`.
  - Zephyr reserves more RAM up front than ESP-IDF did: fixed network and Bluetooth buffer pools, static driver stacks, and the shell. To compensate, LVGL uses a single 10 % draw buffer (no flush thread). App task stacks are sized from measured peaks: about 2× the peak, measured with Bluetooth, Gadgetbridge, a Wi-Fi scan and the web server all running.

    | Free kernel heap | Debug | Release |
    |------------------|-------|---------|
    | At boot, Bluetooth off | 55 KB | 67 KB |
    | Bluetooth on | 36 KB | about 48 KB |
    | Bluetooth + Gadgetbridge + Wi-Fi scan + web server | 34 KB (was 9 KB before the trim) | — |

  - The recorder buffer is 0.5 s instead of 1 s.
- **Watch-mode BLE test hook.** `watch ble watch nomedia` leaves out the media HID service. Use it only with hosts (BlueZ) that try to pair against it without an agent.
