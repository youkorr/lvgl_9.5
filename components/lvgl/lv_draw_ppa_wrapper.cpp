/**
 * @file lv_draw_ppa_wrapper.cpp
 * Wrapper to compile the fixed PPA source files within ESPHome's build system.
 *
 * The PPA translation units live at the component TOP LEVEL with a .tcc
 * extension. Since ESPHome 2026.6 an external component's source files are
 * copied into the build from its declared resources, which only include
 * TOP-LEVEL files (external components cannot set recursive_sources), so a
 * ppa/ subdirectory would be dropped -> "ppa/lv_draw_ppa.c: No such file".
 * .tcc is copied but not standalone-compiled, so #including the units here
 * keeps them one translation unit (no duplicate symbols) and preserves the
 * C linkage the rest of the code expects.
 */

#include "esphome/core/defines.h"

#ifdef USE_LVGL_PPA

extern "C" {
#include "lv_draw_ppa.tcc"
#include "lv_draw_ppa_fill.tcc"
#include "lv_draw_ppa_img.tcc"
#include "lv_draw_ppa_buf.tcc"
#include "lvgl_ppa_accel_v9.tcc"
}

#endif /* USE_LVGL_PPA */
