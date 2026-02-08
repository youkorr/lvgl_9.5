#pragma once

#ifdef USE_ESP32

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <lvgl.h>


namespace esphome {
namespace lvgl {

static const char *const LOTTIE_TAG = "lottie";

// Lottie loader + render task parameters
struct LottieLoadParams {
    lv_obj_t *obj;
    const void *data;
    size_t data_size;
    const char *file_path;
    bool loop;
    bool auto_start;
};

// Stack size for ThorVG parsing AND rendering (64KB in PSRAM)
static constexpr size_t LOTTIE_TASK_STACK_SIZE = 64 * 1024;

// Task that loads lottie data then drives frame rendering.
//
// LVGL's built-in animation calls lottie_update() from lv_timer_handler()
// which runs on the main task.  ThorVG's SW renderer (without thread support)
// executes all rasterisation synchronously on the caller's stack, needing
// 32 KB+.  The main task typically has only 8-16 KB → stack overflow → crash.
//
// Solution: after loading, we DELETE the LVGL animation and drive the frame
// updates ourselves from this task which has a 64 KB PSRAM stack.
inline void lottie_load_task(void *param) {
    LottieLoadParams *p = (LottieLoadParams *)param;

    // Wait for LVGL loop to be fully running and stable
    vTaskDelay(pdMS_TO_TICKS(1000));

    ESP_LOGI(LOTTIE_TAG, "Loading lottie animation data...");

    // --- PHASE 1: Load data (ThorVG JSON parsing – needs large stack) ---
    lv_lock();

    if (p->data != nullptr) {
        lv_lottie_set_src_data(p->obj, p->data, p->data_size);
        ESP_LOGI(LOTTIE_TAG, "Lottie data loaded from embedded source (%d bytes)", (int)p->data_size);
    } else if (p->file_path != nullptr) {
        lv_lottie_set_src_file(p->obj, p->file_path);
        ESP_LOGI(LOTTIE_TAG, "Lottie data loaded from file: %s", p->file_path);
    }

    // --- PHASE 2: Capture animation parameters ---
    lv_anim_t *anim = lv_lottie_get_anim(p->obj);

    lv_anim_exec_xcb_t exec_cb = nullptr;
    void *anim_var = nullptr;
    int32_t start_frame = 0;
    int32_t end_frame = 0;
    uint32_t duration_ms = 0;

    if (anim != nullptr) {
        // Read animation parameters set by lv_lottie_set_src_data()
        exec_cb = anim->exec_cb;
        anim_var = anim->var;
        start_frame = anim->start_value;
        end_frame = anim->end_value;
        duration_ms = (uint32_t)lv_anim_get_time(anim);

        ESP_LOGI(LOTTIE_TAG, "Animation: frames %d..%d, duration %u ms",
                 (int)start_frame, (int)end_frame, (unsigned)duration_ms);

        // DELETE the LVGL animation so it will NOT fire on the main task.
        // This is the key fix: lottie_update() (called by the exec_cb) does
        // synchronous ThorVG rendering which needs >>16 KB of stack.
        lv_anim_delete(anim_var, exec_cb);
        ESP_LOGI(LOTTIE_TAG, "LVGL animation removed – rendering from PSRAM task");
    } else {
        ESP_LOGE(LOTTIE_TAG, "Animation INVALID – parsing may have failed!");
    }

    // Show the widget now that data is loaded
    lv_obj_remove_flag(p->obj, LV_OBJ_FLAG_HIDDEN);

    lv_unlock();

    // Copy params we need for the render loop, then free the struct
    bool loop = p->loop;
    bool auto_start = p->auto_start;
    lv_obj_t *obj = p->obj;
    heap_caps_free(param);

    // If parsing failed or auto_start is off, we're done
    if (exec_cb == nullptr || duration_ms == 0 || end_frame <= start_frame) {
        ESP_LOGW(LOTTIE_TAG, "No valid animation to render, task exiting");
        vTaskDelete(NULL);
        return;
    }

    if (!auto_start) {
        ESP_LOGI(LOTTIE_TAG, "auto_start=false, render task exiting");
        vTaskDelete(NULL);
        return;
    }

    // --- PHASE 3: Frame render loop (runs on this 64 KB PSRAM stack) ---
    // Calculate per-frame delay from animation metadata
    int32_t total_frames = end_frame - start_frame;
    uint32_t frame_delay_ms = duration_ms / (uint32_t)total_frames;
    if (frame_delay_ms < 16)  frame_delay_ms = 16;   // cap ~60 fps
    if (frame_delay_ms > 100) frame_delay_ms = 100;   // floor 10 fps

    ESP_LOGI(LOTTIE_TAG, "Render loop: %d ms/frame, loop=%d", (int)frame_delay_ms, (int)loop);

    TickType_t start_tick = xTaskGetTickCount();

    while (true) {
        uint32_t elapsed_ms = (uint32_t)((xTaskGetTickCount() - start_tick) * portTICK_PERIOD_MS);

        int32_t frame;
        if (loop) {
            uint32_t phase = elapsed_ms % duration_ms;
            frame = start_frame + (int32_t)((int64_t)total_frames * phase / duration_ms);
        } else {
            if (elapsed_ms >= duration_ms) {
                // One-shot complete – render last frame and stop
                lv_lock();
                exec_cb(anim_var, end_frame);
                lv_unlock();
                ESP_LOGI(LOTTIE_TAG, "Animation complete");
                break;
            }
            frame = start_frame + (int32_t)((int64_t)total_frames * elapsed_ms / duration_ms);
        }

        // exec_cb is LVGL's internal anim_exec_cb which calls lottie_update()
        // → tvg_canvas_update / draw / sync.  All heavy ThorVG work runs HERE
        // on our 64 KB PSRAM stack instead of the tiny main-task stack.
        lv_lock();
        exec_cb(anim_var, frame);
        lv_unlock();

        vTaskDelay(pdMS_TO_TICKS(frame_delay_ms));
    }

    vTaskDelete(NULL);
}

// Start lottie loading (and later rendering) in a task with PSRAM stack.
inline bool lottie_load_async(lv_obj_t *obj, const void *data, size_t data_size,
                              const char *file_path, bool loop, bool auto_start) {
    // Allocate params in PSRAM
    LottieLoadParams *params = (LottieLoadParams *)heap_caps_malloc(
        sizeof(LottieLoadParams), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (params == nullptr) {
        ESP_LOGE(LOTTIE_TAG, "Failed to allocate params in PSRAM");
        return false;
    }

    params->obj = obj;
    params->data = data;
    params->data_size = data_size;
    params->file_path = file_path;
    params->loop = loop;
    params->auto_start = auto_start;

    // Allocate stack in PSRAM
    StackType_t *stack = (StackType_t *)heap_caps_malloc(
        LOTTIE_TASK_STACK_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (stack == nullptr) {
        ESP_LOGE(LOTTIE_TAG, "Failed to allocate %d bytes stack in PSRAM", LOTTIE_TASK_STACK_SIZE);
        heap_caps_free(params);
        return false;
    }

    // Allocate task control block in internal RAM (required by FreeRTOS)
    StaticTask_t *tcb = (StaticTask_t *)heap_caps_malloc(
        sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (tcb == nullptr) {
        ESP_LOGE(LOTTIE_TAG, "Failed to allocate TCB in internal RAM");
        heap_caps_free(params);
        heap_caps_free(stack);
        return false;
    }

    ESP_LOGI(LOTTIE_TAG, "Creating lottie task with %d KB PSRAM stack", LOTTIE_TASK_STACK_SIZE / 1024);

    TaskHandle_t task = xTaskCreateStatic(
        lottie_load_task,
        "lottie_anim",
        LOTTIE_TASK_STACK_SIZE / sizeof(StackType_t),
        params,
        5,  // Priority
        stack,
        tcb
    );

    if (task == nullptr) {
        ESP_LOGE(LOTTIE_TAG, "Failed to create lottie task");
        heap_caps_free(params);
        heap_caps_free(stack);
        heap_caps_free(tcb);
        return false;
    }

    // Note: stack and tcb persist for the lifetime of the render loop
    return true;
}

}  // namespace lvgl
}  // namespace esphome

#endif  // USE_ESP32
