/**
 * @file lv_draw_ppa.h
 * Custom PPA draw unit header for ESP32-P4
 * Based on https://github.com/lvgl/lvgl/pull/9162 (included in LVGL 9.5+)
 */

#ifndef LV_DRAW_PPA_FIXED_H
#define LV_DRAW_PPA_FIXED_H

#ifdef __cplusplus
extern "C" {
#endif

/*********************
 *      INCLUDES
 *********************/
#include "lvgl.h"
#include "src/draw/lv_draw_private.h"
#include "src/display/lv_display_private.h"
#include "src/misc/lv_area_private.h"

/*********************
 *      DEFINES
 *********************/

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 * GLOBAL PROTOTYPES
 **********************/

/* Which branch the last PPA image blend took. The draw thread only stores
 * into these (logging from that task goes through the ESPHome logger and is
 * not safe there); LvglComponent::loop() reports the value when it changes. */
#define LV_PPA_IMG_MODE_NONE             0
#define LV_PPA_IMG_MODE_BLIT             1  /* opaque copy, no compositing */
#define LV_PPA_IMG_MODE_ALPHA_NO_CHANGE  2  /* per-pixel alpha, opa >= LV_OPA_MAX */
#define LV_PPA_IMG_MODE_ALPHA_SCALE      3  /* per-pixel alpha x global opacity */
#define LV_PPA_IMG_MODE_ALPHA_FIX        4  /* global opacity only (no alpha channel) */

extern volatile uint8_t lv_ppa_img_last_mode;
extern volatile uint8_t lv_ppa_img_seen_modes;
extern volatile uint8_t lv_ppa_img_last_src_cf;
extern volatile uint8_t lv_ppa_img_last_dest_cf;
extern volatile uint8_t lv_ppa_img_last_opa;

void lv_draw_ppa_init(void);
void lv_draw_ppa_deinit(void);
void lv_draw_buf_ppa_init_handlers(void);

void lv_draw_ppa_fill(lv_draw_task_t * t, const lv_draw_fill_dsc_t * dsc,
                      const lv_area_t * coords);

void lv_draw_ppa_img(lv_draw_task_t * t, const lv_draw_image_dsc_t * dsc,
                     const lv_area_t * coords);

#ifdef LV_USE_PPA_IMG
void lv_draw_ppa_img_rotate(lv_draw_task_t * t, const lv_draw_image_dsc_t * dsc,
                            const lv_area_t * coords);
void lv_draw_ppa_img_srm(lv_draw_task_t * t, const lv_draw_image_dsc_t * dsc,
                         const lv_area_t * coords);
#endif

void lv_draw_ppa_cache_sync(lv_draw_buf_t * buf);

/**********************
 *      MACROS
 **********************/

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /* LV_DRAW_PPA_FIXED_H */
