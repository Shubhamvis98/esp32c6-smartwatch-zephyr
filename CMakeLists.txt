# SPDX-License-Identifier: Apache-2.0
# Smartwatch firmware for the Waveshare ESP32-C6-Touch-LCD-1.69 (Zephyr port
# of the ESP-IDF firmware: the app/ and ui/ code is shared, compat/ maps the
# ESP-IDF and FreeRTOS calls it makes onto Zephyr).

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(smartwatch)

file(GLOB WATCH_SRCS
  src/*.c src/hal/*.c src/app/*.c src/ui/*.c src/ui/faces/*.c src/port/*.c
  compat/*.c lib/cjson/*.c)


# The face-upload page, linked in like ESP-IDF's EMBED_FILES
list(APPEND WATCH_SRCS compat/faces_html.S)
set_source_files_properties(compat/faces_html.S PROPERTIES
  COMPILE_DEFINITIONS "FACES_HTML=\"${CMAKE_CURRENT_SOURCE_DIR}/web/faces.html\""
  OBJECT_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/web/faces.html)

target_sources(app PRIVATE ${WATCH_SRCS})

# compat/ comes first so its esp_*.h and freertos/*.h win over hal_espressif's
target_include_directories(app BEFORE PRIVATE compat src lib/cjson)
# ui/theme.c reaches into LVGL's private theme header
target_include_directories(app PRIVATE ${ZEPHYR_LVGL_MODULE_DIR})
# malloc & co. on the kernel heap (compat/heap_wrap.c)
zephyr_ld_options(-Wl,--wrap=malloc -Wl,--wrap=free -Wl,--wrap=calloc
                  -Wl,--wrap=realloc -Wl,--wrap=aligned_alloc)

target_compile_options(app PRIVATE
  "SHELL:$<$<COMPILE_LANGUAGE:C>:-include ${CMAKE_CURRENT_SOURCE_DIR}/compat/watch_fs.h>"
  "SHELL:$<$<COMPILE_LANGUAGE:C>:-include ${CMAKE_CURRENT_SOURCE_DIR}/compat/lv_compat.h>"
  -Wno-unused-function -Wno-format-truncation -Wno-unused-variable)
target_compile_definitions(app PRIVATE WATCH_LOG_LEVEL=3 _GNU_SOURCE)
