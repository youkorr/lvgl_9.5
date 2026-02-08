/**
 * @file lv_draw_ppa.c
 * Fixed PPA draw unit for LVGL 9.4 on ESP32-P4
 * Backported from https://github.com/lvgl/lvgl/pull/9162
 * Adapted for C++ compilation (ESPHome build system)
 */

#include "sdkconfig.h"
#ifdef CONFIG_SOC_PPA_SUPPORTED

#include "lv_draw_ppa_private.h"
#include "lv_draw_ppa.h"

/*********************
 *      DEFINES
 *********************/
#define DRAW_UNIT_ID_PPA 120

/**********************
 *  STATIC PROTOTYPES
 **********************/
static int32_t ppa_evaluate(lv_draw_unit_t * draw_unit, lv_draw_task_t * task);
static int32_t ppa_dispatch(lv_draw_unit_t * draw_unit, lv_layer_t * layer);
static int32_t ppa_delete(lv_draw_unit_t * draw_unit);
static void  ppa_execute_drawing(lv_draw_ppa_unit_t * u);

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

void lv_draw_ppa_init(void)
{
    esp_err_t res;
    ppa_client_config_t cfg;
    lv_memzero(&cfg, sizeof(cfg));

    /* Create draw unit */
    lv_draw_buf_ppa_init_handlers();

    lv_draw_ppa_unit_t * draw_ppa_unit = (lv_draw_ppa_unit_t *)lv_draw_create_unit(sizeof(lv_draw_ppa_unit_t));
    draw_ppa_unit->base_unit.evaluate_cb = ppa_evaluate;
    draw_ppa_unit->base_unit.dispatch_cb = ppa_dispatch;
    draw_ppa_unit->base_unit.delete_cb = ppa_delete;

    /* Register SRM client */
    cfg.oper_type = PPA_OPERATION_SRM;
    cfg.max_pending_trans_num = 1;
    cfg.data_burst_length = PPA_DATA_BURST_LENGTH_128;

    res = ppa_register_client(&cfg, &draw_ppa_unit->srm_client);
    LV_ASSERT(res == ESP_OK);

    /* Register Fill client */
    cfg.oper_type = PPA_OPERATION_FILL;
    cfg.data_burst_length = PPA_DATA_BURST_LENGTH_128;

    res = ppa_register_client(&cfg, &draw_ppa_unit->fill_client);
    LV_ASSERT(res == ESP_OK);

    /* Register Blend client */
    cfg.oper_type = PPA_OPERATION_BLEND;
    cfg.data_burst_length = PPA_DATA_BURST_LENGTH_128;

    res = ppa_register_client(&cfg, &draw_ppa_unit->blend_client);
    LV_ASSERT(res == ESP_OK);
}

void lv_draw_ppa_deinit(void)
{
}

/**********************
 *   STATIC FUNCTIONS
 **********************/
static int32_t ppa_evaluate(lv_draw_unit_t * u, lv_draw_task_t * t)
{
    LV_UNUSED(u);

    switch(t->type) {
        case LV_DRAW_TASK_TYPE_FILL: {
            const lv_draw_fill_dsc_t * dsc = (const lv_draw_fill_dsc_t *)t->draw_dsc;
            if(dsc->radius != 0) return 0;
            if(dsc->grad.dir != LV_GRAD_DIR_NONE) return 0;
            if(dsc->opa < (lv_opa_t)LV_OPA_MAX) return 0;

            lv_draw_buf_t * draw_buf = t->target_layer->draw_buf;
            if(!ppa_dest_cf_supported((lv_color_format_t)draw_buf->header.cf)) return 0;

            if(t->preference_score > 70) {
                t->preference_score = 70;
                t->preferred_draw_unit_id = DRAW_UNIT_ID_PPA;
            }
            return 1;
        }

        case LV_DRAW_TASK_TYPE_IMAGE: {
            const lv_draw_image_dsc_t * dsc = (const lv_draw_image_dsc_t *)t->draw_dsc;
            if(dsc->rotation != 0) return 0;
            if(dsc->skew_x != 0 || dsc->skew_y != 0) return 0;
            if(dsc->scale_x != LV_SCALE_NONE || dsc->scale_y != LV_SCALE_NONE) return 0;
            if(dsc->opa < (lv_opa_t)LV_OPA_MAX) return 0;
            if(dsc->blend_mode != LV_BLEND_MODE_NORMAL) return 0;
            if(!ppa_src_cf_supported((lv_color_format_t)dsc->header.cf)) return 0;

            lv_draw_buf_t * dest_buf = t->target_layer->draw_buf;
            if(!ppa_dest_cf_supported((lv_color_format_t)dest_buf->header.cf)) return 0;

            if(t->preference_score > 70) {
                t->preference_score = 70;
                t->preferred_draw_unit_id = DRAW_UNIT_ID_PPA;
            }
            return 1;
        }

        default:
            break;
    }

    return 0;
}

static int32_t ppa_dispatch(lv_draw_unit_t * draw_unit, lv_layer_t * layer)
{
    lv_draw_ppa_unit_t * u = (lv_draw_ppa_unit_t *)draw_unit;
    if(u->task_act) {
        return LV_DRAW_UNIT_IDLE;
    }

    lv_draw_task_t * t = lv_draw_get_available_task(layer, NULL, DRAW_UNIT_ID_PPA);
    if(!t || t->preferred_draw_unit_id != DRAW_UNIT_ID_PPA) return LV_DRAW_UNIT_IDLE;
    if(lv_draw_layer_alloc_buf(layer) == NULL) return LV_DRAW_UNIT_IDLE;

    t->state = LV_DRAW_TASK_STATE_IN_PROGRESS;
    u->task_act = t;

    ppa_execute_drawing(u);

    u->task_act->state = LV_DRAW_TASK_STATE_FINISHED;
    u->task_act = NULL;
    lv_draw_dispatch_request();

    return 1;
}

static int32_t ppa_delete(lv_draw_unit_t * draw_unit)
{
    LV_UNUSED(draw_unit);
    return 0;
}

static void ppa_execute_drawing(lv_draw_ppa_unit_t * u)
{
    lv_draw_task_t * t         = u->task_act;
    lv_layer_t * layer         = t->target_layer;
    lv_draw_buf_t * buf        = layer->draw_buf;
    lv_area_t area;

    if(!lv_area_intersect(&area, &t->area, &t->clip_area)) return;
    lv_draw_buf_invalidate_cache(buf, &area);

    switch(t->type) {
        case LV_DRAW_TASK_TYPE_FILL:
            lv_draw_ppa_fill(t, (lv_draw_fill_dsc_t *)t->draw_dsc, &area);
            lv_draw_buf_invalidate_cache(buf, &area);
            break;
        case LV_DRAW_TASK_TYPE_IMAGE:
            lv_draw_ppa_img(t, (lv_draw_image_dsc_t *)t->draw_dsc, &area);
            lv_draw_buf_invalidate_cache(buf, &area);
            break;
        default:
            break;
    }
}

#endif /* CONFIG_SOC_PPA_SUPPORTED */
