/**
 * @file lv_draw_ppa_buf.c
 * Fixed PPA buffer cache handling for LVGL 9.4 on ESP32-P4
 * Backported from https://github.com/lvgl/lvgl/pull/9162
 * Adapted for C++ compilation (ESPHome build system)
 */

#include "sdkconfig.h"
#ifdef CONFIG_SOC_PPA_SUPPORTED

#include "lv_draw_ppa_private.h"
#include "lv_draw_ppa.h"

/**********************
 *  STATIC PROTOTYPES
 **********************/
static void invalidate_cache(const lv_draw_buf_t * draw_buf, const lv_area_t * area);

/**********************
 *   GLOBAL FUNCTIONS
 **********************/
void lv_draw_buf_ppa_init_handlers(void)
{
    lv_draw_buf_handlers_t * handlers = lv_draw_buf_get_handlers();
    handlers->invalidate_cache_cb = invalidate_cache;
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

static void invalidate_cache(const lv_draw_buf_t * draw_buf, const lv_area_t * area)
{
    LV_UNUSED(area);
    esp_cache_msync(draw_buf->data, draw_buf->data_size,
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
}

#endif /* CONFIG_SOC_PPA_SUPPORTED */
