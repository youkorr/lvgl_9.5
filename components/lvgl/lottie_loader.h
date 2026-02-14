#pragma once

#ifdef USE_ESP32

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <cstring>
#include <lvgl.h>
#include <src/widgets/lottie/lv_lottie_private.h>

namespace esphome {
namespace lvgl {

static const char *const LOTTIE_TAG = "lottie";
static constexpr size_t LOTTIE_TASK_STACK_SIZE = 96 * 1024;  // Increased from 64KB to prevent stack overflow

struct LottieContext {
    lv_obj_t *obj;
    const void *data;
    size_t data_size;
    const char *file_path;
    bool loop;
    bool auto_start;
    uint32_t width;
    uint32_t height;

    lv_anim_exec_xcb_t exec_cb;
    void *anim_var;
    int32_t start_frame;
    int32_t end_frame;
    uint32_t duration_ms;
    bool data_loaded;

    uint8_t *pixel_buffer;
    StackType_t *task_stack;
    StaticTask_t *task_tcb;
    TaskHandle_t task_handle;
    volatile bool stop_requested;
    volatile bool is_paused;  // NEW: Track if animation is paused due to hidden state
    bool initial_hidden;  // Track if widget was initially hidden in YAML
};

// Forward declarations
inline void lottie_free_resources(LottieContext *ctx);



// ============================================================
// LOTTIE TASK
// ============================================================

inline void lottie_load_task(void *param) {
    LottieContext *ctx = (LottieContext *)param;

    vTaskDelay(pdMS_TO_TICKS(1000));

    lv_lock();

    if (!ctx->data_loaded) {

        ESP_LOGI(LOTTIE_TAG, "First load");

        lv_lottie_set_buffer(ctx->obj, ctx->width, ctx->height, ctx->pixel_buffer);

        if (ctx->data != nullptr) {
            lv_lottie_set_src_data(ctx->obj, ctx->data, ctx->data_size);
        } else if (ctx->file_path != nullptr) {
            lv_lottie_set_src_file(ctx->obj, ctx->file_path);
        }

        lv_anim_t *anim = lv_lottie_get_anim(ctx->obj);
        if (anim != nullptr) {

            ctx->exec_cb     = anim->exec_cb;
            ctx->anim_var    = anim->var;
            ctx->start_frame = anim->start_value;
            ctx->end_frame   = anim->end_value;
            ctx->duration_ms = (uint32_t)lv_anim_get_time(anim);

            lv_anim_delete(ctx->anim_var, ctx->exec_cb);

            lv_lottie_t *lottie = (lv_lottie_t *)ctx->obj;
            lottie->anim = NULL;

            ctx->data_loaded = true;
        }
    } else {

        ESP_LOGI(LOTTIE_TAG, "Reload");

        lv_lottie_t *lottie = (lv_lottie_t *)ctx->obj;
        tvg_canvas_clear(lottie->tvg_canvas, false);
        lv_lottie_set_buffer(ctx->obj, ctx->width, ctx->height, ctx->pixel_buffer);
    }

    // Restore visibility based on initial state
    // If widget was NOT hidden in YAML, show it now that data is loaded
    // If widget WAS hidden in YAML (e.g., weather widgets), keep it hidden
    if (!ctx->initial_hidden) {
        lv_obj_remove_flag(ctx->obj, LV_OBJ_FLAG_HIDDEN);
    }

    lv_unlock();



    if (!ctx->data_loaded || ctx->exec_cb == nullptr ||
        ctx->duration_ms == 0 || ctx->end_frame <= ctx->start_frame) {

        ctx->task_handle = nullptr;
        vTaskDelete(NULL);
        return;
    }

    if (!ctx->auto_start) {
        ctx->task_handle = nullptr;
        vTaskDelete(NULL);
        return;
    }



    int32_t total_frames = ctx->end_frame - ctx->start_frame;
    uint32_t frame_delay_ms = ctx->duration_ms / (uint32_t)total_frames;

    if (frame_delay_ms < 16)  frame_delay_ms = 16;
    if (frame_delay_ms > 100) frame_delay_ms = 100;

    TickType_t start_tick = xTaskGetTickCount();
    TickType_t pause_start_tick = 0;

    while (!ctx->stop_requested) {

        // Check if widget is hidden - if so, pause animation to save CPU
        lv_lock();
        bool is_hidden = lv_obj_has_flag(ctx->obj, LV_OBJ_FLAG_HIDDEN);
        lv_unlock();

        if (is_hidden) {
            if (!ctx->is_paused) {
                // Just became hidden - save pause timestamp
                pause_start_tick = xTaskGetTickCount();
                ctx->is_paused = true;
                ESP_LOGI(LOTTIE_TAG, "Animation paused (widget hidden)");
            }

            // If hidden for more than 2 seconds, free memory and stop task
            // This allows lazy-loaded weather widgets to be unloaded when not visible
            TickType_t hidden_duration = xTaskGetTickCount() - pause_start_tick;
            if (hidden_duration > pdMS_TO_TICKS(2000)) {
                ESP_LOGI(LOTTIE_TAG, "Widget hidden for 2s, freeing memory and stopping task");
                ctx->stop_requested = true;
                break;
            }

            // Sleep longer while paused to save CPU
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        } else {
            if (ctx->is_paused) {
                // Just became visible - adjust start tick to account for pause duration
                TickType_t pause_duration = xTaskGetTickCount() - pause_start_tick;
                start_tick += pause_duration;
                ctx->is_paused = false;
                ESP_LOGI(LOTTIE_TAG, "Animation resumed (widget visible)");
            }
        }

        uint32_t elapsed_ms =
            (uint32_t)((xTaskGetTickCount() - start_tick) * portTICK_PERIOD_MS);

        int32_t frame;

        if (ctx->loop) {
            uint32_t phase = elapsed_ms % ctx->duration_ms;
            frame = ctx->start_frame +
                    (int32_t)((int64_t)total_frames * phase / ctx->duration_ms);
        } else {
            if (elapsed_ms >= ctx->duration_ms) {
                if (!ctx->stop_requested) {
                    lv_lock();
                    ctx->exec_cb(ctx->anim_var, ctx->end_frame);
                    lv_unlock();
                }
                break;
            }
            frame = ctx->start_frame +
                    (int32_t)((int64_t)total_frames * elapsed_ms / ctx->duration_ms);
        }

        if (ctx->stop_requested)
            break;

        lv_lock();
        ctx->exec_cb(ctx->anim_var, frame);
        lv_unlock();

        vTaskDelay(pdMS_TO_TICKS(frame_delay_ms));
    }

    ESP_LOGI(LOTTIE_TAG, "Task exiting cleanly");

    // Free resources before exiting (e.g., if hidden for 2s)
    lottie_free_resources(ctx);

    ctx->task_handle = nullptr;
    vTaskDelete(NULL);
}



// ============================================================
// RESOURCE FREE
// ============================================================

inline void lottie_wait_task_stop(LottieContext *ctx) {
    ctx->stop_requested = true;

    TickType_t timeout = xTaskGetTickCount() + pdMS_TO_TICKS(500);

    while (ctx->task_handle != nullptr &&
           xTaskGetTickCount() < timeout) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

inline void lottie_free_resources(LottieContext *ctx) {

    lottie_wait_task_stop(ctx);

    if (ctx->task_stack)   { heap_caps_free(ctx->task_stack);   ctx->task_stack = nullptr; }
    if (ctx->task_tcb)     { heap_caps_free(ctx->task_tcb);     ctx->task_tcb = nullptr; }
    if (ctx->pixel_buffer) { heap_caps_free(ctx->pixel_buffer); ctx->pixel_buffer = nullptr; }

    ctx->stop_requested = false;
    ctx->is_paused = false;
}



// ============================================================
// LAUNCH
// ============================================================

inline bool lottie_launch(LottieContext *ctx) {

    size_t buf_bytes = (size_t)ctx->width * ctx->height * 4;

    ctx->pixel_buffer =
        (uint8_t *)heap_caps_malloc(buf_bytes,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (!ctx->pixel_buffer)
        return false;

    memset(ctx->pixel_buffer, 0, buf_bytes);

    // Hide widget temporarily during load to prevent displaying empty/glitchy content
    // initial_hidden was already saved in lottie_init() from YAML config
    lv_obj_add_flag(ctx->obj, LV_OBJ_FLAG_HIDDEN);

    ctx->task_stack =
        (StackType_t *)heap_caps_malloc(
            LOTTIE_TASK_STACK_SIZE,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    ctx->task_tcb =
        (StaticTask_t *)heap_caps_malloc(
            sizeof(StaticTask_t),
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    if (!ctx->task_stack || !ctx->task_tcb) {
        lottie_free_resources(ctx);
        return false;
    }

    ctx->stop_requested = false;
    ctx->is_paused = false;

    ctx->task_handle = xTaskCreateStatic(
        lottie_load_task,
        "lottie_anim",
        LOTTIE_TASK_STACK_SIZE / sizeof(StackType_t),
        ctx,
        1,  // LOW PRIORITY to prevent CPU monopolization
        ctx->task_stack,
        ctx->task_tcb);

    return ctx->task_handle != nullptr;
}



// ============================================================
// SCREEN EVENTS
// ============================================================

inline void lottie_screen_unload_start_cb(lv_event_t *e) {
    LottieContext *ctx =
        (LottieContext *)lv_event_get_user_data(e);

    lottie_wait_task_stop(ctx);

    lv_obj_add_flag(ctx->obj, LV_OBJ_FLAG_HIDDEN);
}

inline void lottie_screen_unloaded_cb(lv_event_t *e) {
    LottieContext *ctx =
        (LottieContext *)lv_event_get_user_data(e);

    lottie_free_resources(ctx);
}

inline void lottie_screen_loaded_cb(lv_event_t *e) {
    LottieContext *ctx =
        (LottieContext *)lv_event_get_user_data(e);

    // Only reload if widget was NOT initially hidden in YAML
    // Use initial_hidden instead of current flag because widget may be temporarily
    // hidden during loading. This ensures:
    // - Visible widgets (e.g., screensaver) always reload
    // - Hidden widgets (e.g., weather) use lazy loading via visibility callback
    if (ctx->pixel_buffer == nullptr && !ctx->initial_hidden) {
        lottie_launch(ctx);
    }
}

inline void lottie_widget_draw_cb(lv_event_t *e) {
    LottieContext *ctx =
        (LottieContext *)lv_event_get_user_data(e);

    // LV_EVENT_DRAW_MAIN_BEGIN is ONLY called when widget is visible (not hidden)
    // If we reach here and buffer is null, it means widget just became visible
    // This handles lazy loading for hidden weather widgets that become visible
    if (ctx->pixel_buffer == nullptr && ctx->task_handle == nullptr) {
        ESP_LOGI(LOTTIE_TAG, "Widget became visible, lazy loading now");
        lottie_launch(ctx);
    }
}




// ============================================================
// INIT
// ============================================================

inline bool lottie_init(lv_obj_t *obj,
                        const void *data,
                        size_t data_size,
                        const char *file_path,
                        uint32_t width,
                        uint32_t height,
                        bool loop,
                        bool auto_start) {

    LottieContext *ctx =
        (LottieContext *)heap_caps_malloc(
            sizeof(LottieContext),
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    if (!ctx)
        return false;

    memset(ctx, 0, sizeof(LottieContext));

    ctx->obj        = obj;
    ctx->data       = data;
    ctx->data_size  = data_size;
    ctx->file_path  = file_path;
    ctx->loop       = loop;
    ctx->auto_start = auto_start;
    ctx->width      = width;
    ctx->height     = height;

    // Save initial hidden state from YAML before any modifications
    // This determines if widget should auto-start (visible) or lazy-load (hidden)
    ctx->initial_hidden = lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *screen = lv_obj_get_screen(obj);

    lv_obj_add_event_cb(screen,
                        lottie_screen_unload_start_cb,
                        LV_EVENT_SCREEN_UNLOAD_START,
                        ctx);

    lv_obj_add_event_cb(screen,
                        lottie_screen_unloaded_cb,
                        LV_EVENT_SCREEN_UNLOADED,
                        ctx);

    lv_obj_add_event_cb(screen,
                        lottie_screen_loaded_cb,
                        LV_EVENT_SCREEN_LOADED,
                        ctx);

    // Add event listener on the widget itself to handle lazy loading
    // when a hidden widget becomes visible after screen reload
    lv_obj_add_event_cb(obj,
                        lottie_widget_draw_cb,
                        LV_EVENT_DRAW_MAIN_BEGIN,
                        ctx);

    // Only auto-launch if widget is NOT initially hidden
    // Hidden widgets (e.g., weather) will lazy-load via LV_EVENT_DRAW_MAIN_BEGIN
    // when they become visible
    if (!ctx->initial_hidden) {
        return lottie_launch(ctx);
    }

    return true;
}

}  // namespace lvgl
}  // namespace esphome

#endif










