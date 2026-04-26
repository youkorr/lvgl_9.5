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

// --------------------------------------------------------------------------
// Global mutex that serialises FIRST-load parsing across Lottie widgets.
//
// On a page like the 10-widget weather panel, all tasks wake up after their
// initial vTaskDelay() at roughly the same instant and race against each
// other for two non-thread-safe global resources:
//   1. SD card / VFS – simultaneous fopen()/fread() from several tasks have
//      been observed to crash the driver with a NULL+offset store fault.
//   2. ThorVG engine init – the first lv_lottie_set_src_*() call ever made
//      transitively initialises tvg_engine_init() once and pushes shared
//      paint state; concurrent first-init has caused random crashes in the
//      past.
//
// lv_lock() does NOT cover this for us in practice: lv_lottie_set_src_file()
// is free to release the LVGL lock around its file I/O. A dedicated static
// mutex makes the first-load parse path strictly sequential.
// --------------------------------------------------------------------------
inline SemaphoreHandle_t lottie_first_load_mutex() {
    static StaticSemaphore_t storage;
    static SemaphoreHandle_t handle = xSemaphoreCreateMutexStatic(&storage);
    return handle;
}

// Atomic counter used to stagger the initial vTaskDelay across widgets so
// they do not all hit the lv_lock + parse path at the same scheduler tick.
inline uint32_t lottie_next_load_slot() {
    static volatile uint32_t counter = 0;
    return __atomic_fetch_add(&counter, 1, __ATOMIC_SEQ_CST);
}

// --------------------------------------------------------------------------
// LottieFiles "Interactivity" markers — each segment has a name plus a
// frame range. Populated either at build time from the YAML config or
// parsed at first load from the JSON's `markers` array. Drives the
// play_marker / on_marker_complete API.
// --------------------------------------------------------------------------
struct LottieMarker {
    const char *name;     // string literal: "idle", "yes", "thinking", ...
    int32_t start_frame;  // inclusive
    int32_t end_frame;    // exclusive (start + duration)
};

// Callback invoked from the render task right after an animation segment
// completes (only fires when the segment is being played in one-shot mode,
// i.e. loop=false). Implements the *Complete events of the LottieFiles
// Interactivity vocabulary.
using LottieMarkerCompleteCb = void (*)(void *user_arg, const char *marker);

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
    int32_t start_frame;        // current segment start (modified by play_marker)
    int32_t end_frame;          // current segment end   (modified by play_marker)
    int32_t native_start_frame; // full-animation start (frame 0)
    int32_t native_end_frame;   // full-animation end   (last frame)
    uint32_t duration_ms;       // duration of the FULL animation in ms
    bool data_loaded;           // true after first successful parse

    // --- Markers ("Interactivity" segments) ---
    const LottieMarker *markers;
    uint8_t marker_count;
    int8_t  active_marker;       // index into markers[], -1 if playing full anim
    const char *initial_marker;  // optional name selected at first launch
    volatile bool one_shot;      // when true, fire completion cb after one cycle
    LottieMarkerCompleteCb on_complete;
    void *on_complete_arg;
    // Completion event is dispatched via lv_async_call so the YAML script
    // runs on the LVGL main task rather than from inside the render task.
    volatile const char *pending_complete_marker;

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

    // --- Non-blocking cleanup (poll task exit from an LVGL timer) ---
    // The screen-unload callback must NOT block the LVGL main loop, otherwise
    // touch input becomes laggy when leaving a page with several Lottie
    // widgets (e.g. the 10-widget weather page). Instead we kick off an
    // lv_timer that polls exit_sem every few ms and frees memory only once
    // the task has actually exited.
    lv_timer_t *cleanup_timer;
    uint16_t cleanup_attempts;
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

    // Wait for LVGL to be ready, plus a per-widget stagger on first load so
    // 10 tasks don't race against each other (and the SD card driver) for
    // the parsing path.
    if (!ctx->data_loaded) {
        uint32_t slot = lottie_next_load_slot();
        vTaskDelay(pdMS_TO_TICKS(800 + slot * 150));
    } else {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    // Bail out early if a stop was requested before we even started: the
    // screen could have been swapped right after launch (e.g. boot animation
    // racing with the auto-lock timer).
    if (ctx->stop_requested) {
        ESP_LOGI(LOTTIE_TAG, "Stop requested before init, exiting");
        lottie_task_exit(ctx);
    }

    // Serialise first-load parsing globally. SD/VFS and ThorVG's first-time
    // init are not safe to invoke from several tasks concurrently, even
    // through lv_lottie_set_src_file() (which may release lv_lock around its
    // file I/O). Re-loads do not contend on this – they hit the cached
    // ThorVG state and only need lv_lock for the LVGL-side bookkeeping.
    SemaphoreHandle_t parse_mtx = ctx->data_loaded ? nullptr
                                                    : lottie_first_load_mutex();
    if (parse_mtx) {
        xSemaphoreTake(parse_mtx, portMAX_DELAY);
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
            ctx->native_start_frame = anim->start_value;
            ctx->native_end_frame   = anim->end_value;
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

            // If the YAML specified an `initial_marker`, narrow the segment
            // range to that marker right now so auto_start plays only that
            // state instead of the full timeline (which would visibly cycle
            // through all 6 LottieFiles "Interactivity" states – idle, yes,
            // no, alert, thinking, jump – before the voice assistant ever
            // sends its first play_marker call).
            if (ctx->initial_marker != nullptr && ctx->initial_marker[0] != '\0' &&
                ctx->markers != nullptr && ctx->marker_count > 0) {
                for (uint8_t i = 0; i < ctx->marker_count; i++) {
                    if (strcmp(ctx->markers[i].name, ctx->initial_marker) == 0) {
                        ctx->start_frame   = ctx->markers[i].start_frame;
                        ctx->end_frame     = ctx->markers[i].end_frame;
                        ctx->active_marker = (int8_t)i;
                        ESP_LOGI(LOTTIE_TAG, "Initial marker '%s' (%d..%d)",
                                 ctx->initial_marker,
                                 (int)ctx->start_frame, (int)ctx->end_frame);
                        break;
                    }
                }
            }
        } else {
            ESP_LOGE(LOTTIE_TAG, "Animation INVALID – parsing may have failed!");
        }
    } else {
        // ===== RE-LOAD (screen came back) =====
        // We keep the pixel buffer, the lv_lottie widget and its ThorVG
        // canvas/picture alive across screen unloads (see lottie_free_*
        // below). So a re-load is just: resume the render loop. ThorVG
        // already has everything it needs and any attempt to "reset" or
        // re-attach the buffer/picture has been observed to crash with a
        // Store access fault on a NULL+0x30 deref – ThorVG's reference
        // graph is not designed for re-targeting once it's running.
        //
        // This mirrors the Espressif thorvg_lottie example pattern: never
        // mutate live ThorVG state, just keep playing frames.
        ESP_LOGI(LOTTIE_TAG, "Re-load: resuming render loop on cached state");

        // Make sure LVGL repaints the widget area even if the first
        // exec_cb tick hasn't landed yet on the next display refresh.
        lv_obj_invalidate(ctx->obj);
    }

    // Restore visibility: use runtime_hidden which captures the actual state
    // before page unload (preserves dynamic show/hide from user scripts).
    // On first load, runtime_hidden == user_wants_hidden (from YAML config).
    if (!ctx->runtime_hidden) {
        lv_obj_remove_flag(ctx->obj, LV_OBJ_FLAG_HIDDEN);
    }

    lv_unlock();

    // Release the global first-load gate so the next widget can parse.
    if (parse_mtx) {
        xSemaphoreGive(parse_mtx);
    }

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
    // The frame delay is computed from the FULL-animation duration so that
    // every segment plays at its native speed regardless of which marker is
    // currently active.
    int32_t native_total = ctx->native_end_frame - ctx->native_start_frame;
    if (native_total <= 0) native_total = 1;
    uint32_t frame_delay_ms = ctx->duration_ms / (uint32_t)native_total;
    if (frame_delay_ms < 16)  frame_delay_ms = 16;
    if (frame_delay_ms > 100) frame_delay_ms = 100;

    ESP_LOGI(LOTTIE_TAG, "Render loop: %u ms/frame, loop=%d",
             (unsigned)frame_delay_ms, (int)ctx->loop);

    ctx->start_tick = xTaskGetTickCount();  // ✅ Store in context for restart capability

    while (!ctx->stop_requested) {
        // Snapshot the segment range up-front so a play_marker() call
        // mid-cycle is picked up cleanly on the next iteration without
        // tearing the maths.
        int32_t seg_start = ctx->start_frame;
        int32_t seg_end   = ctx->end_frame;
        int32_t seg_total = seg_end - seg_start;
        if (seg_total <= 0) seg_total = 1;
        uint32_t seg_duration_ms = (uint32_t)seg_total * frame_delay_ms;
        if (seg_duration_ms == 0) seg_duration_ms = 1;
        bool seg_loop = ctx->loop && !ctx->one_shot;

        if (ctx->restart_requested) {
            ctx->start_tick = xTaskGetTickCount();
            ctx->restart_requested = false;
            ESP_LOGI(LOTTIE_TAG, "Segment restarted (%d..%d, loop=%d)",
                     (int)seg_start, (int)seg_end, (int)seg_loop);
        }

        uint32_t elapsed_ms = (uint32_t)((xTaskGetTickCount() - ctx->start_tick) * portTICK_PERIOD_MS);

        int32_t frame;
        bool segment_complete = false;
        if (seg_loop) {
            uint32_t phase = elapsed_ms % seg_duration_ms;
            frame = seg_start + (int32_t)((int64_t)seg_total * phase / seg_duration_ms);
        } else {
            if (elapsed_ms >= seg_duration_ms) {
                frame = seg_end;
                segment_complete = true;
            } else {
                frame = seg_start + (int32_t)((int64_t)seg_total * elapsed_ms / seg_duration_ms);
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

        if (segment_complete) {
            const char *marker_name = (ctx->active_marker >= 0 &&
                                       ctx->active_marker < (int8_t)ctx->marker_count)
                                       ? ctx->markers[ctx->active_marker].name
                                       : "complete";
            ESP_LOGI(LOTTIE_TAG, "Segment '%s' complete", marker_name);

            // Hand the completion event off to the LVGL main task (lv_async)
            // so the YAML on_marker_complete trigger runs in the right
            // context and does not stall this render task.
            if (ctx->on_complete != nullptr) {
                ctx->pending_complete_marker = marker_name;
                lv_async_call([](void *arg) {
                    auto *c = (LottieContext *)arg;
                    if (c->on_complete != nullptr && c->pending_complete_marker != nullptr) {
                        const char *mn = (const char *)c->pending_complete_marker;
                        c->pending_complete_marker = nullptr;
                        c->on_complete(c->on_complete_arg, mn);
                    }
                }, ctx);
            }

            if (ctx->one_shot) {
                // One-shot done: leave the last frame visible and freeze the
                // task on a long sleep until play_marker() restarts us.
                ctx->one_shot = false;
                while (!ctx->stop_requested && !ctx->restart_requested) {
                    vTaskDelay(pdMS_TO_TICKS(50));
                }
                continue;
            }
            // Not one-shot, not loop → exit the render loop entirely.
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
// Free the render-task stack and TCB. Caller MUST have ensured the render
// task is no longer running (semaphore taken or vTaskDelete acknowledged).
//
// IMPORTANT: we deliberately do NOT free pixel_buffer here. The pixel buffer
// is owned by the lv_lottie widget's ThorVG canvas (lv_lottie_set_buffer
// hands it to ThorVG). Freeing it while the canvas still references it –
// then re-allocating + re-attaching it on the next screen load – has been
// observed to crash ThorVG with a Store access fault on a NULL+0x30 deref
// (the same kind of failure reported by users when navigating back to the
// home page that has 10 weather Lottie widgets).
//
// Mirroring the Espressif thorvg_lottie example pattern: never mutate live
// ThorVG state. The buffer + canvas + picture stay alive for the lifetime
// of the widget. Only the (large) task stack is reclaimed when the page is
// not visible, which is where the real PSRAM pressure was anyway.
// --------------------------------------------------------------------------
inline void lottie_free_buffers(LottieContext *ctx) {
    if (ctx->task_stack)    { heap_caps_free(ctx->task_stack);    ctx->task_stack = nullptr; }
    if (ctx->task_tcb)      { heap_caps_free(ctx->task_tcb);      ctx->task_tcb = nullptr; }
    // pixel_buffer intentionally retained – see header comment above.
    ctx->stop_requested = false;
    ctx->task_handle = nullptr;

    ESP_LOGI(LOTTIE_TAG, "Lottie task freed (%ux%u, 64 KB stack) → free PSRAM: %u KB, free SRAM: %u KB",
             (unsigned)ctx->width, (unsigned)ctx->height,
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
}

// --------------------------------------------------------------------------
// Synchronous wait + free. Only used as a fallback (e.g. when the screen is
// being reloaded while a previous cleanup is still pending). Normal unloads
// go through the asynchronous lv_timer path so they never block the UI.
// --------------------------------------------------------------------------
inline void lottie_free_resources(LottieContext *ctx) {
    ctx->stop_requested = true;
    if (ctx->task_handle) {
        bool clean = (ctx->exit_sem &&
                      xSemaphoreTake(ctx->exit_sem, pdMS_TO_TICKS(300)) == pdTRUE);
        if (!clean) {
            ESP_LOGW(LOTTIE_TAG, "Render task did not exit in 300ms – force deleting");
            vTaskDelete(ctx->task_handle);
            vTaskDelay(pdMS_TO_TICKS(20));
        } else {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
    lottie_free_buffers(ctx);
}

// --------------------------------------------------------------------------
// LVGL timer callback that periodically checks whether the render task has
// finished self-deleting. When it has, the buffers are freed and the timer
// removes itself. Runs from the LVGL main loop, so each tick is a quick
// non-blocking check – touch input stays responsive.
// --------------------------------------------------------------------------
inline void lottie_cleanup_timer_cb(lv_timer_t *t) {
    LottieContext *ctx = (LottieContext *)lv_timer_get_user_data(t);

    bool task_exited = (ctx->task_handle == nullptr);
    if (!task_exited && ctx->exit_sem &&
        xSemaphoreTake(ctx->exit_sem, 0) == pdTRUE) {
        task_exited = true;
    }

    if (task_exited) {
        lottie_free_buffers(ctx);
        lv_timer_delete(t);
        ctx->cleanup_timer = nullptr;
        return;
    }

    // Bail out after ~1 s to avoid leaking memory if something went wrong.
    // 1 s is well above the maximum expected exit time (one frame_delay_ms,
    // capped at 100 ms, plus a few ms of teardown).
    if (++ctx->cleanup_attempts >= 40) {  // 40 × 25 ms = 1 s
        ESP_LOGW(LOTTIE_TAG, "Render task still alive after 1 s – force deleting");
        if (ctx->task_handle) {
            vTaskDelete(ctx->task_handle);
        }
        lottie_free_buffers(ctx);
        lv_timer_delete(t);
        ctx->cleanup_timer = nullptr;
    }
    // Otherwise let the timer keep polling.
}

// --------------------------------------------------------------------------
// Allocate pixel buffer (first call only) and launch the render task.
// lv_lottie_set_buffer is NOT called here – it is called inside the task
// because it triggers ThorVG rendering which needs the 64 KB stack.
//
// On re-loads only the task stack is re-allocated. The pixel buffer (and
// the lv_lottie / ThorVG canvas/picture references it) is kept alive for
// the lifetime of the widget – see lottie_free_buffers().
// --------------------------------------------------------------------------
inline bool lottie_launch(LottieContext *ctx) {
    // NOTE: Do NOT re-capture runtime_hidden here!
    // On re-loads the widget is already hidden (by lottie_screen_unload_start_cb),
    // so reading LV_OBJ_FLAG_HIDDEN would always return true, losing the real
    // visibility state that was correctly saved in the unload callback.
    // runtime_hidden is set by:
    //   - lottie_init()                      → first load (from YAML config)
    //   - lottie_screen_unload_start_cb()    → re-loads  (actual state before hide)

    // Allocate pixel buffer in PSRAM only on first launch. On re-loads we
    // reuse the existing buffer: ThorVG's canvas and parsed picture still
    // point at it, and re-allocating + re-attaching is exactly the path
    // that crashes ThorVG with NULL+0x30.
    size_t buf_bytes = (size_t)ctx->width * ctx->height * 4;
    if (ctx->pixel_buffer == nullptr) {
        ctx->pixel_buffer = (uint8_t *)heap_caps_malloc(
            buf_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!ctx->pixel_buffer) {
            ESP_LOGE(LOTTIE_TAG, "PSRAM alloc failed (%u bytes)", (unsigned)buf_bytes);
            return false;
        }
        memset(ctx->pixel_buffer, 0, buf_bytes);
    }

    // Hide temporarily during async load (pixel buffer is blank or stale)
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

    // Don't block the LVGL main loop here. Schedule an lv_timer that will
    // free the task stack/TCB as soon as the render task has self-deleted.
    // Polling happens between LVGL frames so touch input keeps responding
    // even when 10 Lottie widgets unload at once (e.g. on auto-lock).
    if (ctx->cleanup_timer != nullptr) {
        // A previous cleanup is somehow still pending – let it finish.
        return;
    }
    if (ctx->task_handle == nullptr) {
        // Task already gone, nothing to clean up.
        return;
    }

    ctx->cleanup_attempts = 0;
    ctx->cleanup_timer = lv_timer_create(lottie_cleanup_timer_cb, 25, ctx);
    ESP_LOGI(LOTTIE_TAG, "Lottie cleanup scheduled (non-blocking)");
}

inline void lottie_screen_loaded_cb(lv_event_t *e) {
    LottieContext *ctx = (LottieContext *)lv_event_get_user_data(e);

    // Rare: user came back to the page before the deferred cleanup fired.
    // Finish it synchronously (still bounded by a short timeout) so the
    // re-launch starts from a clean slate.
    if (ctx->cleanup_timer != nullptr) {
        lv_timer_delete(ctx->cleanup_timer);
        ctx->cleanup_timer = nullptr;
        lottie_free_resources(ctx);
    }

    // Relaunch the task whenever it's gone. The pixel buffer / lv_lottie /
    // ThorVG state is kept alive across screen unloads, so the relaunched
    // task only needs to resume the render loop – no buffer or canvas
    // re-initialisation that could crash ThorVG.
    if (ctx->task_handle == nullptr) {
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
// Public API: Play a named marker (LottieFiles "Interactivity" *Click event).
//
// Looks the marker name up in the context's marker table, swaps the active
// segment range and triggers a restart from the first frame of that segment.
// When `one_shot=true` the segment plays once and then fires the
// on_marker_complete callback (the LottieFiles *Complete event) — perfect for
// transient states like "yes" / "no". When false, the segment loops — perfect
// for sustained states like "idle" / "thinking" / "alert".
//
// Safe to call from any LVGL task; only sets atomic flags read by the
// render task.
// --------------------------------------------------------------------------
inline bool lottie_play_marker(LottieContext *ctx, const char *name, bool one_shot) {
    if (!ctx || !ctx->task_handle || !name) return false;
    for (uint8_t i = 0; i < ctx->marker_count; i++) {
        if (strcmp(ctx->markers[i].name, name) == 0) {
            ctx->start_frame  = ctx->markers[i].start_frame;
            ctx->end_frame    = ctx->markers[i].end_frame;
            ctx->active_marker = (int8_t)i;
            ctx->one_shot      = one_shot;
            ctx->restart_requested = true;
            ESP_LOGI(LOTTIE_TAG, "play_marker '%s' (%d..%d, one_shot=%d)",
                     name, (int)ctx->start_frame, (int)ctx->end_frame, (int)one_shot);
            return true;
        }
    }
    ESP_LOGW(LOTTIE_TAG, "play_marker: unknown marker '%s'", name);
    return false;
}

// --------------------------------------------------------------------------
// Public API: Register a callback that fires once a one-shot segment has
// finished playing (LottieFiles "Interactivity" *Complete event). Pass
// nullptr to unregister.
// --------------------------------------------------------------------------
inline void lottie_set_on_complete(LottieContext *ctx,
                                    LottieMarkerCompleteCb cb,
                                    void *user_arg) {
    if (!ctx) return;
    ctx->on_complete     = cb;
    ctx->on_complete_arg = user_arg;
}

// --------------------------------------------------------------------------
// Public API: initialise Lottie widget – allocate buffer, register screen
// events, and launch the load/render task.
// Call under lv_lock (from LVGL init code).
// --------------------------------------------------------------------------
inline bool lottie_init(lv_obj_t *obj, const void *data, size_t data_size,
                         const char *file_path, uint32_t width, uint32_t height,
                         bool loop, bool auto_start, bool user_wants_hidden,
                         const LottieMarker *markers = nullptr,
                         uint8_t marker_count = 0,
                         const char *initial_marker = nullptr) {
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
    ctx->markers        = markers;
    ctx->marker_count   = marker_count;
    ctx->initial_marker = initial_marker;
    ctx->active_marker  = -1;

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











