/**
 * @file lv_draw_ppa_buf.c
 * PPA-aligned buffer allocator and cache sync for ESP32-P4
 */

#include "sdkconfig.h"
#ifdef CONFIG_SOC_PPA_SUPPORTED

#include "lv_draw_ppa_private.h"
#include "lv_draw_ppa.h"
#include "src/draw/lv_draw_buf.h"
#include "esp_memory_utils.h"

/**********************
 *  STATIC FUNCTIONS
 **********************/

static void * ppa_buf_malloc(size_t size, lv_color_format_t cf)
{
    LV_UNUSED(cf);
    uint32_t aligned_size = lv_draw_ppa_align_size((uint32_t)size);
    void * p = heap_caps_aligned_alloc((size_t)LV_DRAW_BUF_ALIGN, aligned_size,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if(p == NULL) {
        p = heap_caps_aligned_alloc((size_t)LV_DRAW_BUF_ALIGN, aligned_size,
                                    MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    }
    return p;
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

static inline uint32_t ppa_buf_compute_size(lv_draw_buf_t * buf)
{
    if(buf->data_size > 0) return buf->data_size;
    uint32_t bpp = lv_color_format_get_size((lv_color_format_t)buf->header.cf);
    return (uint32_t)buf->header.w * (uint32_t)buf->header.h * bpp;
}

void lv_draw_ppa_cache_sync(lv_draw_buf_t * buf)
{
    if(buf == NULL || buf->data == NULL) return;
    if(!esp_ptr_external_ram(buf->data)) return;

    uint32_t size = ppa_buf_compute_size(buf);
    if(size == 0) return;

    uint32_t aligned_size = lv_draw_ppa_align_size(size);

    esp_cache_msync(buf->data, aligned_size,
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA |
                    ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

#endif /* CONFIG_SOC_PPA_SUPPORTED */
