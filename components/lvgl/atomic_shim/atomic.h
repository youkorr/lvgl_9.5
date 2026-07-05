/**
 * @file atomic.h
 * @brief Shim header for LVGL FreeRTOS support on ESP-IDF
 *
 * The lvgl/lvgl 9.5.0 managed component's src/osal/lv_freertos.c does an
 * unconditional  #include "atomic.h"  (LVGL issue #7589). On ESP-IDF the real
 * header lives at <freertos/atomic.h>, so this shim redirects to it.
 *
 * This directory is a header-only local ESP-IDF component whose CMakeLists.txt
 * appends its own path to the global COMPILE_OPTIONS (-I). That is the only way
 * to get an include directory onto the SEPARATELY-compiled managed __idf_lvgl
 * component: PlatformIO routes -I build_flags to the app's CPPPATH only, while
 * ESP-IDF global COMPILE_OPTIONS reach every component.
 */

#pragma once

#ifdef ESP_PLATFORM
#include "freertos/atomic.h"
#endif
