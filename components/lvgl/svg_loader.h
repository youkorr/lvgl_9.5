#pragma once

#ifdef USE_ESP32

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <cstdio>
#include <lvgl.h>

// ThorVG C API – compiled into LVGL when LV_USE_THORVG_INTERNAL=1.
// The header lives inside LVGL's source tree; the LVGL PlatformIO library
// root is on the include path (same reason <lvgl.h> works).
#include <src/libs/thorvg/thorvg_capi.h>

namespace esphome {
namespace lvgl {

static const char *const SVG_TAG = "svg";

// Parameters passed to the one-shot SVG render task.
struct SvgRenderParams {
    lv_obj_t *canvas_obj;     // lv_canvas widget to display the result
    const char *svg_data;     // SVG XML data (for embedded; NULL if file-based)
    size_t svg_data_size;     // Length of svg_data (excl. null-terminator)
    const char *file_path;    // Filesystem path (for src; NULL if embedded)
    uint32_t width;           // Target render width in pixels
    uint32_t height;          // Target render height in pixels
    uint32_t *pixel_buffer;   // ARGB8888 buffer in PSRAM (width * height * 4 bytes)
};

// 64 KB stack in PSRAM – ThorVG SW rasterisation needs 32 KB+.
static constexpr size_t SVG_TASK_STACK_SIZE = 64 * 1024;

// --------------------------------------------------------------------------
// FreeRTOS task: render SVG via ThorVG into the pixel buffer, then unhide
// the LVGL canvas widget.  Runs once and self-deletes.
// --------------------------------------------------------------------------
inline void svg_render_task(void *param) {
    SvgRenderParams *p = (SvgRenderParams *)param;

    // Let LVGL settle after boot / page load.
    vTaskDelay(pdMS_TO_TICKS(500));

    // --- Resolve SVG data (embedded or read from filesystem) ---
    const char *svg_data = p->svg_data;
    size_t svg_data_size = p->svg_data_size;
    char *file_buf = nullptr;  // non-null if we allocated it (must free later)

    if (svg_data == nullptr && p->file_path != nullptr) {
        // Read SVG from filesystem (SD card / LittleFS / SPIFFS)
        ESP_LOGI(SVG_TAG, "Reading SVG from %s ...", p->file_path);
        FILE *f = fopen(p->file_path, "r");
        if (!f) {
            ESP_LOGE(SVG_TAG, "Cannot open SVG file: %s", p->file_path);
            goto finish;
        }
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz <= 0) {
            ESP_LOGE(SVG_TAG, "SVG file is empty: %s", p->file_path);
            fclose(f);
            goto finish;
        }
        file_buf = (char *)heap_caps_malloc(sz + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!file_buf) {
            ESP_LOGE(SVG_TAG, "Failed to allocate %ld bytes for SVG file", sz);
            fclose(f);
            goto finish;
        }
        size_t nread = fread(file_buf, 1, sz, f);
        fclose(f);
        file_buf[nread] = '\0';
        svg_data = file_buf;
        svg_data_size = nread;
        ESP_LOGI(SVG_TAG, "Read %u bytes from %s", (unsigned)nread, p->file_path);
    }

    if (!svg_data || svg_data_size == 0) {
        ESP_LOGE(SVG_TAG, "No SVG data to render");
        goto finish;
    }

    ESP_LOGI(SVG_TAG, "Rendering SVG (%u bytes) to %ux%u buffer...",
             (unsigned)svg_data_size, (unsigned)p->width, (unsigned)p->height);

    // Clear buffer to fully transparent black.
    memset(p->pixel_buffer, 0, p->width * p->height * sizeof(uint32_t));

    {
        bool ok = false;

        // ThorVG engine init is ref-counted – safe to call if already initialised.
        tvg_engine_init(TVG_ENGINE_SW, 0);

        Tvg_Canvas *tvg_canvas = tvg_swcanvas_create();
        if (!tvg_canvas) {
            ESP_LOGE(SVG_TAG, "Failed to create ThorVG SW canvas");
            goto finish;
        }

        if (tvg_swcanvas_set_target(tvg_canvas, p->pixel_buffer, p->width,
                                     p->width, p->height,
                                     TVG_COLORSPACE_ARGB8888) != TVG_RESULT_SUCCESS) {
            ESP_LOGE(SVG_TAG, "tvg_swcanvas_set_target failed");
            tvg_canvas_destroy(tvg_canvas);
            goto finish;
        }

        {
            // Load SVG data into a ThorVG picture.
            Tvg_Paint *picture = tvg_picture_new();
            if (!picture) {
                ESP_LOGE(SVG_TAG, "tvg_picture_new failed");
                tvg_canvas_destroy(tvg_canvas);
                goto finish;
            }

            Tvg_Result res = tvg_picture_load_data(
                picture, svg_data, (uint32_t)svg_data_size, "svg", true);
            if (res != TVG_RESULT_SUCCESS) {
                ESP_LOGE(SVG_TAG, "tvg_picture_load_data failed: %d", (int)res);
                tvg_paint_del(picture);
                tvg_canvas_destroy(tvg_canvas);
                goto finish;
            }

            float orig_w = 0, orig_h = 0;
            tvg_picture_get_size(picture, &orig_w, &orig_h);
            ESP_LOGI(SVG_TAG, "SVG intrinsic size: %.0fx%.0f  -> target %ux%u",
                     orig_w, orig_h, (unsigned)p->width, (unsigned)p->height);

            // Scale SVG to the target canvas size.
            tvg_picture_set_size(picture, (float)p->width, (float)p->height);

            // Push picture to canvas – canvas takes ownership of the paint.
            res = tvg_canvas_push(tvg_canvas, picture);
            if (res != TVG_RESULT_SUCCESS) {
                ESP_LOGE(SVG_TAG, "tvg_canvas_push failed: %d", (int)res);
                tvg_paint_del(picture);
                tvg_canvas_destroy(tvg_canvas);
                goto finish;
            }

            // Rasterise (synchronous because we initialised with 0 threads).
            res = tvg_canvas_draw(tvg_canvas);
            if (res != TVG_RESULT_SUCCESS) {
                ESP_LOGE(SVG_TAG, "tvg_canvas_draw failed: %d", (int)res);
                tvg_canvas_destroy(tvg_canvas);
                goto finish;
            }

            tvg_canvas_sync(tvg_canvas);
            ok = true;
            ESP_LOGI(SVG_TAG, "SVG rendered successfully (%ux%u)",
                     (unsigned)p->width, (unsigned)p->height);
        }

        tvg_canvas_destroy(tvg_canvas);

        if (!ok) {
            ESP_LOGW(SVG_TAG, "SVG rendering failed – canvas will be blank");
        }
    }

finish:
    // Free file buffer if we allocated one.
    if (file_buf) {
        heap_caps_free(file_buf);
    }

    // Show the canvas in LVGL (even on failure – user sees a blank area).
    lv_lock();
    lv_obj_remove_flag(p->canvas_obj, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(p->canvas_obj);
    lv_unlock();

    heap_caps_free(param);
    vTaskDelete(NULL);
}

// --------------------------------------------------------------------------
// Set up the LVGL canvas draw-buffer and launch the async render task.
//
// Call from the generated LVGL init code (under lv_lock).
// The canvas widget must already exist.
// --------------------------------------------------------------------------
inline bool svg_setup_and_render(lv_obj_t *canvas_obj,
                                  const char *svg_data, size_t svg_data_size,
                                  uint32_t width, uint32_t height) {
    // --- 1. Allocate ARGB8888 pixel buffer in PSRAM ---
    size_t buf_bytes = (size_t)width * height * sizeof(uint32_t);
    uint32_t *pixel_buf = (uint32_t *)heap_caps_malloc(
        buf_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pixel_buf) {
        ESP_LOGE(SVG_TAG, "Failed to allocate %u bytes in PSRAM for SVG", (unsigned)buf_bytes);
        return false;
    }
    memset(pixel_buf, 0, buf_bytes);

    // --- 2. Create LVGL draw-buf and attach to canvas ---
    lv_draw_buf_t *draw_buf = (lv_draw_buf_t *)heap_caps_malloc(
        sizeof(lv_draw_buf_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!draw_buf) {
        ESP_LOGE(SVG_TAG, "Failed to allocate draw_buf");
        heap_caps_free(pixel_buf);
        return false;
    }
    lv_draw_buf_init(draw_buf, width, height,
                     LV_COLOR_FORMAT_ARGB8888, 0,
                     pixel_buf, buf_bytes);
    lv_draw_buf_set_flag(draw_buf, LV_IMAGE_FLAGS_MODIFIABLE);
    lv_canvas_set_draw_buf(canvas_obj, draw_buf);

    // --- 3. Prepare task parameters ---
    SvgRenderParams *params = (SvgRenderParams *)heap_caps_malloc(
        sizeof(SvgRenderParams), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!params) {
        ESP_LOGE(SVG_TAG, "Failed to allocate SVG render params");
        return false;
    }
    params->canvas_obj    = canvas_obj;
    params->svg_data      = svg_data;
    params->svg_data_size = svg_data_size;
    params->file_path     = nullptr;
    params->width         = width;
    params->height        = height;
    params->pixel_buffer  = pixel_buf;

    // --- 4. Allocate stack (PSRAM) + TCB (internal) for FreeRTOS task ---
    StackType_t *stack = (StackType_t *)heap_caps_malloc(
        SVG_TASK_STACK_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!stack) {
        ESP_LOGE(SVG_TAG, "Failed to allocate %u bytes SVG stack in PSRAM",
                 (unsigned)SVG_TASK_STACK_SIZE);
        heap_caps_free(params);
        return false;
    }

    StaticTask_t *tcb = (StaticTask_t *)heap_caps_malloc(
        sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!tcb) {
        ESP_LOGE(SVG_TAG, "Failed to allocate SVG task TCB");
        heap_caps_free(params);
        heap_caps_free(stack);
        return false;
    }

    ESP_LOGI(SVG_TAG, "Creating SVG render task (%u KB PSRAM stack)",
             (unsigned)(SVG_TASK_STACK_SIZE / 1024));

    TaskHandle_t task = xTaskCreateStatic(
        svg_render_task,
        "svg_render",
        SVG_TASK_STACK_SIZE / sizeof(StackType_t),
        params,
        5,   // priority
        stack,
        tcb);

    if (!task) {
        ESP_LOGE(SVG_TAG, "xTaskCreateStatic failed for SVG render");
        heap_caps_free(params);
        heap_caps_free(stack);
        heap_caps_free(tcb);
        return false;
    }

    // stack + tcb persist until the task self-deletes.
    return true;
}

// --------------------------------------------------------------------------
// Convenience: set up canvas + render from a filesystem SVG file.
// The file is read inside the async task (on the large PSRAM stack).
// --------------------------------------------------------------------------
inline bool svg_setup_and_render_file(lv_obj_t *canvas_obj,
                                       const char *file_path,
                                       uint32_t width, uint32_t height) {
    // Allocate pixel buffer + draw-buf (same as embedded path)
    size_t buf_bytes = (size_t)width * height * sizeof(uint32_t);
    uint32_t *pixel_buf = (uint32_t *)heap_caps_malloc(
        buf_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pixel_buf) {
        ESP_LOGE(SVG_TAG, "Failed to allocate %u bytes in PSRAM for SVG", (unsigned)buf_bytes);
        return false;
    }
    memset(pixel_buf, 0, buf_bytes);

    lv_draw_buf_t *draw_buf = (lv_draw_buf_t *)heap_caps_malloc(
        sizeof(lv_draw_buf_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!draw_buf) {
        ESP_LOGE(SVG_TAG, "Failed to allocate draw_buf");
        heap_caps_free(pixel_buf);
        return false;
    }
    lv_draw_buf_init(draw_buf, width, height,
                     LV_COLOR_FORMAT_ARGB8888, 0,
                     pixel_buf, buf_bytes);
    lv_draw_buf_set_flag(draw_buf, LV_IMAGE_FLAGS_MODIFIABLE);
    lv_canvas_set_draw_buf(canvas_obj, draw_buf);

    // Prepare params – svg_data=NULL, file_path set
    SvgRenderParams *params = (SvgRenderParams *)heap_caps_malloc(
        sizeof(SvgRenderParams), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!params) {
        ESP_LOGE(SVG_TAG, "Failed to allocate SVG render params");
        return false;
    }
    params->canvas_obj    = canvas_obj;
    params->svg_data      = nullptr;
    params->svg_data_size = 0;
    params->file_path     = file_path;
    params->width         = width;
    params->height        = height;
    params->pixel_buffer  = pixel_buf;

    StackType_t *stack = (StackType_t *)heap_caps_malloc(
        SVG_TASK_STACK_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!stack) {
        ESP_LOGE(SVG_TAG, "Failed to allocate SVG stack in PSRAM");
        heap_caps_free(params);
        return false;
    }

    StaticTask_t *tcb = (StaticTask_t *)heap_caps_malloc(
        sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!tcb) {
        ESP_LOGE(SVG_TAG, "Failed to allocate SVG task TCB");
        heap_caps_free(params);
        heap_caps_free(stack);
        return false;
    }

    ESP_LOGI(SVG_TAG, "Creating SVG file render task for %s (%u KB PSRAM stack)",
             file_path, (unsigned)(SVG_TASK_STACK_SIZE / 1024));

    TaskHandle_t task = xTaskCreateStatic(
        svg_render_task,
        "svg_render",
        SVG_TASK_STACK_SIZE / sizeof(StackType_t),
        params,
        5,
        stack,
        tcb);

    if (!task) {
        ESP_LOGE(SVG_TAG, "xTaskCreateStatic failed for SVG file render");
        heap_caps_free(params);
        heap_caps_free(stack);
        heap_caps_free(tcb);
        return false;
    }

    return true;
}

}  // namespace lvgl
}  // namespace esphome

#endif  // USE_ESP32
