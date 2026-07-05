/**
 * @file lv_draw_ppa_buf.c
 * Fixed PPA buffer cache handling for LVGL 9.5 on ESP32-P4
 * Backported from https://github.com/lvgl/lvgl/pull/9162
 * Adapted for C++ compilation (ESPHome build system)
 *
 * We register a global aligned allocator so that ALL LVGL draw buffers
 * are allocated in PSRAM with PPA_CACHE_LINE_SIZE alignment.  This
 * eliminates the per-call temp-buffer workaround in lv_draw_ppa_img.c
 * (which was allocating+freeing 20-30 times per frame and killing
 * throughput).
 */

#include "sdkconfig.h"
#ifdef CONFIG_SOC_PPA_SUPPORTED

#include "lv_draw_ppa_private.h"
#include "lv_draw_ppa.h"
#include "src/draw/lv_draw_buf.h"
#include "esp_memory_utils.h"
#include <string.h>

/**********************
 *  STATIC FUNCTIONS
 **********************/

static void * ppa_buf_malloc(size_t size, lv_color_format_t cf)
{
    LV_UNUSED(cf);
    /* Allocate from PSRAM, aligned to cache line so PPA DMA never rejects
     * the buffer.  Size is also rounded up so esp_cache_msync() is happy. */
    uint32_t aligned_size = lv_draw_ppa_align_size((uint32_t)size);
    return heap_caps_aligned_alloc(PPA_CACHE_LINE_SIZE, aligned_size,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static void ppa_buf_free(void * buf)
{
    heap_caps_free(buf);
}

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

void lv_draw_buf_ppa_init_handlers(void)
{
    lv_draw_buf_handlers_t * h = lv_draw_buf_get_handlers();
    h->buf_malloc_cb = ppa_buf_malloc;
    h->buf_free_cb   = ppa_buf_free;
}

void lv_draw_ppa_cache_sync(lv_draw_buf_t * buf)
{
    if(buf == NULL || buf->data == NULL || buf->data_size == 0) return;

    /* Only sync if buffer is in external (PSRAM) memory, which is cached. */
    if(!esp_ptr_external_ram(buf->data)) return;

    uint32_t aligned_size = lv_draw_ppa_align_size(buf->data_size);

    esp_cache_msync(buf->data, aligned_size,
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA |
                    ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

#endif /* CONFIG_SOC_PPA_SUPPORTED */
