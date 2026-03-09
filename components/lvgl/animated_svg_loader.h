#pragma once

#ifdef USE_ESP32

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <lvgl.h>

// ThorVG C API – compiled into LVGL when LV_USE_THORVG_INTERNAL=1.
#include <src/libs/thorvg/thorvg_capi.h>

namespace esphome {
namespace lvgl {

static const char *const ASVG_TAG = "anim_svg";
static constexpr size_t ASVG_TASK_STACK_SIZE = 64 * 1024;
static constexpr uint32_t ASVG_MAX_ANIMS = 16;
static constexpr uint32_t ASVG_MAX_VALUES = 8;

// ---------------------------------------------------------------------------
// SMIL animation types
// ---------------------------------------------------------------------------
enum SmilAnimType : uint8_t {
    SMIL_ROTATE = 0,
    SMIL_TRANSLATE = 1,
    SMIL_OPACITY = 2,
    SMIL_SCALE = 3,
};

// ---------------------------------------------------------------------------
// One keyframe value – up to 3 components (e.g. rotate: angle cx cy)
// ---------------------------------------------------------------------------
struct SmilValue {
    float v[3];    // components (unused ones = 0)
    uint8_t count; // number of valid components (1-3)
};

// ---------------------------------------------------------------------------
// Describes a single SMIL animation extracted from the SVG at compile time.
// The Python code generates an array of these as C initialisers.
// ---------------------------------------------------------------------------
struct SmilAnim {
    SmilAnimType type;
    uint8_t num_values;                // number of keyframe values
    SmilValue values[ASVG_MAX_VALUES]; // keyframe values
    float key_times[ASVG_MAX_VALUES];  // normalised 0..1 timing (0 if linear)
    bool has_key_times;
    float duration_s;                  // animation duration in seconds
    float begin_delay_s;               // initial delay in seconds
    float repeat_gap_s;                // gap between repeats (for begin="id.end+Xs")
    bool additive;                     // additive="sum"
    uint16_t placeholder_id;           // index of placeholder in SVG template
};

// ---------------------------------------------------------------------------
// Runtime context for one animated SVG widget.
// ---------------------------------------------------------------------------
struct AnimSvgContext {
    // --- Config (set once, never freed) ---
    lv_obj_t *canvas_obj;
    const char *svg_template;        // SVG text with placeholders (PROGMEM)
    size_t svg_template_size;
    const SmilAnim *anims;           // animation descriptors (PROGMEM)
    uint8_t num_anims;
    uint32_t width;
    uint32_t height;
    uint32_t frame_delay_ms;         // target frame time (e.g. 100 = 10 FPS)

    // --- Runtime state (freed on screen unload) ---
    uint32_t *pixel_buffer;          // PSRAM – width*height*4 bytes
    lv_draw_buf_t *draw_buf;         // internal RAM
    StackType_t *task_stack;         // PSRAM – 64 KB
    StaticTask_t *task_tcb;          // internal RAM
    TaskHandle_t task_handle;
    volatile bool stop_requested;
    bool user_wants_hidden;
    bool runtime_hidden;
};

// ---------------------------------------------------------------------------
// Interpolate between keyframe values at normalised time t (0..1).
// ---------------------------------------------------------------------------
inline void smil_interpolate(const SmilAnim *anim, float t, float out[3]) {
    if (anim->num_values == 0) {
        out[0] = out[1] = out[2] = 0;
        return;
    }
    if (anim->num_values == 1) {
        for (int i = 0; i < 3; i++) out[i] = anim->values[0].v[i];
        return;
    }

    // Build effective key times
    float kt[ASVG_MAX_VALUES];
    if (anim->has_key_times) {
        for (int i = 0; i < anim->num_values; i++) kt[i] = anim->key_times[i];
    } else {
        for (int i = 0; i < anim->num_values; i++) {
            kt[i] = (float)i / (float)(anim->num_values - 1);
        }
    }

    // Clamp t
    if (t <= kt[0]) {
        for (int i = 0; i < 3; i++) out[i] = anim->values[0].v[i];
        return;
    }
    if (t >= kt[anim->num_values - 1]) {
        for (int i = 0; i < 3; i++) out[i] = anim->values[anim->num_values - 1].v[i];
        return;
    }

    // Find segment
    for (int seg = 0; seg < anim->num_values - 1; seg++) {
        if (t >= kt[seg] && t <= kt[seg + 1]) {
            float seg_t = (t - kt[seg]) / (kt[seg + 1] - kt[seg]);
            const SmilValue *a = &anim->values[seg];
            const SmilValue *b = &anim->values[seg + 1];
            uint8_t nc = a->count > b->count ? a->count : b->count;
            for (int i = 0; i < 3; i++) {
                if (i < nc)
                    out[i] = a->v[i] + (b->v[i] - a->v[i]) * seg_t;
                else
                    out[i] = 0;
            }
            return;
        }
    }
    // fallback
    for (int i = 0; i < 3; i++) out[i] = anim->values[0].v[i];
}

// ---------------------------------------------------------------------------
// Calculate the normalised time (0..1) for an animation given elapsed seconds.
// Handles begin delay, duration, and repeat gap.
// ---------------------------------------------------------------------------
inline float smil_calc_t(const SmilAnim *anim, float elapsed_s) {
    float cycle = anim->duration_s + anim->repeat_gap_s;
    if (cycle <= 0) return 0;

    // Adjust for begin delay
    float adj = elapsed_s - anim->begin_delay_s;
    if (adj < 0) {
        // Handle negative begin delay (e.g. begin="-.33s")
        // This means the animation starts partway through
        adj = fmodf(adj, cycle);
        if (adj < 0) adj += cycle;
    }

    // Position within current cycle
    float in_cycle = fmodf(adj, cycle);
    if (in_cycle >= anim->duration_s) {
        // In the gap – show last frame
        return 1.0f;
    }

    return in_cycle / anim->duration_s;
}

// ---------------------------------------------------------------------------
// Build the SVG string for the current frame by replacing placeholders.
//
// The SVG template contains placeholders like:
//   __PH0__ for placeholder_id 0
//   __PH1__ for placeholder_id 1
//   etc.
//
// Each placeholder is replaced with the appropriate SVG attribute fragment:
//   rotate:    rotate(angle cx cy)
//   translate: translate(tx ty)
//   opacity:   0.75
//   scale:     scale(sx)
// ---------------------------------------------------------------------------
inline size_t asvg_build_frame_svg(const AnimSvgContext *ctx, float elapsed_s,
                                    char *out_buf, size_t out_buf_size) {
    // First, compute all placeholder replacement strings
    char replacements[ASVG_MAX_ANIMS][64];
    memset(replacements, 0, sizeof(replacements));

    for (int i = 0; i < ctx->num_anims; i++) {
        const SmilAnim *anim = &ctx->anims[i];
        float t = smil_calc_t(anim, elapsed_s);
        float val[3];
        smil_interpolate(anim, t, val);

        uint16_t pid = anim->placeholder_id;
        if (pid >= ASVG_MAX_ANIMS) continue;

        switch (anim->type) {
            case SMIL_ROTATE:
                snprintf(replacements[pid], sizeof(replacements[pid]),
                         " rotate(%.2f %.2f %.2f)", val[0], val[1], val[2]);
                break;
            case SMIL_TRANSLATE:
                snprintf(replacements[pid], sizeof(replacements[pid]),
                         " translate(%.2f %.2f)", val[0], val[1]);
                break;
            case SMIL_OPACITY:
                snprintf(replacements[pid], sizeof(replacements[pid]),
                         "%.3f", val[0]);
                break;
            case SMIL_SCALE:
                snprintf(replacements[pid], sizeof(replacements[pid]),
                         " scale(%.3f)", val[0]);
                break;
        }
    }

    // Now scan the template and copy to output, replacing __PHn__ placeholders
    size_t out_pos = 0;
    size_t tmpl_len = ctx->svg_template_size;
    const char *tmpl = ctx->svg_template;

    for (size_t i = 0; i < tmpl_len && out_pos < out_buf_size - 1; ) {
        // Check for placeholder pattern: __PH followed by digit(s) then __
        if (i + 6 <= tmpl_len && tmpl[i] == '_' && tmpl[i+1] == '_' &&
            tmpl[i+2] == 'P' && tmpl[i+3] == 'H') {
            // Parse the placeholder ID
            size_t j = i + 4;
            int pid = 0;
            bool found_digits = false;
            while (j < tmpl_len && tmpl[j] >= '0' && tmpl[j] <= '9') {
                pid = pid * 10 + (tmpl[j] - '0');
                j++;
                found_digits = true;
            }
            // Check for closing __
            if (found_digits && j + 1 < tmpl_len && tmpl[j] == '_' && tmpl[j+1] == '_') {
                // Valid placeholder – replace with computed value
                if (pid < ASVG_MAX_ANIMS && replacements[pid][0] != '\0') {
                    size_t rlen = strlen(replacements[pid]);
                    if (out_pos + rlen < out_buf_size - 1) {
                        memcpy(out_buf + out_pos, replacements[pid], rlen);
                        out_pos += rlen;
                    }
                }
                i = j + 2; // skip past closing __
                continue;
            }
        }
        out_buf[out_pos++] = tmpl[i++];
    }
    out_buf[out_pos] = '\0';
    return out_pos;
}

// ---------------------------------------------------------------------------
// Render one frame of the animated SVG using ThorVG.
// ---------------------------------------------------------------------------
inline bool asvg_render_frame(AnimSvgContext *ctx, const char *svg_data, size_t svg_len) {
    memset(ctx->pixel_buffer, 0, ctx->width * ctx->height * sizeof(uint32_t));

    tvg_engine_init(TVG_ENGINE_SW, 0);

    Tvg_Canvas *tc = tvg_swcanvas_create();
    if (!tc) return false;

    if (tvg_swcanvas_set_target(tc, ctx->pixel_buffer, ctx->width,
                                 ctx->width, ctx->height,
                                 TVG_COLORSPACE_ARGB8888) != TVG_RESULT_SUCCESS) {
        tvg_canvas_destroy(tc);
        return false;
    }

    Tvg_Paint *pic = tvg_picture_new();
    if (!pic) { tvg_canvas_destroy(tc); return false; }

    if (tvg_picture_load_data(pic, svg_data, (uint32_t)svg_len,
                               "svg", true) != TVG_RESULT_SUCCESS) {
        tvg_paint_del(pic);
        tvg_canvas_destroy(tc);
        return false;
    }

    float ow = 0, oh = 0;
    tvg_picture_get_size(pic, &ow, &oh);
    tvg_picture_set_size(pic, (float)ctx->width, (float)ctx->height);

    if (tvg_canvas_push(tc, pic) != TVG_RESULT_SUCCESS) {
        tvg_paint_del(pic);
        tvg_canvas_destroy(tc);
        return false;
    }

    tvg_canvas_draw(tc);
    tvg_canvas_sync(tc);
    tvg_canvas_destroy(tc);

    return true;
}

// ---------------------------------------------------------------------------
// Animation render task – runs on 64 KB PSRAM stack.
// Renders the SVG at the target frame rate, updating placeholders each frame.
// ---------------------------------------------------------------------------
inline void asvg_render_task(void *param) {
    AnimSvgContext *ctx = (AnimSvgContext *)param;

    vTaskDelay(pdMS_TO_TICKS(500));

    // Allocate working buffer for SVG text (template + extra space for replacements)
    size_t svg_buf_size = ctx->svg_template_size + ctx->num_anims * 64 + 256;
    char *svg_buf = (char *)heap_caps_malloc(svg_buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!svg_buf) {
        // Fallback to internal RAM if PSRAM alloc fails
        svg_buf = (char *)malloc(svg_buf_size);
    }
    if (!svg_buf) {
        ESP_LOGE(ASVG_TAG, "SVG buffer alloc failed (%u bytes)", (unsigned)svg_buf_size);
        goto done;
    }

    ESP_LOGI(ASVG_TAG, "Animated SVG render loop starting (%ux%u, %u anims, %u ms/frame)",
             (unsigned)ctx->width, (unsigned)ctx->height,
             (unsigned)ctx->num_anims, (unsigned)ctx->frame_delay_ms);

    {
        TickType_t start_tick = xTaskGetTickCount();
        bool first_frame = true;

        while (!ctx->stop_requested) {
            float elapsed_s = (float)((xTaskGetTickCount() - start_tick) * portTICK_PERIOD_MS) / 1000.0f;

            // Build SVG for this frame
            size_t svg_len = asvg_build_frame_svg(ctx, elapsed_s, svg_buf, svg_buf_size);

            // Render
            bool ok = asvg_render_frame(ctx, svg_buf, svg_len);

            if (ok) {
                lv_lock();
                if (first_frame) {
                    first_frame = false;
                    if (!ctx->runtime_hidden) {
                        lv_obj_remove_flag(ctx->canvas_obj, LV_OBJ_FLAG_HIDDEN);
                    }
                    ESP_LOGI(ASVG_TAG, "First frame rendered OK");
                }
                lv_obj_invalidate(ctx->canvas_obj);
                lv_unlock();
            }

            vTaskDelay(pdMS_TO_TICKS(ctx->frame_delay_ms));
        }
    }

    if (svg_buf) {
        if (heap_caps_get_allocated_size(svg_buf) > 0) {
            heap_caps_free(svg_buf);
        } else {
            free(svg_buf);
        }
    }

done:
    ESP_LOGI(ASVG_TAG, "Render task stopping");
    ctx->stop_requested = false;
    vTaskSuspend(NULL);
}

// ---------------------------------------------------------------------------
// Free all PSRAM/internal-RAM resources.
// ---------------------------------------------------------------------------
inline void asvg_free_resources(AnimSvgContext *ctx) {
    ctx->stop_requested = true;
    if (ctx->task_handle) {
        vTaskDelete(ctx->task_handle);
        ctx->task_handle = nullptr;
    }
    if (ctx->task_stack)   { heap_caps_free(ctx->task_stack);   ctx->task_stack = nullptr; }
    if (ctx->task_tcb)     { heap_caps_free(ctx->task_tcb);     ctx->task_tcb = nullptr; }
    if (ctx->pixel_buffer) { heap_caps_free(ctx->pixel_buffer); ctx->pixel_buffer = nullptr; }
    if (ctx->draw_buf)     { heap_caps_free(ctx->draw_buf);     ctx->draw_buf = nullptr; }
    ctx->stop_requested = false;

    ESP_LOGI(ASVG_TAG, "Animated SVG PSRAM freed (%ux%u = %u KB)",
             (unsigned)ctx->width, (unsigned)ctx->height,
             (unsigned)(ctx->width * ctx->height * 4 / 1024));
}

// ---------------------------------------------------------------------------
// (Re-)allocate buffers and launch the render task.
// Must be called under lv_lock.
// ---------------------------------------------------------------------------
inline bool asvg_launch(AnimSvgContext *ctx) {
    size_t buf_bytes = (size_t)ctx->width * ctx->height * sizeof(uint32_t);
    ctx->pixel_buffer = (uint32_t *)heap_caps_malloc(
        buf_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ctx->pixel_buffer) {
        ESP_LOGE(ASVG_TAG, "PSRAM alloc failed (%u bytes)", (unsigned)buf_bytes);
        return false;
    }
    memset(ctx->pixel_buffer, 0, buf_bytes);

    ctx->draw_buf = (lv_draw_buf_t *)heap_caps_malloc(
        sizeof(lv_draw_buf_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!ctx->draw_buf) {
        heap_caps_free(ctx->pixel_buffer); ctx->pixel_buffer = nullptr;
        return false;
    }
    lv_draw_buf_init(ctx->draw_buf, ctx->width, ctx->height,
                     LV_COLOR_FORMAT_ARGB8888, 0,
                     ctx->pixel_buffer, buf_bytes);
    lv_draw_buf_set_flag(ctx->draw_buf, LV_IMAGE_FLAGS_MODIFIABLE);
    lv_canvas_set_draw_buf(ctx->canvas_obj, ctx->draw_buf);

    lv_obj_add_flag(ctx->canvas_obj, LV_OBJ_FLAG_HIDDEN);

    ctx->task_stack = (StackType_t *)heap_caps_malloc(
        ASVG_TASK_STACK_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ctx->task_tcb = (StaticTask_t *)heap_caps_malloc(
        sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!ctx->task_stack || !ctx->task_tcb) {
        ESP_LOGE(ASVG_TAG, "Task alloc failed");
        asvg_free_resources(ctx);
        return false;
    }

    ctx->stop_requested = false;
    ctx->task_handle = xTaskCreateStatic(
        asvg_render_task, "asvg_render",
        ASVG_TASK_STACK_SIZE / sizeof(StackType_t),
        ctx, 5, ctx->task_stack, ctx->task_tcb);

    if (!ctx->task_handle) {
        asvg_free_resources(ctx);
        return false;
    }

    ESP_LOGI(ASVG_TAG, "Animated SVG launched (PSRAM: %u KB buf + 64 KB stack)",
             (unsigned)(buf_bytes / 1024));
    return true;
}

// ---------------------------------------------------------------------------
// Screen event callbacks – two-phase unload.
// ---------------------------------------------------------------------------
inline void asvg_screen_unload_start_cb(lv_event_t *e) {
    AnimSvgContext *ctx = (AnimSvgContext *)lv_event_get_user_data(e);

    ctx->runtime_hidden = lv_obj_has_flag(ctx->canvas_obj, LV_OBJ_FLAG_HIDDEN);

    ctx->stop_requested = true;
    if (ctx->task_handle) {
        vTaskDelete(ctx->task_handle);
        ctx->task_handle = nullptr;
    }

    lv_obj_add_flag(ctx->canvas_obj, LV_OBJ_FLAG_HIDDEN);

    ESP_LOGI(ASVG_TAG, "Task stopped, widget hidden (was_hidden=%d)", (int)ctx->runtime_hidden);
}

inline void asvg_screen_unloaded_cb(lv_event_t *e) {
    AnimSvgContext *ctx = (AnimSvgContext *)lv_event_get_user_data(e);

    if (ctx->task_stack)   { heap_caps_free(ctx->task_stack);   ctx->task_stack = nullptr; }
    if (ctx->task_tcb)     { heap_caps_free(ctx->task_tcb);     ctx->task_tcb = nullptr; }
    if (ctx->pixel_buffer) { heap_caps_free(ctx->pixel_buffer); ctx->pixel_buffer = nullptr; }
    if (ctx->draw_buf)     { heap_caps_free(ctx->draw_buf);     ctx->draw_buf = nullptr; }
    ctx->stop_requested = false;

    ESP_LOGI(ASVG_TAG, "Animated SVG FREED (%ux%u)",
             (unsigned)ctx->width, (unsigned)ctx->height);
}

inline void asvg_screen_loaded_cb(lv_event_t *e) {
    AnimSvgContext *ctx = (AnimSvgContext *)lv_event_get_user_data(e);
    if (ctx->pixel_buffer == nullptr) {
        asvg_launch(ctx);
    }
}

// ---------------------------------------------------------------------------
// Public API: initialise animated SVG widget.
// Call under lv_lock (from LVGL init code).
//
// svg_template:   SVG text with __PHn__ placeholders (embedded, PROGMEM)
// svg_template_size: length of svg_template (without null terminator)
// anims:          array of SmilAnim descriptors (embedded, PROGMEM)
// num_anims:      number of animations
// width, height:  render dimensions
// frame_delay_ms: time between frames (e.g. 100 = 10 FPS)
// ---------------------------------------------------------------------------
inline bool asvg_init(lv_obj_t *canvas_obj,
                       const char *svg_template, size_t svg_template_size,
                       const SmilAnim *anims, uint8_t num_anims,
                       uint32_t width, uint32_t height,
                       uint32_t frame_delay_ms, bool user_wants_hidden) {
    AnimSvgContext *ctx = (AnimSvgContext *)heap_caps_malloc(
        sizeof(AnimSvgContext), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!ctx) return false;
    memset(ctx, 0, sizeof(AnimSvgContext));

    ctx->canvas_obj       = canvas_obj;
    ctx->svg_template     = svg_template;
    ctx->svg_template_size = svg_template_size;
    ctx->anims            = anims;
    ctx->num_anims        = num_anims;
    ctx->width            = width;
    ctx->height           = height;
    ctx->frame_delay_ms   = frame_delay_ms > 0 ? frame_delay_ms : 100;
    ctx->user_wants_hidden = user_wants_hidden;
    ctx->runtime_hidden   = user_wants_hidden;

    lv_obj_set_user_data(canvas_obj, ctx);

    lv_obj_t *screen = lv_obj_get_screen(canvas_obj);
    lv_obj_add_event_cb(screen, asvg_screen_unload_start_cb,
                        LV_EVENT_SCREEN_UNLOAD_START, ctx);
    lv_obj_add_event_cb(screen, asvg_screen_unloaded_cb,
                        LV_EVENT_SCREEN_UNLOADED, ctx);
    lv_obj_add_event_cb(screen, asvg_screen_loaded_cb,
                        LV_EVENT_SCREEN_LOADED, ctx);

    ESP_LOGI(ASVG_TAG, "Animated SVG registered (%ux%u, %u anims, %u ms/frame), waiting for page load",
             (unsigned)width, (unsigned)height, (unsigned)num_anims, (unsigned)frame_delay_ms);
    return true;
}

// ---------------------------------------------------------------------------
// Public API: initialise from filesystem path (reads file at runtime).
// ---------------------------------------------------------------------------
inline bool asvg_init_file(lv_obj_t *canvas_obj,
                            const char *file_path,
                            const SmilAnim *anims, uint8_t num_anims,
                            uint32_t width, uint32_t height,
                            uint32_t frame_delay_ms, bool user_wants_hidden) {
    // Read file to get SVG template
    FILE *f = fopen(file_path, "r");
    if (!f) {
        ESP_LOGE(ASVG_TAG, "Cannot open: %s", file_path);
        return false;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return false; }

    char *buf = (char *)heap_caps_malloc(sz + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) { fclose(f); return false; }

    size_t nread = fread(buf, 1, sz, f);
    fclose(f);
    buf[nread] = '\0';

    return asvg_init(canvas_obj, buf, nread, anims, num_anims,
                      width, height, frame_delay_ms, user_wants_hidden);
}

}  // namespace lvgl
}  // namespace esphome

#endif  // USE_ESP32
