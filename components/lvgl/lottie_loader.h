#pragma once

#ifdef USE_ESP32

// Only compile when Lottie widget is actually used (LV_USE_LOTTIE=1 in lv_conf.h).
// This header is pulled in via esphome.h for all builds, so we must guard it.
#include <lvgl.h>
#if LV_USE_LOTTIE

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <cstring>

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
    volatile bool restart_requested;  // ✅ Flag to restart animation from frame 0
    TickType_t start_tick;            // ✅ Animation start time (can be reset)
    bool user_wants_hidden;     // Save user's 'hidden' config from YAML
    bool runtime_hidden;        // Actual visibility at time of unload (captures script changes)

    // --- Safe shutdown synchronisation ---
    // Static binary semaphore signalled by the render task right before it
    // self-deletes. The unload sequence waits on this before freeing the
    // task stack/TCB so we can never free memory still owned by the task.
    StaticSemaphore_t exit_sem_storage;
    SemaphoreHandle_t exit_sem;
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
// Helper used by every exit path of lottie_load_task: signal the unload
// sequence that we are done touching shared state, then self-delete so the
// FreeRTOS scheduler tears the task down cleanly. This is much safer than
// having the unload callback call vTaskDelete() on us – that would kill the
// task while it might be holding the recursive lv_lock() mutex, leaving the
// mutex permanently owned by a dead task and dead-locking the next call to
// lv_timer_handler() (the actual cause of the watchdog reset reported on
// auto-lock with multiple Lottie widgets on screen).
[[noreturn]] inline void lottie_task_exit(LottieContext *ctx) {
    if (ctx->exit_sem) {
        xSemaphoreGive(ctx->exit_sem);
    }
    vTaskDelete(NULL);
    // Unreachable: vTaskDelete(NULL) does not return for the calling task.
    for (;;) { vTaskDelay(portMAX_DELAY); }
}

inline void lottie_load_task(void *param) {
    LottieContext *ctx = (LottieContext *)param;

    // Wait for LVGL to be ready.
    // First load needs longer delay (LVGL may still be initialising).
    // Re-load needs only a short delay (LVGL is already running).
    vTaskDelay(pdMS_TO_TICKS(ctx->data_loaded ? 100 : 1000));

    // Bail out early if a stop was requested before we even started: the
    // screen could have been swapped right after launch (e.g. boot animation
    // racing with the auto-lock timer).
    if (ctx->stop_requested) {
        ESP_LOGI(LOTTIE_TAG, "Stop requested before init, exiting");
        lottie_task_exit(ctx);
    }

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

    // Restore visibility: use runtime_hidden which captures the actual state
    // before page unload (preserves dynamic show/hide from user scripts).
    // On first load, runtime_hidden == user_wants_hidden (from YAML config).
    if (!ctx->runtime_hidden) {
        lv_obj_remove_flag(ctx->obj, LV_OBJ_FLAG_HIDDEN);
    }

    lv_unlock();

    // Validate animation parameters
    if (!ctx->data_loaded || ctx->exec_cb == nullptr ||
        ctx->duration_ms == 0 || ctx->end_frame <= ctx->start_frame) {
        ESP_LOGW(LOTTIE_TAG, "No valid animation, task exiting");
        lottie_task_exit(ctx);
    }
    if (!ctx->auto_start) {
        ESP_LOGI(LOTTIE_TAG, "auto_start=false, task exiting (idle until reload)");
        lottie_task_exit(ctx);
    }

    // --- Frame render loop (64 KB PSRAM stack) ---
    int32_t total_frames = ctx->end_frame - ctx->start_frame;
    uint32_t frame_delay_ms = ctx->duration_ms / (uint32_t)total_frames;
    if (frame_delay_ms < 16)  frame_delay_ms = 16;
    if (frame_delay_ms > 100) frame_delay_ms = 100;

    ESP_LOGI(LOTTIE_TAG, "Render loop: %u ms/frame, loop=%d",
             (unsigned)frame_delay_ms, (int)ctx->loop);

    ctx->start_tick = xTaskGetTickCount();  // ✅ Store in context for restart capability

    while (!ctx->stop_requested) {
        // ✅ Check if restart requested
        if (ctx->restart_requested) {
            ctx->start_tick = xTaskGetTickCount();
            ctx->restart_requested = false;
            ESP_LOGI(LOTTIE_TAG, "Animation restarted from frame 0");
        }

        uint32_t elapsed_ms = (uint32_t)((xTaskGetTickCount() - ctx->start_tick) * portTICK_PERIOD_MS);

        int32_t frame;
        bool render_end_frame = false;
        if (ctx->loop) {
            uint32_t phase = elapsed_ms % ctx->duration_ms;
            frame = ctx->start_frame + (int32_t)((int64_t)total_frames * phase / ctx->duration_ms);
        } else {
            if (elapsed_ms >= ctx->duration_ms) {
                frame = ctx->end_frame;
                render_end_frame = true;
            } else {
                frame = ctx->start_frame + (int32_t)((int64_t)total_frames * elapsed_ms / ctx->duration_ms);
            }
        }

        // Acquire lv_lock then re-check stop_requested. Without this
        // double-check, a stop signalled while we are blocked on lv_lock
        // would still cause one final exec_cb(...) to run after the unload
        // has already begun – exec_cb writes into the lottie pixel buffer
        // that is about to be freed.
        lv_lock();
        if (ctx->stop_requested) {
            lv_unlock();
            break;
        }
        ctx->exec_cb(ctx->anim_var, frame);
        lv_unlock();

        if (render_end_frame) {
            ESP_LOGI(LOTTIE_TAG, "Animation complete");
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(frame_delay_ms));
    }

    if (ctx->stop_requested) {
        ESP_LOGI(LOTTIE_TAG, "Stop requested – task exiting cleanly");
    }

    // Self-delete after signalling the unload sequence. This is the only
    // safe way to tear the task down: an external vTaskDelete() can kill
    // us mid-critical-section and orphan lv_lock() forever.
    lottie_task_exit(ctx);
}

// --------------------------------------------------------------------------
// Wait (with timeout) for the render task to acknowledge stop_requested and
// self-delete via lottie_task_exit(). Returns true on clean shutdown.
//
// We MUST wait here before freeing the static stack/TCB. On ESP32-P4 (SMP),
// the render task may be running on the other core; freeing its stack while
// it is still executing causes random memory corruption that surfaces much
// later as a watchdog reset on the loop task.
// --------------------------------------------------------------------------
inline bool lottie_wait_for_task_exit(LottieContext *ctx, uint32_t timeout_ms) {
    if (!ctx->task_handle) return true;

    bool clean = false;
    if (ctx->exit_sem) {
        if (xSemaphoreTake(ctx->exit_sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) {
            clean = true;
        }
    } else {
        // Should not happen, but degrade gracefully.
        vTaskDelay(pdMS_TO_TICKS(timeout_ms));
    }

    if (!clean) {
        // Last-resort: task did not exit voluntarily. Force-delete and pray
        // it was not holding lv_lock; then give the scheduler a tick to
        // process the deletion on both cores before we free its stack.
        ESP_LOGW(LOTTIE_TAG, "Render task did not exit in %ums – force deleting",
                 (unsigned)timeout_ms);
        vTaskDelete(ctx->task_handle);
        vTaskDelay(pdMS_TO_TICKS(20));
    } else {
        // Even after the give, the task still has to execute vTaskDelete(NULL)
        // and the scheduler has to remove its TCB from internal lists. A
        // short delay closes the race against memory free in SMP mode.
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    ctx->task_handle = nullptr;
    return clean;
}

// --------------------------------------------------------------------------
// Free all PSRAM/internal-RAM resources for one Lottie widget.
// --------------------------------------------------------------------------
inline void lottie_free_resources(LottieContext *ctx) {
    ctx->stop_requested = true;
    lottie_wait_for_task_exit(ctx, 500);
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
    // NOTE: Do NOT re-capture runtime_hidden here!
    // On re-loads the widget is already hidden (by lottie_screen_unload_start_cb),
    // so reading LV_OBJ_FLAG_HIDDEN would always return true, losing the real
    // visibility state that was correctly saved in the unload callback.
    // runtime_hidden is set by:
    //   - lottie_init()                      → first load (from YAML config)
    //   - lottie_screen_unload_start_cb()    → re-loads  (actual state before hide)

    // Allocate pixel buffer in PSRAM
    size_t buf_bytes = (size_t)ctx->width * ctx->height * 4;
    ctx->pixel_buffer = (uint8_t *)heap_caps_malloc(
        buf_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ctx->pixel_buffer) {
        ESP_LOGE(LOTTIE_TAG, "PSRAM alloc failed (%u bytes)", (unsigned)buf_bytes);
        return false;
    }
    memset(ctx->pixel_buffer, 0, buf_bytes);

    // Hide temporarily during async load (pixel buffer is blank)
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

    // Drain any leftover give from a previous run so the next wait blocks
    // until THIS task actually signals exit.
    if (ctx->exit_sem) {
        xSemaphoreTake(ctx->exit_sem, 0);
    }

    ctx->task_handle = xTaskCreateStatic(
        lottie_load_task, "lottie_anim",
        LOTTIE_TASK_STACK_SIZE / sizeof(StackType_t),
        ctx, 5, ctx->task_stack, ctx->task_tcb);

    if (!ctx->task_handle) {
        lottie_free_resources(ctx);
        return false;
    }

    ESP_LOGI(LOTTIE_TAG, "Lottie launched (runtime_hidden=%d, PSRAM: %u KB buf + 64 KB stack, free PSRAM: %u KB, free SRAM: %u KB)",
             (int)ctx->runtime_hidden,
             (unsigned)(buf_bytes / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
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

    // Capture actual visibility BEFORE hiding – this preserves dynamic
    // show/hide from user scripts (e.g. weather widget selection).
    ctx->runtime_hidden = lv_obj_has_flag(ctx->obj, LV_OBJ_FLAG_HIDDEN);

    // Signal the render task to stop. We deliberately do NOT call
    // vTaskDelete() here: the render task may currently be holding
    // lv_lock() (the recursive LVGL mutex), and forcefully killing it
    // would orphan the mutex – the very next lv_timer_handler() call
    // from the loop task would then block forever and the task
    // watchdog would fire ~60 s later. Instead, the task observes
    // stop_requested, exits its critical section, and self-deletes.
    ctx->stop_requested = true;

    // Hide widget so LVGL won't try to draw the image during transition
    lv_obj_add_flag(ctx->obj, LV_OBJ_FLAG_HIDDEN);

    ESP_LOGI(LOTTIE_TAG, "Lottie stop requested, widget hidden (was_hidden=%d)",
             (int)ctx->runtime_hidden);
}

inline void lottie_screen_unloaded_cb(lv_event_t *e) {
    LottieContext *ctx = (LottieContext *)lv_event_get_user_data(e);

    // Wait for the render task to actually exit before freeing its memory.
    // 200 ms is enough headroom for the worst case (frame_delay_ms ≤ 100 ms
    // + one exec_cb iteration). With several Lottie widgets on the same
    // screen all stop_requested flags were set during UNLOAD_START, so the
    // tasks shut down in parallel on both cores – only the slowest matters.
    lottie_wait_for_task_exit(ctx, 200);

    if (ctx->task_stack)    { heap_caps_free(ctx->task_stack);    ctx->task_stack = nullptr; }
    if (ctx->task_tcb)      { heap_caps_free(ctx->task_tcb);      ctx->task_tcb = nullptr; }
    if (ctx->pixel_buffer)  { heap_caps_free(ctx->pixel_buffer);  ctx->pixel_buffer = nullptr; }
    ctx->stop_requested = false;

    ESP_LOGI(LOTTIE_TAG, "Lottie FREED (%ux%u = %u KB buf + 64 KB stack) → free PSRAM: %u KB, free SRAM: %u KB",
             (unsigned)ctx->width, (unsigned)ctx->height,
             (unsigned)(ctx->width * ctx->height * 4 / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
}

inline void lottie_screen_loaded_cb(lv_event_t *e) {
    LottieContext *ctx = (LottieContext *)lv_event_get_user_data(e);
    if (ctx->pixel_buffer == nullptr) {
        lottie_launch(ctx);
    }
}

// --------------------------------------------------------------------------
// Public API: Restart animation from frame 0 (preserves loop/hidden state)
// Safe to call at any time – sets a flag checked by the render loop.
// --------------------------------------------------------------------------
inline void lottie_restart(LottieContext *ctx) {
    if (ctx && ctx->task_handle) {
        ctx->restart_requested = true;
        ESP_LOGI(LOTTIE_TAG, "Restart requested (will reset on next frame)");
    }
}

// --------------------------------------------------------------------------
// Public API: initialise Lottie widget – allocate buffer, register screen
// events, and launch the load/render task.
// Call under lv_lock (from LVGL init code).
// --------------------------------------------------------------------------
inline bool lottie_init(lv_obj_t *obj, const void *data, size_t data_size,
                         const char *file_path, uint32_t width, uint32_t height,
                         bool loop, bool auto_start, bool user_wants_hidden) {
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
    ctx->user_wants_hidden = user_wants_hidden;  // Save user's 'hidden' config from YAML
    ctx->runtime_hidden = user_wants_hidden;    // Initially matches YAML config

    // Static binary semaphore used to wait for clean task shutdown without
    // ever calling vTaskDelete() from another task. Stored inside the
    // LottieContext so it lives as long as the widget itself.
    ctx->exit_sem = xSemaphoreCreateBinaryStatic(&ctx->exit_sem_storage);

    // Store context on the LVGL object so user scripts can retrieve it
    // via lv_obj_get_user_data() for lottie_restart() calls
    lv_obj_set_user_data(obj, ctx);

    // Register screen events for PSRAM lifecycle (two-phase unload).
    // IMPORTANT: We do NOT call lottie_launch() here.  Resources are only
    // allocated when the page actually becomes visible (SCREEN_LOADED event).
    // This prevents non-active pages from consuming PSRAM at startup.
    // With 19+ widgets across multiple pages, this saves megabytes of PSRAM.
    lv_obj_t *screen = lv_obj_get_screen(obj);
    lv_obj_add_event_cb(screen, lottie_screen_unload_start_cb,
                        LV_EVENT_SCREEN_UNLOAD_START, ctx);
    lv_obj_add_event_cb(screen, lottie_screen_unloaded_cb,
                        LV_EVENT_SCREEN_UNLOADED, ctx);
    lv_obj_add_event_cb(screen, lottie_screen_loaded_cb,
                        LV_EVENT_SCREEN_LOADED, ctx);

    ESP_LOGI(LOTTIE_TAG, "Lottie registered (%ux%u), waiting for page load to allocate",
             (unsigned)width, (unsigned)height);
    return true;
}

}  // namespace lvgl
}  // namespace esphome

#endif  // LV_USE_LOTTIE
#endif  // USE_ESP32











