#pragma once

#ifdef USE_ESP32

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <cstring>
#include <lvgl.h>

// Access lv_lottie_t internals for safe re-initialisation on screen re-load.
// Needed to null out the dangling anim pointer and to clear the ThorVG canvas
// before re-pushing the paint.
#include <src/widgets/lottie/lv_lottie_private.h>

namespace esphome {
namespace lvgl {

static const char *const LOTTIE_TAG = "lottie";
static constexpr size_t LOTTIE_TASK_STACK_SIZE = 64 * 1024;

// Persistent context for each Lottie widget – tracks all PSRAM allocations,
// the render task, and cached animation parameters for safe re-load.
struct LottieContext {
    // --- Config (set once, never freed) ---
    lv_obj_t *obj;
    const void *data;           // PROGMEM (embedded) or nullptr
    size_t data_size;
    const char *file_path;      // string literal or nullptr
    bool loop;
    bool auto_start;
    uint32_t width;
    uint32_t height;

    // --- Animation params (captured on first load, reused on re-loads) ---
    lv_anim_exec_xcb_t exec_cb;
    void *anim_var;
    int32_t start_frame;
    int32_t end_frame;
    uint32_t duration_ms;
    bool data_loaded;           // true after first successful parse

    // --- Runtime state (freed on screen unload) ---
    uint8_t *pixel_buffer;      // PSRAM – width*height*4
    StackType_t *task_stack;    // PSRAM – 64 KB
    StaticTask_t *task_tcb;     // internal RAM
    TaskHandle_t task_handle;
    volatile bool stop_requested;
};

// --------------------------------------------------------------------------
// Render task – runs on 64 KB PSRAM stack.
//
// First load:  set buffer → parse data → capture anim params → render loop
// Re-load:     clear canvas → set buffer (no re-parse) → render loop
//
// lv_lottie_set_buffer() MUST be called from this task (not from an LVGL
// event callback) because it internally triggers a ThorVG render that
// needs the large stack.
// --------------------------------------------------------------------------
inline void lottie_load_task(void *param) {
    LottieContext *ctx = (LottieContext *)param;

    // Wait for LVGL to be fully running
    vTaskDelay(pdMS_TO_TICKS(1000));

    lv_lock();

    if (!ctx->data_loaded) {
        // ===== FIRST LOAD =====
        ESP_LOGI(LOTTIE_TAG, "First load: parsing lottie data...");

        // Set pixel buffer – this calls anim_exec_cb internally but since
        // no data is loaded yet ThorVG has nothing to render (safe).
        lv_lottie_set_buffer(ctx->obj, ctx->width, ctx->height, ctx->pixel_buffer);

        // Parse lottie data (heavy ThorVG work – needs 64 KB stack)
        if (ctx->data != nullptr) {
            lv_lottie_set_src_data(ctx->obj, ctx->data, ctx->data_size);
            ESP_LOGI(LOTTIE_TAG, "Data loaded from embedded source (%d bytes)", (int)ctx->data_size);
        } else if (ctx->file_path != nullptr) {
            lv_lottie_set_src_file(ctx->obj, ctx->file_path);
            ESP_LOGI(LOTTIE_TAG, "Data loaded from file: %s", ctx->file_path);
        }

        // Capture animation parameters before deleting the LVGL animation
        lv_anim_t *anim = lv_lottie_get_anim(ctx->obj);
        if (anim != nullptr) {
            ctx->exec_cb     = anim->exec_cb;
            ctx->anim_var    = anim->var;
            ctx->start_frame = anim->start_value;
            ctx->end_frame   = anim->end_value;
            ctx->duration_ms = (uint32_t)lv_anim_get_time(anim);

            ESP_LOGI(LOTTIE_TAG, "Anim: frames %d..%d, duration %u ms",
                     (int)ctx->start_frame, (int)ctx->end_frame, (unsigned)ctx->duration_ms);

            // Delete the LVGL animation – we drive rendering ourselves
            // from this PSRAM task instead of the main task (small stack).
            lv_anim_delete(ctx->anim_var, ctx->exec_cb);

            // CRITICAL: null out the dangling pointer in lv_lottie_t.
            // Without this, anim_exec_cb (called by lv_lottie_set_buffer
            // on re-load) would dereference freed memory.
            lv_lottie_t *lottie = (lv_lottie_t *)ctx->obj;
            lottie->anim = NULL;

            ctx->data_loaded = true;
            ESP_LOGI(LOTTIE_TAG, "LVGL anim removed – rendering from PSRAM task");
        } else {
            ESP_LOGE(LOTTIE_TAG, "Animation INVALID – parsing may have failed!");
        }
    } else {
        // ===== RE-LOAD (screen came back) =====
        // Data is already parsed in the lv_lottie widget.  We just need
        // to point ThorVG + LVGL canvas at the new pixel buffer.
        //
        // tvg_canvas_clear removes the paint from the canvas without
        // deleting it (false), so lv_lottie_set_buffer can push it again
        // without a double-push.
        ESP_LOGI(LOTTIE_TAG, "Re-load: updating buffer (no re-parse)");

        lv_lottie_t *lottie = (lv_lottie_t *)ctx->obj;
        tvg_canvas_clear(lottie->tvg_canvas, false);

        // Safe to call: widget is hidden (lv_obj_is_visible → false)
        // and lottie->anim is NULL (no dangling pointer access).
        lv_lottie_set_buffer(ctx->obj, ctx->width, ctx->height, ctx->pixel_buffer);
    }

    // Show widget
    lv_obj_remove_flag(ctx->obj, LV_OBJ_FLAG_HIDDEN);

    lv_unlock();

    // Validate animation parameters
    if (!ctx->data_loaded || ctx->exec_cb == nullptr ||
        ctx->duration_ms == 0 || ctx->end_frame <= ctx->start_frame) {
        ESP_LOGW(LOTTIE_TAG, "No valid animation, task suspending");
        vTaskSuspend(NULL);
        return;
    }
    if (!ctx->auto_start) {
        ESP_LOGI(LOTTIE_TAG, "auto_start=false, task suspending");
        vTaskSuspend(NULL);
        return;
    }

    // --- Frame render loop (64 KB PSRAM stack) ---
    int32_t total_frames = ctx->end_frame - ctx->start_frame;
    uint32_t frame_delay_ms = ctx->duration_ms / (uint32_t)total_frames;
    if (frame_delay_ms < 16)  frame_delay_ms = 16;
    if (frame_delay_ms > 100) frame_delay_ms = 100;

    ESP_LOGI(LOTTIE_TAG, "Render loop: %u ms/frame, loop=%d",
             (unsigned)frame_delay_ms, (int)ctx->loop);

    TickType_t start_tick = xTaskGetTickCount();

    while (!ctx->stop_requested) {
        uint32_t elapsed_ms = (uint32_t)((xTaskGetTickCount() - start_tick) * portTICK_PERIOD_MS);

        int32_t frame;
        if (ctx->loop) {
            uint32_t phase = elapsed_ms % ctx->duration_ms;
            frame = ctx->start_frame + (int32_t)((int64_t)total_frames * phase / ctx->duration_ms);
        } else {
            if (elapsed_ms >= ctx->duration_ms) {
                lv_lock();
                ctx->exec_cb(ctx->anim_var, ctx->end_frame);
                lv_unlock();
                ESP_LOGI(LOTTIE_TAG, "Animation complete");
                break;
            }
            frame = ctx->start_frame + (int32_t)((int64_t)total_frames * elapsed_ms / ctx->duration_ms);
        }

        lv_lock();
        ctx->exec_cb(ctx->anim_var, frame);
        lv_unlock();

        vTaskDelay(pdMS_TO_TICKS(frame_delay_ms));
    }

    if (ctx->stop_requested) {
        ESP_LOGI(LOTTIE_TAG, "Stop requested – task suspending");
    }

    // Suspend (NOT delete) – cleanup callback will delete us safely
    vTaskSuspend(NULL);
}

// --------------------------------------------------------------------------
// Free all PSRAM/internal-RAM resources for one Lottie widget.
// --------------------------------------------------------------------------
inline void lottie_free_resources(LottieContext *ctx) {
    ctx->stop_requested = true;
    if (ctx->task_handle) {
        vTaskDelete(ctx->task_handle);
        ctx->task_handle = nullptr;
    }
    if (ctx->task_stack)    { heap_caps_free(ctx->task_stack);    ctx->task_stack = nullptr; }
    if (ctx->task_tcb)      { heap_caps_free(ctx->task_tcb);      ctx->task_tcb = nullptr; }
    if (ctx->pixel_buffer)  { heap_caps_free(ctx->pixel_buffer);  ctx->pixel_buffer = nullptr; }
    ctx->stop_requested = false;

    ESP_LOGI(LOTTIE_TAG, "Lottie PSRAM freed (%ux%u = %u KB + 64 KB stack)",
             (unsigned)ctx->width, (unsigned)ctx->height,
             (unsigned)(ctx->width * ctx->height * 4 / 1024));
}

// --------------------------------------------------------------------------
// (Re-)allocate pixel buffer and launch the render task.
// lv_lottie_set_buffer is NOT called here – it is called inside the task
// because it triggers ThorVG rendering which needs the 64 KB stack.
// --------------------------------------------------------------------------
inline bool lottie_launch(LottieContext *ctx) {
    // Allocate pixel buffer in PSRAM
    size_t buf_bytes = (size_t)ctx->width * ctx->height * 4;
    ctx->pixel_buffer = (uint8_t *)heap_caps_malloc(
        buf_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ctx->pixel_buffer) {
        ESP_LOGE(LOTTIE_TAG, "PSRAM alloc failed (%u bytes)", (unsigned)buf_bytes);
        return false;
    }
    memset(ctx->pixel_buffer, 0, buf_bytes);

    // Hide until the task sets the buffer and loads data
    lv_obj_add_flag(ctx->obj, LV_OBJ_FLAG_HIDDEN);

    // Allocate task stack + TCB
    ctx->task_stack = (StackType_t *)heap_caps_malloc(
        LOTTIE_TASK_STACK_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ctx->task_tcb = (StaticTask_t *)heap_caps_malloc(
        sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!ctx->task_stack || !ctx->task_tcb) {
        ESP_LOGE(LOTTIE_TAG, "Task alloc failed");
        lottie_free_resources(ctx);
        return false;
    }

    ctx->stop_requested = false;
    ctx->task_handle = xTaskCreateStatic(
        lottie_load_task, "lottie_anim",
        LOTTIE_TASK_STACK_SIZE / sizeof(StackType_t),
        ctx, 5, ctx->task_stack, ctx->task_tcb);

    if (!ctx->task_handle) {
        lottie_free_resources(ctx);
        return false;
    }

    ESP_LOGI(LOTTIE_TAG, "Lottie task launched (%u KB PSRAM stack)",
             (unsigned)(LOTTIE_TASK_STACK_SIZE / 1024));
    return true;
}

// --------------------------------------------------------------------------
// Screen event callbacks – two-phase unload to avoid drawing freed buffer
// during screen transition animation.
//
//   SCREEN_UNLOAD_START  → stop task + hide widget (LVGL still draws screen)
//   SCREEN_UNLOADED      → free PSRAM (screen no longer visible)
//   SCREEN_LOADED        → re-allocate and re-launch
// --------------------------------------------------------------------------
inline void lottie_screen_unload_start_cb(lv_event_t *e) {
    LottieContext *ctx = (LottieContext *)lv_event_get_user_data(e);

    // Stop the render task immediately
    ctx->stop_requested = true;
    if (ctx->task_handle) {
        vTaskDelete(ctx->task_handle);
        ctx->task_handle = nullptr;
    }

    // Hide widget so LVGL won't try to draw the image during transition
    lv_obj_add_flag(ctx->obj, LV_OBJ_FLAG_HIDDEN);

    ESP_LOGI(LOTTIE_TAG, "Lottie task stopped, widget hidden (transition starting)");
}

inline void lottie_screen_unloaded_cb(lv_event_t *e) {
    LottieContext *ctx = (LottieContext *)lv_event_get_user_data(e);

    // Now safe to free – screen is no longer visible
    if (ctx->task_stack)    { heap_caps_free(ctx->task_stack);    ctx->task_stack = nullptr; }
    if (ctx->task_tcb)      { heap_caps_free(ctx->task_tcb);      ctx->task_tcb = nullptr; }
    if (ctx->pixel_buffer)  { heap_caps_free(ctx->pixel_buffer);  ctx->pixel_buffer = nullptr; }
    ctx->stop_requested = false;

    ESP_LOGI(LOTTIE_TAG, "Lottie PSRAM freed (%ux%u = %u KB + 64 KB stack)",
             (unsigned)ctx->width, (unsigned)ctx->height,
             (unsigned)(ctx->width * ctx->height * 4 / 1024));
}

inline void lottie_screen_loaded_cb(lv_event_t *e) {
    LottieContext *ctx = (LottieContext *)lv_event_get_user_data(e);
    if (ctx->pixel_buffer == nullptr) {
        lottie_launch(ctx);
    }
}

// --------------------------------------------------------------------------
// Public API: initialise Lottie widget – allocate buffer, register screen
// events, and launch the load/render task.
// Call under lv_lock (from LVGL init code).
// --------------------------------------------------------------------------
inline bool lottie_init(lv_obj_t *obj, const void *data, size_t data_size,
                         const char *file_path, uint32_t width, uint32_t height,
                         bool loop, bool auto_start) {
    LottieContext *ctx = (LottieContext *)heap_caps_malloc(
        sizeof(LottieContext), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!ctx) return false;
    memset(ctx, 0, sizeof(LottieContext));

    ctx->obj       = obj;
    ctx->data      = data;
    ctx->data_size = data_size;
    ctx->file_path = file_path;
    ctx->loop      = loop;
    ctx->auto_start = auto_start;
    ctx->width     = width;
    ctx->height    = height;

    // Register screen events for PSRAM lifecycle (two-phase unload)
    lv_obj_t *screen = lv_obj_get_screen(obj);
    lv_obj_add_event_cb(screen, lottie_screen_unload_start_cb,
                        LV_EVENT_SCREEN_UNLOAD_START, ctx);
    lv_obj_add_event_cb(screen, lottie_screen_unloaded_cb,
                        LV_EVENT_SCREEN_UNLOADED, ctx);
    lv_obj_add_event_cb(screen, lottie_screen_loaded_cb,
                        LV_EVENT_SCREEN_LOADED, ctx);

    return lottie_launch(ctx);
}

}  // namespace lvgl
}  // namespace esphome

#endif  // USE_ESP32
