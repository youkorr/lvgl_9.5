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

// Lottie loader task parameters
struct LottieLoadParams {
    lv_obj_t *obj;
    const void *data;
    size_t data_size;
    const char *file_path;
};

// Stack size for ThorVG parsing (48KB to be safe)
static constexpr size_t LOTTIE_TASK_STACK_SIZE = 48 * 1024;

// Task function that loads lottie data with large PSRAM stack
inline void lottie_load_task(void *param) {
    LottieLoadParams *p = (LottieLoadParams *)param;

    // Wait for LVGL loop to be fully running and stable
    vTaskDelay(pdMS_TO_TICKS(1000));

    ESP_LOGI(LOTTIE_TAG, "Loading lottie animation data...");

    // Load source data or file (ThorVG parsing happens here - needs large stack)
    if (p->data != nullptr) {
        lv_lottie_set_src_data(p->obj, p->data, p->data_size);
        ESP_LOGI(LOTTIE_TAG, "Lottie data loaded from embedded source (size: %d bytes)", p->data_size);
    } else if (p->file_path != nullptr) {
        lv_lottie_set_src_file(p->obj, p->file_path);
        ESP_LOGI(LOTTIE_TAG, "Lottie data loaded from file: %s", p->file_path);
    }

    // Diagnostic: Check if animation was parsed correctly
    lv_anim_t *anim = lv_lottie_get_anim(p->obj);
    if (anim != nullptr) {
        ESP_LOGI(LOTTIE_TAG, "Animation VALID - ready to play");
        ESP_LOGI(LOTTIE_TAG, "  Animation duration: %d ms", lv_anim_get_time(anim));
    } else {
        ESP_LOGE(LOTTIE_TAG, "Animation INVALID - parsing may have failed!");
    }

    // Check buffer
    lv_image_dsc_t *img = lv_canvas_get_image(p->obj);
    if (img != nullptr) {
        ESP_LOGI(LOTTIE_TAG, "  Canvas buffer: %dx%d, data=%p", img->header.w, img->header.h, img->data);
    } else {
        ESP_LOGW(LOTTIE_TAG, "  Canvas buffer: NOT SET");
    }

    // Free the params struct
    heap_caps_free(param);

    ESP_LOGI(LOTTIE_TAG, "Lottie load task complete, deleting task");

    // Delete this task
    vTaskDelete(NULL);
}

// Function to start lottie loading in a task with PSRAM stack
inline bool lottie_load_async(lv_obj_t *obj, const void *data, size_t data_size, const char *file_path) {
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

    ESP_LOGI(LOTTIE_TAG, "Creating lottie load task with %d KB PSRAM stack", LOTTIE_TASK_STACK_SIZE / 1024);

    // Create task with static allocation (stack in PSRAM)
    TaskHandle_t task = xTaskCreateStatic(
        lottie_load_task,
        "lottie_load",
        LOTTIE_TASK_STACK_SIZE / sizeof(StackType_t),
        params,
        5,  // Priority
        stack,
        tcb
    );

    if (task == nullptr) {
        ESP_LOGE(LOTTIE_TAG, "Failed to create lottie load task");
        heap_caps_free(params);
        heap_caps_free(stack);
        heap_caps_free(tcb);
        return false;
    }

    // Note: stack and tcb will leak when task completes - acceptable for one-shot loading
    return true;
}

}  // namespace lvgl
}  // namespace esphome

#endif  // USE_ESP32
