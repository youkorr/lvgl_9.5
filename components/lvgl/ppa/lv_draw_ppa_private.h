/**
 * @file lv_draw_ppa_private.h
 * Fixed PPA private header for LVGL 9.4 on ESP32-P4
 * Backported from https://github.com/lvgl/lvgl/pull/9162
 * Adapted for C++ compilation (ESPHome build system)
 */

#ifndef LV_DRAW_PPA_PRIVATE_FIXED_H
#define LV_DRAW_PPA_PRIVATE_FIXED_H

/*********************
*      INCLUDES
*********************/
#include "lvgl.h"
#include "src/lv_conf_internal.h"
#include "src/draw/lv_draw_private.h"
#include "src/draw/lv_draw_buf_private.h"
#include "src/display/lv_display_private.h"
#include "src/misc/lv_area_private.h"

/* The ppa driver depends heavily on the esp-idf headers */
#include "sdkconfig.h"

#ifndef CONFIG_SOC_PPA_SUPPORTED
#error "This SoC does not support PPA"
#endif

#include "driver/ppa.h"
#include "esp_heap_caps.h"
#include "esp_err.h"
#include "hal/color_hal.h"
#include "esp_cache.h"
#include "esp_log.h"

/*********************
*      DEFINES
*********************/

/**********************
*      TYPEDEFS
**********************/
typedef struct lv_draw_ppa_unit {
    lv_draw_unit_t base_unit;
    lv_draw_task_t * task_act;
    ppa_client_handle_t srm_client;
    ppa_client_handle_t fill_client;
    ppa_client_handle_t blend_client;
    uint8_t * buf;
} lv_draw_ppa_unit_t;

/**********************
*   STATIC FUNCTIONS
**********************/

static inline bool ppa_src_cf_supported(lv_color_format_t cf)
{
    switch(cf) {
        case LV_COLOR_FORMAT_RGB565:
        case LV_COLOR_FORMAT_ARGB8888:
        case LV_COLOR_FORMAT_XRGB8888:
            return true;
        default:
            return false;
    }
}

static inline bool ppa_dest_cf_supported(lv_color_format_t cf)
{
    switch(cf) {
        case LV_COLOR_FORMAT_RGB565:
        case LV_COLOR_FORMAT_RGB888:
        case LV_COLOR_FORMAT_ARGB8888:
            return true;
        default:
            return false;
    }
}

static inline ppa_fill_color_mode_t lv_color_format_to_ppa_fill(lv_color_format_t lv_fmt)
{
    switch(lv_fmt) {
        case LV_COLOR_FORMAT_RGB565:
            return PPA_FILL_COLOR_MODE_RGB565;
        case LV_COLOR_FORMAT_RGB888:
            return PPA_FILL_COLOR_MODE_RGB888;
        case LV_COLOR_FORMAT_ARGB8888:
            return PPA_FILL_COLOR_MODE_ARGB8888;
        default:
            return PPA_FILL_COLOR_MODE_RGB565;
    }
}

static inline ppa_blend_color_mode_t lv_color_format_to_ppa_blend(lv_color_format_t lv_fmt)
{
    switch(lv_fmt) {
        case LV_COLOR_FORMAT_RGB565:
            return PPA_BLEND_COLOR_MODE_RGB565;
        case LV_COLOR_FORMAT_RGB888:
            return PPA_BLEND_COLOR_MODE_RGB888;
        case LV_COLOR_FORMAT_ARGB8888:
            return PPA_BLEND_COLOR_MODE_ARGB8888;
        default:
            return PPA_BLEND_COLOR_MODE_RGB565;
    }
}

static inline ppa_srm_color_mode_t lv_color_format_to_ppa_srm(lv_color_format_t lv_fmt)
{
    switch(lv_fmt) {
        case LV_COLOR_FORMAT_RGB565:
            return PPA_SRM_COLOR_MODE_RGB565;
        case LV_COLOR_FORMAT_RGB888:
            return PPA_SRM_COLOR_MODE_RGB888;
        case LV_COLOR_FORMAT_XRGB8888:
            return PPA_SRM_COLOR_MODE_ARGB8888;
        default:
            return PPA_SRM_COLOR_MODE_RGB565;
    }
}

#endif /* LV_DRAW_PPA_PRIVATE_FIXED_H */
