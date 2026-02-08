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

static void lv_draw_img_ppa_core(lv_draw_task_t * t, const lv_draw_image_dsc_t * draw_dsc,
                                 const lv_image_decoder_dsc_t * decoder_dsc, lv_draw_image_sup_t * sup,
                                 const lv_area_t * img_coords, const lv_area_t * clipped_img_area);


void lv_draw_ppa_img(lv_draw_task_t * t, const lv_draw_image_dsc_t * dsc,
                     const lv_area_t * coords)
{
    if(dsc->opa <= (lv_opa_t)LV_OPA_MIN)
        return;
    lv_draw_image_normal_helper(t, dsc, coords, lv_draw_img_ppa_core);
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
    cfg.out.buffer_size      = draw_buf->data_size;
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

#endif /* CONFIG_SOC_PPA_SUPPORTED */
