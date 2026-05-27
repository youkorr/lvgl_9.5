/**
 * @file lv_draw_ppa_img.c
 * Fixed PPA image blending for LVGL 9.4 on ESP32-P4
 * Backported from https://github.com/lvgl/lvgl/pull/9162
 * Adapted for C++ compilation (ESPHome build system)
 */

#include "sdkconfig.h"
#ifdef CONFIG_SOC_PPA_SUPPORTED

#include "lv_draw_ppa_private.h"
#include "lv_draw_ppa.h"
#include "src/draw/lv_draw_image_private.h"
#include "src/draw/lv_image_decoder_private.h"
#include "src/draw/lv_image_decoder.h"
#include <math.h>

static void lv_draw_img_ppa_core(lv_draw_task_t * t, const lv_draw_image_dsc_t * draw_dsc,
                                 const lv_image_decoder_dsc_t * decoder_dsc, lv_draw_image_sup_t * sup,
                                 const lv_area_t * img_coords, const lv_area_t * clipped_img_area);


void lv_draw_ppa_img(lv_draw_task_t * t, const lv_draw_image_dsc_t * dsc,
                     const lv_area_t * coords)
{
    if(dsc->opa <= (lv_opa_t)LV_OPA_MIN)
        return;
    lv_draw_image_normal_helper(t, dsc, coords, lv_draw_img_ppa_core, NULL);
}

static void lv_draw_img_ppa_core(lv_draw_task_t * t, const lv_draw_image_dsc_t * draw_dsc,
                                 const lv_image_decoder_dsc_t * decoder_dsc, lv_draw_image_sup_t * sup,
                                 const lv_area_t * img_coords, const lv_area_t * clipped_img_area)
{
    LV_UNUSED(sup);

    lv_layer_t * layer = t->target_layer;
    lv_draw_buf_t * draw_buf = layer->draw_buf;
    const lv_draw_buf_t * decoded = decoder_dsc->decoded;
    lv_draw_ppa_unit_t * u = (lv_draw_ppa_unit_t *)t->draw_unit;

    lv_area_t rel_clip_area;
    lv_area_copy(&rel_clip_area, clipped_img_area);
    lv_area_move(&rel_clip_area, -img_coords->x1, -img_coords->y1);

    lv_area_t rel_img_coords;
    lv_area_copy(&rel_img_coords, img_coords);
    lv_area_move(&rel_img_coords, -img_coords->x1, -img_coords->y1);

    lv_area_t src_area;
    if(!lv_area_intersect(&src_area, &rel_clip_area, &rel_img_coords))
        return;

    lv_area_t dest_area;
    lv_area_copy(&dest_area, clipped_img_area);
    lv_area_move(&dest_area, -t->target_layer->buf_area.x1, -t->target_layer->buf_area.y1);

    const uint8_t * src_buf = decoded->data;
    lv_color_format_t src_cf = (lv_color_format_t)draw_dsc->header.cf;
    lv_color_format_t dest_cf = (lv_color_format_t)draw_buf->header.cf;
    uint8_t * dest_buf = draw_buf->data;

    /* Use field-by-field assignment for C++ compatibility
     * (C++ designated initializers must be in declaration order) */
    ppa_blend_oper_config_t cfg;
    lv_memzero(&cfg, sizeof(cfg));

    /* Background input (source image) */
    cfg.in_bg.buffer         = (void *)src_buf;
    cfg.in_bg.pic_w          = draw_dsc->header.w;
    cfg.in_bg.pic_h          = draw_dsc->header.h;
    cfg.in_bg.block_w        = (uint32_t)lv_area_get_width(clipped_img_area);
    cfg.in_bg.block_h        = (uint32_t)lv_area_get_height(clipped_img_area);
    cfg.in_bg.block_offset_x = (uint32_t)src_area.x1;
    cfg.in_bg.block_offset_y = (uint32_t)src_area.y1;
    cfg.in_bg.blend_cm       = lv_color_format_to_ppa_blend(src_cf);

    cfg.bg_rgb_swap          = false;
    cfg.bg_byte_swap         = false;
    cfg.bg_alpha_update_mode = PPA_ALPHA_FIX_VALUE;
    cfg.bg_alpha_fix_val     = 0xFF;
    cfg.bg_ck_en             = false;

    /* Foreground input */
    cfg.in_fg.buffer         = (void *)dest_buf;
    cfg.in_fg.pic_w          = draw_dsc->header.w;
    cfg.in_fg.pic_h          = draw_dsc->header.h;
    cfg.in_fg.block_w        = (uint32_t)lv_area_get_width(clipped_img_area);
    cfg.in_fg.block_h        = (uint32_t)lv_area_get_height(clipped_img_area);
    cfg.in_fg.block_offset_x = (uint32_t)src_area.x1;
    cfg.in_fg.block_offset_y = (uint32_t)src_area.y1;
    cfg.in_fg.blend_cm       = PPA_BLEND_COLOR_MODE_A8;

    cfg.fg_rgb_swap          = false;
    cfg.fg_byte_swap         = false;
    cfg.fg_alpha_update_mode = PPA_ALPHA_FIX_VALUE;
    cfg.fg_alpha_fix_val     = 0;
    cfg.fg_ck_en             = false;

    /* Output */
    cfg.out.buffer           = dest_buf;
    /* PPA hardware rejects unaligned out.buffer_size (issue #9868). */
    cfg.out.buffer_size      = lv_draw_ppa_align_size(draw_buf->data_size);
    cfg.out.pic_w            = draw_buf->header.w;
    cfg.out.pic_h            = draw_buf->header.h;
    cfg.out.block_offset_x   = (uint32_t)dest_area.x1;
    cfg.out.block_offset_y   = (uint32_t)dest_area.y1;
    cfg.out.blend_cm         = lv_color_format_to_ppa_blend(dest_cf);

    cfg.mode                 = PPA_TRANS_MODE_BLOCKING;
    cfg.user_data            = u;

    esp_err_t ret = ppa_do_blend(u->blend_client, &cfg);
    if(ret != ESP_OK) {
        LV_LOG_ERROR("PPA blend failed: %d", ret);
    }
}

#ifdef LV_USE_PPA_IMG

void lv_draw_ppa_img_srm(lv_draw_task_t * t, const lv_draw_image_dsc_t * dsc,
                          const lv_area_t * coords)
{
    if(dsc->opa <= (lv_opa_t)LV_OPA_MIN) return;

    lv_draw_ppa_unit_t * u   = (lv_draw_ppa_unit_t *)t->draw_unit;
    lv_layer_t * layer        = t->target_layer;
    lv_draw_buf_t * dest_buf  = layer->draw_buf;

    /* coords = image rect at 1:1 scale (may extend off-screen).
     * Intersect with the render tile to get the actual visible clip. */
    lv_area_t visible_area;
    if(!lv_area_intersect(&visible_area, coords, &layer->buf_area)) return;

    lv_image_decoder_dsc_t decoder_dsc;
    lv_image_decoder_args_t dec_args;
    lv_memzero(&dec_args, sizeof(dec_args));
    dec_args.flush_cache = true;

    lv_result_t res = lv_image_decoder_open(&decoder_dsc, dsc->src, &dec_args);
    if(res != LV_RESULT_OK) return;

    const lv_draw_buf_t * decoded = decoder_dsc.decoded;
    if(!decoded || !decoded->data) {
        lv_image_decoder_close(&decoder_dsc);
        return;
    }

    lv_color_format_t src_cf  = (lv_color_format_t)decoded->header.cf;
    lv_color_format_t dest_cf = (lv_color_format_t)dest_buf->header.cf;
    if(!ppa_src_cf_supported(src_cf) || !ppa_dest_cf_supported(dest_cf)) {
        lv_image_decoder_close(&decoder_dsc);
        return;
    }

    float sx = (dsc->scale_x != LV_SCALE_NONE) ? ((float)dsc->scale_x / 256.0f) : 1.0f;
    float sy = (dsc->scale_y != LV_SCALE_NONE) ? ((float)dsc->scale_y / 256.0f) : 1.0f;

    uint32_t src_w = decoded->header.w;
    uint32_t src_h = decoded->header.h;

    /* Virtual image origin: pivot stays fixed on screen as scale changes.
     * coords->x1/y1 = image top-left at 1:1 scale. */
    float virt_x = (float)coords->x1 + (float)dsc->pivot.x * (1.0f - sx);
    float virt_y = (float)coords->y1 + (float)dsc->pivot.y * (1.0f - sy);

    /* --- PPA SRM DEBUG (log once per second) --- */
    {
        static uint32_t srm_dbg_count = 0;
        if(srm_dbg_count % 100 == 0) {
            LV_LOG_USER("PPA_SRM coords=[%d,%d→%d,%d] pivot=(%d,%d) scale=(%d,%d) "
                        "virt=(%.1f,%.1f) src=%ux%u buf_area=[%d,%d→%d,%d]",
                        (int)coords->x1, (int)coords->y1,
                        (int)coords->x2, (int)coords->y2,
                        (int)dsc->pivot.x, (int)dsc->pivot.y,
                        (int)dsc->scale_x, (int)dsc->scale_y,
                        (double)virt_x, (double)virt_y,
                        src_w, src_h,
                        (int)layer->buf_area.x1, (int)layer->buf_area.y1,
                        (int)layer->buf_area.x2, (int)layer->buf_area.y2);
        }
        srm_dbg_count++;
    }

    /* Visible clip dimensions and buffer-local destination (always non-negative) */
    int32_t clip_w = lv_area_get_width(&visible_area);
    int32_t clip_h = lv_area_get_height(&visible_area);

    lv_area_t dest_area;
    lv_area_copy(&dest_area, &visible_area);
    lv_area_move(&dest_area, -layer->buf_area.x1, -layer->buf_area.y1);

    /* Map visible tile top-left back into source image space */
    int32_t src_bx = (int32_t)(((float)visible_area.x1 - virt_x) / sx);
    int32_t src_by = (int32_t)(((float)visible_area.y1 - virt_y) / sy);

    /* ceilf covers the full tile; floorf clamp prevents PPA check-1 overflow */
    uint32_t src_bw = (uint32_t)ceilf((float)clip_w / sx);
    uint32_t src_bh = (uint32_t)ceilf((float)clip_h / sy);

    uint32_t avail_w = (uint32_t)(dest_buf->header.w - dest_area.x1);
    uint32_t avail_h = (uint32_t)(dest_buf->header.h - dest_area.y1);
    uint32_t max_src_bw = (uint32_t)floorf((float)avail_w / sx);
    uint32_t max_src_bh = (uint32_t)floorf((float)avail_h / sy);
    if(src_bw > max_src_bw) src_bw = max_src_bw;
    if(src_bh > max_src_bh) src_bh = max_src_bh;

    if(src_bx < 0 || src_by < 0 ||
       (uint32_t)src_bx >= src_w || (uint32_t)src_by >= src_h) {
        lv_image_decoder_close(&decoder_dsc);
        return;
    }
    if((uint32_t)src_bx + src_bw > src_w) src_bw = src_w - (uint32_t)src_bx;
    if((uint32_t)src_by + src_bh > src_h) src_bh = src_h - (uint32_t)src_by;
    if(src_bw == 0 || src_bh == 0) {
        lv_image_decoder_close(&decoder_dsc);
        return;
    }

    /* --- PPA SRM DEBUG: source/dest blocks --- */
    {
        static uint32_t srm_blk_count = 0;
        if(srm_blk_count % 100 == 0) {
            LV_LOG_USER("PPA_SRM src_block=(%d,%d %ux%u) dest_block=(%d,%d) "
                        "clip=%dx%d visible=[%d,%d→%d,%d] dest_buf=%dx%d",
                        (int)src_bx, (int)src_by, src_bw, src_bh,
                        (int)dest_area.x1, (int)dest_area.y1,
                        clip_w, clip_h,
                        (int)visible_area.x1, (int)visible_area.y1,
                        (int)visible_area.x2, (int)visible_area.y2,
                        (int)dest_buf->header.w, (int)dest_buf->header.h);
        }
        srm_blk_count++;
    }

    if(decoded->data_size > 0) {
        esp_cache_msync((void *)decoded->data,
                        lv_draw_ppa_align_size(decoded->data_size),
                        ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    }

    ppa_srm_oper_config_t cfg;
    lv_memzero(&cfg, sizeof(cfg));

    cfg.in.buffer         = (void *)decoded->data;
    cfg.in.pic_w          = src_w;
    cfg.in.pic_h          = src_h;
    cfg.in.block_w        = src_bw;
    cfg.in.block_h        = src_bh;
    cfg.in.block_offset_x = (uint32_t)src_bx;
    cfg.in.block_offset_y = (uint32_t)src_by;
    cfg.in.srm_cm         = lv_color_format_to_ppa_srm(src_cf);

    uint32_t out_bpp = (dest_cf == LV_COLOR_FORMAT_RGB565) ? 2u :
                       (dest_cf == LV_COLOR_FORMAT_RGB888)  ? 3u : 4u;
    uint32_t raw_bytes    = (uint32_t)dest_buf->header.w * dest_buf->header.h * out_bpp;
    uint32_t aligned_size = lv_draw_ppa_align_size(raw_bytes);

    /* Draw buffers are now always cache-aligned (lv_draw_buf_ppa_init_handlers).
     * Pass dest_buf->data directly — no temp buffer needed. */
    cfg.out.buffer         = dest_buf->data;
    cfg.out.buffer_size    = aligned_size;
    cfg.out.pic_w          = dest_buf->header.w;
    cfg.out.pic_h          = dest_buf->header.h;
    cfg.out.block_offset_x = (uint32_t)dest_area.x1;
    cfg.out.block_offset_y = (uint32_t)dest_area.y1;
    cfg.out.srm_cm         = lv_color_format_to_ppa_srm(dest_cf);

    cfg.rotation_angle    = PPA_SRM_ROTATION_ANGLE_0;
    cfg.scale_x           = sx;
    cfg.scale_y           = sy;
    cfg.mirror_x          = false;
    cfg.mirror_y          = false;
    cfg.rgb_swap          = false;
    cfg.byte_swap         = false;
    cfg.alpha_update_mode = PPA_ALPHA_NO_CHANGE;
    cfg.mode              = PPA_TRANS_MODE_BLOCKING;
    cfg.user_data         = u;

    esp_err_t ret = ppa_do_scale_rotate_mirror(u->srm_client, &cfg);
    if(ret != ESP_OK) {
        LV_LOG_ERROR("PPA SRM scale failed: %d (src %ux%u scale %.2f/%.2f)",
                     (int)ret, src_w, src_h, (double)sx, (double)sy);
    }

    lv_image_decoder_close(&decoder_dsc);
}

/**
 * PPA SRM hardware-accelerated image rotation (0/90/180/270 degrees)
 * Uses the ESP32-P4 PPA Scale-Rotate-Mirror engine for zero-CPU-cost rotation.
 */
void lv_draw_ppa_img_rotate(lv_draw_task_t * t, const lv_draw_image_dsc_t * dsc,
                            const lv_area_t * coords)
{
    if(dsc->opa <= (lv_opa_t)LV_OPA_MIN)
        return;

    lv_draw_ppa_unit_t * u = (lv_draw_ppa_unit_t *)t->draw_unit;
    lv_layer_t * layer = t->target_layer;
    lv_draw_buf_t * dest_buf = layer->draw_buf;

    /* Decode the source image */
    lv_image_decoder_dsc_t decoder_dsc;
    lv_image_decoder_args_t dec_args;
    lv_memzero(&dec_args, sizeof(dec_args));
    dec_args.stride_align = false;
    dec_args.premultiply = false;
    dec_args.no_cache = false;
    dec_args.use_indexed = false;
    dec_args.flush_cache = true;  /* Ensure cache coherency for PPA DMA */

    lv_result_t res = lv_image_decoder_open(&decoder_dsc, dsc->src, &dec_args);
    if(res != LV_RESULT_OK) {
        LV_LOG_WARN("PPA SRM: failed to decode image");
        return;
    }

    const lv_draw_buf_t * decoded = decoder_dsc.decoded;
    if(!decoded || !decoded->data) {
        lv_image_decoder_close(&decoder_dsc);
        return;
    }

    lv_color_format_t src_cf = (lv_color_format_t)decoded->header.cf;
    lv_color_format_t dest_cf = (lv_color_format_t)dest_buf->header.cf;

    /* Verify PPA format support for both source and destination */
    if(!ppa_src_cf_supported(src_cf) || !ppa_dest_cf_supported(dest_cf)) {
        LV_LOG_WARN("PPA SRM: unsupported color format src=%d dest=%d", src_cf, dest_cf);
        lv_image_decoder_close(&decoder_dsc);
        return;
    }

    /* Map LVGL rotation (clockwise, 0.1 deg units) to PPA rotation (counter-clockwise) */
    int32_t angle = dsc->rotation % 3600;
    if(angle < 0) angle += 3600;

    ppa_srm_rotation_angle_t ppa_rot;
    switch(angle) {
        case 0:    ppa_rot = PPA_SRM_ROTATION_ANGLE_0;   break;
        case 900:  ppa_rot = PPA_SRM_ROTATION_ANGLE_270; break;  /* 90° CW = 270° CCW */
        case 1800: ppa_rot = PPA_SRM_ROTATION_ANGLE_180; break;
        case 2700: ppa_rot = PPA_SRM_ROTATION_ANGLE_90;  break;  /* 270° CW = 90° CCW */
        default:
            lv_image_decoder_close(&decoder_dsc);
            return;
    }

    uint32_t src_w = decoded->header.w;
    uint32_t src_h = decoded->header.h;

    /* Compute destination area relative to layer buffer origin */
    lv_area_t dest_area;
    lv_area_copy(&dest_area, &t->area);
    lv_area_move(&dest_area, -layer->buf_area.x1, -layer->buf_area.y1);

    /* Flush decoded source buffer for PPA DMA access. Align size to cache
     * line; _UNALIGNED flag is only a safety net for the address. */
    if(decoded->data_size > 0) {
        esp_cache_msync((void *)decoded->data,
                        lv_draw_ppa_align_size(decoded->data_size),
                        ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    }

    /* Configure PPA SRM operation */
    ppa_srm_oper_config_t cfg;
    lv_memzero(&cfg, sizeof(cfg));

    /* Input: full source image block */
    cfg.in.buffer         = (void *)decoded->data;
    cfg.in.pic_w          = src_w;
    cfg.in.pic_h          = src_h;
    cfg.in.block_w        = src_w;
    cfg.in.block_h        = src_h;
    cfg.in.block_offset_x = 0;
    cfg.in.block_offset_y = 0;
    cfg.in.srm_cm         = lv_color_format_to_ppa_srm(src_cf);

    uint32_t out_bpp_r    = (dest_cf == LV_COLOR_FORMAT_RGB565) ? 2u :
                            (dest_cf == LV_COLOR_FORMAT_RGB888)  ? 3u : 4u;
    uint32_t aligned_size_r = lv_draw_ppa_align_size(
                                  (uint32_t)dest_buf->header.w * dest_buf->header.h * out_bpp_r);

    /* Draw buffers are cache-aligned (lv_draw_buf_ppa_init_handlers). */
    cfg.out.buffer         = dest_buf->data;
    cfg.out.buffer_size    = aligned_size_r;
    cfg.out.pic_w          = dest_buf->header.w;
    cfg.out.pic_h          = dest_buf->header.h;
    cfg.out.block_offset_x = (uint32_t)dest_area.x1;
    cfg.out.block_offset_y = (uint32_t)dest_area.y1;
    cfg.out.srm_cm         = lv_color_format_to_ppa_srm(dest_cf);

    cfg.rotation_angle     = ppa_rot;
    cfg.scale_x            = (dsc->scale_x != LV_SCALE_NONE) ? ((float)dsc->scale_x / 256.0f) : 1.0f;
    cfg.scale_y            = (dsc->scale_y != LV_SCALE_NONE) ? ((float)dsc->scale_y / 256.0f) : 1.0f;
    cfg.mirror_x           = false;
    cfg.mirror_y           = false;
    cfg.rgb_swap           = false;
    cfg.byte_swap          = false;
    cfg.alpha_update_mode  = PPA_ALPHA_NO_CHANGE;
    cfg.mode               = PPA_TRANS_MODE_BLOCKING;
    cfg.user_data          = u;

    esp_err_t ret = ppa_do_scale_rotate_mirror(u->srm_client, &cfg);
    if(ret != ESP_OK) {
        LV_LOG_ERROR("PPA SRM rotation failed: %d  (src %ux%u, angle %d)", (int)ret, src_w, src_h, angle);
    }

    lv_image_decoder_close(&decoder_dsc);
}

#endif /* LV_USE_PPA_IMG */

#endif /* CONFIG_SOC_PPA_SUPPORTED */
