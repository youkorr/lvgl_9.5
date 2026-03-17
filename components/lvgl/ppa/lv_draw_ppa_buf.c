/**
 * @file lv_draw_ppa_buf.c
 * Fixed PPA buffer cache handling for LVGL 9.4 on ESP32-P4
 * Backported from https://github.com/lvgl/lvgl/pull/9162
 * Adapted for C++ compilation (ESPHome build system)
 *
 * NOTE: We do NOT set the global invalidate_cache_cb handler because
 * it would affect ALL draw operations (software renderer included).
 * Cache sync is done per-operation in PPA dispatch, and only for
 * buffers in external (PSRAM) memory which is cached.
 */

#include "sdkconfig.h"
#ifdef CONFIG_SOC_PPA_SUPPORTED

#include "lv_draw_ppa_private.h"
#include "lv_draw_ppa.h"
#include "esp_memory_utils.h"

/**********************
 *   GLOBAL FUNCTIONS
 **********************/
void lv_draw_buf_ppa_init_handlers(void)
{
    /* Intentionally empty - see file header comment */
}

void lv_draw_ppa_cache_sync(lv_draw_buf_t * buf)
{
    if(buf == NULL || buf->data == NULL || buf->data_size == 0) return;

    /* Only sync if buffer is in external (PSRAM) memory, which is cached.
     * Internal SRAM is not cached on ESP32-P4, so esp_cache_msync would
     * either be a no-op or could crash on non-cacheable regions. */
    if(!esp_ptr_external_ram(buf->data)) return;

    esp_cache_msync(buf->data, LV_ROUND_UP(buf->data_size, LV_DRAW_BUF_ALIGN),
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
}

#endif /* CONFIG_SOC_PPA_SUPPORTED */
