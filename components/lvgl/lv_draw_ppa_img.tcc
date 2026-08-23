/**
 * @file lv_draw_ppa_img.c
 * Fixed PPA image blending for LVGL 9.5 on ESP32-P4
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
#include "esp_memory_utils.h"  /* esp_ptr_external_ram(): skip cache sync on internal SRAM */
#include <math.h>

static void lv_draw_img_ppa_core(lv_draw_task_t * t, const lv_draw_image_dsc_t * draw_dsc,
                                 const lv_image_decoder_dsc_t * decoder_dsc, lv_draw_image_sup_t * sup,
                                 const lv_area_t * img_coords, const lv_area_t * clipped_img_area);

/* Which branch the last PPA image blend took. Written from the draw thread
 * (plain stores, no logging there) and reported by LvglComponent::loop() on
 * the main task. 0 means the PPA has not drawn an image yet. */
volatile uint8_t lv_ppa_img_last_mode    = LV_PPA_IMG_MODE_NONE;
volatile uint8_t lv_ppa_img_seen_modes   = 0;  /* bit per LV_PPA_IMG_MODE_* seen */
/* Runtime copy of LV_PPA_ALPHA_MIN_AREA so a test can flip the compositing
 * path on and off without rebuilding. */
#ifndef LV_PPA_ALPHA_MIN_AREA
#define LV_PPA_ALPHA_MIN_AREA 0
#endif
volatile uint32_t lv_ppa_alpha_min_area  = LV_PPA_ALPHA_MIN_AREA;
volatile uint8_t lv_ppa_img_last_src_cf  = 0;
volatile uint8_t lv_ppa_img_last_dest_cf = 0;
volatile uint8_t lv_ppa_img_last_opa     = 0;


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

    const uint32_t block_w = (uint32_t)lv_area_get_width(clipped_img_area);
    const uint32_t block_h = (uint32_t)lv_area_get_height(clipped_img_area);

    /* Does this draw need real compositing, or is a straight blit enough?
     * - a source alpha channel (ARGB8888) must be honoured against what is
     *   already in the destination;
     * - a global opacity (dsc->opa below LV_OPA_MAX) must fade it into them.
     * Everything else is an opaque copy and takes the cheaper path below. */
    /* LVGL treats opa >= LV_OPA_MAX (253) as fully covering, so only a value
     * below that is worth a compositing pass. */
    const lv_opa_t opa = draw_dsc->opa;
    const bool src_has_alpha = lv_ppa_cf_has_alpha(src_cf);
    const bool opa_is_partial = opa < (lv_opa_t)LV_OPA_MAX;
    const bool needs_compositing = src_has_alpha || opa_is_partial;

    /* Use field-by-field assignment for C++ compatibility
     * (C++ designated initializers must be in declaration order) */
    ppa_blend_oper_config_t cfg;
    lv_memzero(&cfg, sizeof(cfg));

    if(needs_compositing) {
        /* Real alpha compositing: BG is what is already on the destination and
         * FG is the image drawn over it. (The blit path below uses the opposite
         * assignment, which is why an ARGB8888 source used to lose its
         * transparency: its alpha was overwritten with 0xFF.) */
        cfg.in_bg.buffer         = (void *)dest_buf;
        cfg.in_bg.pic_w          = draw_buf->header.w;
        cfg.in_bg.pic_h          = draw_buf->header.h;
        cfg.in_bg.block_w        = block_w;
        cfg.in_bg.block_h        = block_h;
        cfg.in_bg.block_offset_x = (uint32_t)dest_area.x1;
        cfg.in_bg.block_offset_y = (uint32_t)dest_area.y1;
        cfg.in_bg.blend_cm       = lv_color_format_to_ppa_blend(dest_cf);

        cfg.bg_rgb_swap          = false;
        cfg.bg_byte_swap         = false;
        /* ppa_evaluate() only accepts RGB565 destinations for this path, and
         * RGB565 carries no alpha, so the backdrop is opaque by construction.
         * This would be wrong for an ARGB8888 layer, which LVGL clears to
         * transparent -- hence the restriction. */
        cfg.bg_alpha_update_mode = PPA_ALPHA_FIX_VALUE;
        cfg.bg_alpha_fix_val     = 0xFF;
        cfg.bg_ck_en             = false;

        cfg.in_fg.buffer         = (void *)src_buf;
        cfg.in_fg.pic_w          = draw_dsc->header.w;
        cfg.in_fg.pic_h          = draw_dsc->header.h;
        cfg.in_fg.block_w        = block_w;
        cfg.in_fg.block_h        = block_h;
        cfg.in_fg.block_offset_x = (uint32_t)src_area.x1;
        cfg.in_fg.block_offset_y = (uint32_t)src_area.y1;
        cfg.in_fg.blend_cm       = lv_color_format_to_ppa_blend(src_cf);

        cfg.fg_rgb_swap          = false;
        cfg.fg_byte_swap         = false;
        if(src_has_alpha && !opa_is_partial) {
            /* Use the image's own per-pixel alpha as-is. */
            cfg.fg_alpha_update_mode = PPA_ALPHA_NO_CHANGE;
        }
        else if(src_has_alpha) {
            /* Per-pixel alpha scaled by the global opacity. alpha_scale_ratio is
             * a float in the OPEN range (0, 1): opa is > LV_OPA_MIN (the caller
             * returns early at or below it) and < LV_OPA_MAX here, so the
             * quotient stays strictly inside it. */
            cfg.fg_alpha_update_mode  = PPA_ALPHA_SCALE;
            cfg.fg_alpha_scale_ratio  = (float)opa / 255.0f;
        }
        else {
            /* No alpha channel (or an undefined X byte): the global opacity is
             * the alpha for every pixel. */
            cfg.fg_alpha_update_mode = PPA_ALPHA_FIX_VALUE;
            cfg.fg_alpha_fix_val     = opa;
        }
        cfg.fg_ck_en             = false;
    }
    else {
        /* Opaque blit: the source is the background and the foreground is a
         * fully transparent dummy, so the output is just the converted source. */
        cfg.in_bg.buffer         = (void *)src_buf;
        cfg.in_bg.pic_w          = draw_dsc->header.w;
        cfg.in_bg.pic_h          = draw_dsc->header.h;
        cfg.in_bg.block_w        = block_w;
        cfg.in_bg.block_h        = block_h;
        cfg.in_bg.block_offset_x = (uint32_t)src_area.x1;
        cfg.in_bg.block_offset_y = (uint32_t)src_area.y1;
        cfg.in_bg.blend_cm       = lv_color_format_to_ppa_blend(src_cf);

        cfg.bg_rgb_swap          = false;
        cfg.bg_byte_swap         = false;
        cfg.bg_alpha_update_mode = PPA_ALPHA_FIX_VALUE;
        cfg.bg_alpha_fix_val     = 0xFF;
        cfg.bg_ck_en             = false;

        /* Dummy A8 foreground. It must describe a region that really exists in
         * the destination buffer: the PPA still fetches it even though
         * fg_alpha_fix_val = 0 makes it contribute nothing. */
        cfg.in_fg.buffer         = (void *)dest_buf;
        cfg.in_fg.pic_w          = draw_buf->header.w;
        cfg.in_fg.pic_h          = draw_buf->header.h;
        cfg.in_fg.block_w        = block_w;
        cfg.in_fg.block_h        = block_h;
        cfg.in_fg.block_offset_x = (uint32_t)dest_area.x1;
        cfg.in_fg.block_offset_y = (uint32_t)dest_area.y1;
        cfg.in_fg.blend_cm       = PPA_BLEND_COLOR_MODE_A8;

        cfg.fg_rgb_swap          = false;
        cfg.fg_byte_swap         = false;
        cfg.fg_alpha_update_mode = PPA_ALPHA_FIX_VALUE;
        cfg.fg_alpha_fix_val     = 0;
        cfg.fg_ck_en             = false;
    }

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
        return;
    }

    /* Diagnostic latch, read and reported by LvglComponent::loop().
     * Plain stores ONLY: this runs on the draw thread, and logging from here
     * goes through the LVGL log callback into the ESPHome logger, which is not
     * safe off the main loop (it faulted on the first render). */
    /* Accumulate every mode seen, do not just keep the last one: a screen can
     * draw several images per frame (different opacities side by side) and
     * loop() only reads this afterwards, so an overwriting latch would report
     * whichever image happened to be drawn last. */
    lv_ppa_img_seen_modes  |= (uint8_t)(1u << (needs_compositing
                                               ? (src_has_alpha ? (opa_is_partial ? LV_PPA_IMG_MODE_ALPHA_SCALE
                                                                                  : LV_PPA_IMG_MODE_ALPHA_NO_CHANGE)
                                                                : LV_PPA_IMG_MODE_ALPHA_FIX)
                                               : LV_PPA_IMG_MODE_BLIT));
    lv_ppa_img_last_mode    = needs_compositing
                              ? (src_has_alpha ? (opa_is_partial ? LV_PPA_IMG_MODE_ALPHA_SCALE
                                                                 : LV_PPA_IMG_MODE_ALPHA_NO_CHANGE)
                                               : LV_PPA_IMG_MODE_ALPHA_FIX)
                              : LV_PPA_IMG_MODE_BLIT;
    lv_ppa_img_last_src_cf  = (uint8_t)src_cf;
    lv_ppa_img_last_dest_cf = (uint8_t)dest_cf;
    lv_ppa_img_last_opa     = (uint8_t)opa;
}

#ifdef LV_USE_PPA_IMG

void lv_draw_ppa_img_srm(lv_draw_task_t * t, const lv_draw_image_dsc_t * dsc,
                          const lv_area_t * coords)
{
    if(dsc->opa <= (lv_opa_t)LV_OPA_MIN) return;

    lv_draw_ppa_unit_t * u   = (lv_draw_ppa_unit_t *)t->draw_unit;
    lv_layer_t * layer        = t->target_layer;
    lv_draw_buf_t * dest_buf  = layer->draw_buf;

    /* coords (== t->area) is the image rect at 1:1 scale. The area actually
     * covered on screen by the SCALED image is t->_real_area (the transformed
     * bounding box: round(src*scale), positioned so the pivot stays fixed).
     * Clip THAT to the render tile -- clipping `coords` instead placed a
     * down-scaled image at the un-scaled rect origin (shifted, wrong size). */
    LV_UNUSED(coords);
    lv_area_t visible_area;
    if(!lv_area_intersect(&visible_area, &t->_real_area, &t->clip_area) ||
       !lv_area_intersect(&visible_area, &visible_area, &layer->buf_area)) return;

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

    /* Virtual image origin = top-left of the scaled image on screen. This is
     * exactly t->_real_area's top-left: for pure scale the box's min corner is
     * source (0,0) mapped through pivot*(1-scale), i.e. coords.x1 +
     * pivot.x*(1-sx). Use _real_area directly so placement matches the region
     * we clipped against above. */
    float virt_x = (float)t->_real_area.x1;
    float virt_y = (float)t->_real_area.y1;

    /* Visible clip dimensions and buffer-local destination (always non-negative) */
    int32_t clip_w = lv_area_get_width(&visible_area);
    int32_t clip_h = lv_area_get_height(&visible_area);

    lv_area_t dest_area;
    lv_area_copy(&dest_area, &visible_area);
    lv_area_move(&dest_area, -layer->buf_area.x1, -layer->buf_area.y1);

    /* Map visible tile top-left back into source image space */
    int32_t src_bx = (int32_t)(((float)visible_area.x1 - virt_x) / sx);
    int32_t src_by = (int32_t)(((float)visible_area.y1 - virt_y) / sy);

    /* ceilf gives the ideal source block; floorf clamp keeps PPA happy.
     * The PPA may render 1 pixel short — we fix that after the call. */
    uint32_t src_bw = (uint32_t)ceilf((float)clip_w / sx);
    uint32_t src_bh = (uint32_t)ceilf((float)clip_h / sy);

    uint32_t avail_w = (uint32_t)(dest_buf->header.w - dest_area.x1);
    uint32_t avail_h = (uint32_t)(dest_buf->header.h - dest_area.y1);
    uint32_t max_src_bw = (uint32_t)floorf((float)avail_w / sx);
    uint32_t max_src_bh = (uint32_t)floorf((float)avail_h / sy);
    bool gap_right  = (src_bw > max_src_bw);
    bool gap_bottom = (src_bh > max_src_bh);
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

    if(decoded->data_size > 0 && esp_ptr_external_ram((void *)decoded->data)) {
        esp_cache_msync((void *)decoded->data,
                        lv_draw_ppa_align_size(decoded->data_size),
                        ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    }

    uint32_t out_bpp = (dest_cf == LV_COLOR_FORMAT_RGB565) ? 2u :
                       (dest_cf == LV_COLOR_FORMAT_RGB888)  ? 3u : 4u;
    uint32_t raw_bytes    = (uint32_t)dest_buf->header.w * dest_buf->header.h * out_bpp;
    uint32_t aligned_size = lv_draw_ppa_align_size(raw_bytes);

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
                     (int)ret, (unsigned)src_w, (unsigned)src_h, (double)sx, (double)sy);
    }

    /* PPA floorf rounding leaves a 1-pixel gap at right/bottom edges.
     * Fill it by duplicating the last rendered column/row.
     * Must invalidate CPU cache first: PPA wrote via DMA, CPU cache is stale. */
    if(ret == ESP_OK && (gap_right || gap_bottom)) {
        /* M2C (invalidate) must be cache-line aligned: recent ESP-IDF rejects
         * ESP_CACHE_MSYNC_FLAG_UNALIGNED for M2C. dest_buf->data and
         * aligned_size are already cache-aligned, so drop the UNALIGNED flag.
         * PSRAM only: internal SRAM is DMA-coherent and rejects esp_cache_msync. */
        if(esp_ptr_external_ram(dest_buf->data))
            esp_cache_msync(dest_buf->data, aligned_size, ESP_CACHE_MSYNC_FLAG_DIR_M2C);

        uint8_t *base = dest_buf->data;
        uint32_t stride = dest_buf->header.w * out_bpp;

        if(gap_right && clip_w >= 2) {
            uint32_t col = dest_area.x1 + (uint32_t)clip_w - 1;
            uint32_t col_prev = col - 1;
            for(int32_t y = 0; y < clip_h; y++) {
                uint32_t row_off = (dest_area.y1 + (uint32_t)y) * stride;
                lv_memcpy(base + row_off + col * out_bpp,
                          base + row_off + col_prev * out_bpp, out_bpp);
            }
        }
        if(gap_bottom && clip_h >= 2) {
            uint32_t row = dest_area.y1 + (uint32_t)clip_h - 1;
            uint32_t row_prev = row - 1;
            lv_memcpy(base + row * stride + dest_area.x1 * out_bpp,
                      base + row_prev * stride + dest_area.x1 * out_bpp,
                      (uint32_t)clip_w * out_bpp);
        }

        if(esp_ptr_external_ram(dest_buf->data))
            esp_cache_msync(dest_buf->data, aligned_size,
                            ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
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

    /* Tile/clip-aware geometry: map the visible render tile back onto a PPA
     * source SUB-block and clamp the destination to the layer buffer. The old
     * code rotated the FULL image and placed it at the on-screen offset, so the
     * rotated block overran the destination picture and the PPA rejected it
     * ("scale does not fit in the out pic").
     *
     * IMPORTANT: `coords` (== t->area) is the ORIGINAL, un-rotated image rect.
     * The on-screen area actually covered by the rotated image is t->_real_area
     * (the transformed bounding box). For 90/270 its dims are swapped
     * (src_h x src_w), which is what maps 1:1 to the PPA output block. Using
     * `coords` here made 90/270 collapse to the wrong (square/empty) region. */
    LV_UNUSED(coords);
    int32_t buf_w = (int32_t)dest_buf->header.w;
    int32_t buf_h = (int32_t)dest_buf->header.h;

    const lv_area_t * real = &t->_real_area;

    lv_area_t vis;
    if(!lv_area_intersect(&vis, real, &t->clip_area) ||
       !lv_area_intersect(&vis, &vis, &layer->buf_area)) {
        lv_image_decoder_close(&decoder_dsc);
        return;
    }
    int32_t out_dx = vis.x1 - real->x1;
    int32_t out_dy = vis.y1 - real->y1;
    int32_t vw = lv_area_get_width(&vis);
    int32_t vh = lv_area_get_height(&vis);

    lv_area_t dest_area;
    lv_area_copy(&dest_area, &vis);
    lv_area_move(&dest_area, -layer->buf_area.x1, -layer->buf_area.y1);
    if(dest_area.x1 < 0 || dest_area.y1 < 0 || dest_area.x1 >= buf_w || dest_area.y1 >= buf_h) {
        lv_image_decoder_close(&decoder_dsc);
        return;
    }
    if(dest_area.x1 + vw > buf_w) vw = buf_w - dest_area.x1;
    if(dest_area.y1 + vh > buf_h) vh = buf_h - dest_area.y1;
    if(vw <= 0 || vh <= 0) {
        lv_image_decoder_close(&decoder_dsc);
        return;
    }

    /* Invert the rotation to find the source sub-block. 90/270 swap the source
     * axes (block dims become vh x vw); 180 keeps them (vw x vh). */
    int32_t sx, sy, sw, sh;
    switch(angle) {
        case 1800:
            sw = vw; sh = vh;
            sx = (int32_t)src_w - out_dx - vw;
            sy = (int32_t)src_h - out_dy - vh;
            break;
        case 900:
            sw = vh; sh = vw;
            sx = out_dy;
            sy = (int32_t)src_h - out_dx - vw;
            break;
        case 2700:
            sw = vh; sh = vw;
            sx = (int32_t)src_w - out_dy - vh;
            sy = out_dx;
            break;
        default:
            lv_image_decoder_close(&decoder_dsc);
            return;
    }
    if(sx < 0 || sy < 0 || sx >= (int32_t)src_w || sy >= (int32_t)src_h) {
        lv_image_decoder_close(&decoder_dsc);
        return;
    }
    if(sx + sw > (int32_t)src_w) sw = (int32_t)src_w - sx;
    if(sy + sh > (int32_t)src_h) sh = (int32_t)src_h - sy;
    if(sw <= 0 || sh <= 0) {
        lv_image_decoder_close(&decoder_dsc);
        return;
    }

    /* Flush decoded source buffer for PPA DMA access. Align size to cache
     * line; _UNALIGNED flag is only a safety net for the address. */
    if(decoded->data_size > 0 && esp_ptr_external_ram((void *)decoded->data)) {
        esp_cache_msync((void *)decoded->data,
                        lv_draw_ppa_align_size(decoded->data_size),
                        ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    }

    /* Configure PPA SRM operation */
    ppa_srm_oper_config_t cfg;
    lv_memzero(&cfg, sizeof(cfg));

    /* Input: the source sub-block that maps onto the visible tile. */
    cfg.in.buffer         = (void *)decoded->data;
    cfg.in.pic_w          = src_w;
    cfg.in.pic_h          = src_h;
    cfg.in.block_w        = (uint32_t)sw;
    cfg.in.block_h        = (uint32_t)sh;
    cfg.in.block_offset_x = (uint32_t)sx;
    cfg.in.block_offset_y = (uint32_t)sy;
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
    /* Pure rotation only: the tile-aware source-block geometry above assumes a
     * 1:1 pixel mapping (source sub-block dims == rotated dest tile dims). A
     * simultaneous scale would invalidate that mapping, so force 1.0 here. The
     * dispatch routes any rotation!=0 through this path regardless of scale, and
     * the test config never combines the two. */
    cfg.scale_x            = 1.0f;
    cfg.scale_y            = 1.0f;
    cfg.mirror_x           = false;
    cfg.mirror_y           = false;
    cfg.rgb_swap           = false;
    cfg.byte_swap          = false;
    cfg.alpha_update_mode  = PPA_ALPHA_NO_CHANGE;
    cfg.mode               = PPA_TRANS_MODE_BLOCKING;
    cfg.user_data          = u;

    esp_err_t ret = ppa_do_scale_rotate_mirror(u->srm_client, &cfg);
    if(ret != ESP_OK) {
        LV_LOG_ERROR("PPA SRM rotate failed: %d angle=%d src=%ux%u "
                     "in[off %d,%d blk %dx%d] out[off %d,%d pic %ux%u]",
                     (int)ret, (int)angle, (unsigned)src_w, (unsigned)src_h,
                     (int)sx, (int)sy, (int)sw, (int)sh,
                     (int)dest_area.x1, (int)dest_area.y1,
                     (unsigned)dest_buf->header.w, (unsigned)dest_buf->header.h);
    }

    lv_image_decoder_close(&decoder_dsc);
}

#endif /* LV_USE_PPA_IMG */

#endif /* CONFIG_SOC_PPA_SUPPORTED */
