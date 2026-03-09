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
// Requires pre-parsed animations (from compile-time Python extraction).
// ---------------------------------------------------------------------------
inline bool asvg_init_file(lv_obj_t *canvas_obj,
                            const char *file_path,
                            const SmilAnim *anims, uint8_t num_anims,
                            uint32_t width, uint32_t height,
                            uint32_t frame_delay_ms, bool user_wants_hidden) {
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

// ===========================================================================
// Runtime SMIL parser — parses animated SVG files from filesystem (SD card)
// at runtime without needing compile-time Python preprocessing.
// ===========================================================================

// ---------------------------------------------------------------------------
// Helper: skip whitespace
// ---------------------------------------------------------------------------
inline const char *rt_skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

// ---------------------------------------------------------------------------
// Helper: extract attribute value from inside an XML tag.
// p should point somewhere inside an opening tag (after '<tagname').
// tag_end should point to the '>' that closes this tag.
// Returns pointer to a heap-allocated string (caller must free), or nullptr.
// ---------------------------------------------------------------------------
inline char *rt_get_attr(const char *tag_start, const char *tag_end, const char *attr_name) {
    size_t attr_len = strlen(attr_name);
    const char *p = tag_start;

    while (p < tag_end) {
        // Find attribute name
        const char *found = strstr(p, attr_name);
        if (!found || found >= tag_end) return nullptr;

        // Ensure it's a real attribute boundary (preceded by space/tab/newline)
        if (found > tag_start) {
            char before = *(found - 1);
            if (before != ' ' && before != '\t' && before != '\n' && before != '\r') {
                p = found + 1;
                continue;
            }
        }

        const char *eq = found + attr_len;
        eq = rt_skip_ws(eq);
        if (*eq != '=') { p = found + 1; continue; }
        eq++;
        eq = rt_skip_ws(eq);

        char quote = *eq;
        if (quote != '"' && quote != '\'') { p = found + 1; continue; }
        eq++;  // skip opening quote

        const char *val_end = strchr(eq, quote);
        if (!val_end || val_end > tag_end) return nullptr;

        size_t val_len = (size_t)(val_end - eq);
        char *result = (char *)malloc(val_len + 1);
        if (!result) return nullptr;
        memcpy(result, eq, val_len);
        result[val_len] = '\0';
        return result;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Helper: parse a duration string like "6s", ".67s", "500ms" to seconds.
// ---------------------------------------------------------------------------
inline float rt_parse_duration(const char *s) {
    if (!s || !*s) return 1.0f;
    float val = 0;
    bool negative = false;
    const char *p = s;
    if (*p == '-') { negative = true; p++; }

    // Parse number
    val = (float)strtod(p, (char **)&p);

    if (strncmp(p, "ms", 2) == 0) val /= 1000.0f;
    // 's' suffix or no suffix: already in seconds

    return negative ? -val : val;
}

// ---------------------------------------------------------------------------
// Helper: parse begin attribute for delay and repeat gap.
// E.g. "0s", "-.33s", "0s; x1.end+.33s"
// ---------------------------------------------------------------------------
inline void rt_parse_begin(const char *s, float *delay, float *repeat_gap) {
    *delay = 0;
    *repeat_gap = 0;
    if (!s || !*s) return;

    // Split on ';' — find first part
    const char *semi = strchr(s, ';');
    size_t first_len = semi ? (size_t)(semi - s) : strlen(s);

    // Check if first part contains ".end" or ".begin" (event reference)
    bool is_event_ref = false;
    for (size_t i = 0; i < first_len; i++) {
        if (s[i] == '.') { is_event_ref = true; break; }
    }

    if (!is_event_ref && first_len > 0) {
        // Simple delay value
        char tmp[32];
        size_t copy_len = first_len < 31 ? first_len : 31;
        memcpy(tmp, s, copy_len);
        tmp[copy_len] = '\0';
        *delay = rt_parse_duration(tmp);
    }

    // Look for ".end+Xs" or ".end-Xs" pattern in any part
    const char *end_ref = strstr(s, ".end");
    if (end_ref) {
        const char *p = end_ref + 4;
        while (*p == ' ') p++;
        if (*p == '+' || *p == '-') {
            bool neg = (*p == '-');
            p++;
            while (*p == ' ') p++;
            float gap = (float)strtod(p, nullptr);
            *repeat_gap = neg ? -gap : gap;
        }
    }
}

// ---------------------------------------------------------------------------
// Helper: parse "values" attribute — semicolon-separated groups of floats.
// E.g. "0 187.5 187.5; 360 187.5 187.5"
// Fills vals[] and returns count.
// ---------------------------------------------------------------------------
inline uint8_t rt_parse_values(const char *s, SmilValue *vals, uint8_t max_vals) {
    if (!s || !*s) return 0;

    uint8_t count = 0;
    const char *p = s;

    while (*p && count < max_vals) {
        p = rt_skip_ws(p);
        if (!*p) break;

        // Parse up to 3 floats
        float components[3] = {0, 0, 0};
        uint8_t nc = 0;

        while (*p && *p != ';' && nc < 3) {
            p = rt_skip_ws(p);
            if (*p == ';' || !*p) break;

            char *endp = nullptr;
            float v = strtof(p, &endp);
            if (endp == p) break;  // no progress
            components[nc++] = v;
            p = endp;
        }

        if (nc > 0) {
            vals[count].v[0] = components[0];
            vals[count].v[1] = components[1];
            vals[count].v[2] = components[2];
            vals[count].count = nc;
            count++;
        }

        // Skip past semicolon
        if (*p == ';') p++;
    }

    return count;
}

// ---------------------------------------------------------------------------
// Helper: parse "keyTimes" attribute — semicolon-separated floats (0..1).
// Returns count.
// ---------------------------------------------------------------------------
inline uint8_t rt_parse_key_times(const char *s, float *kt, uint8_t max_vals) {
    if (!s || !*s) return 0;

    uint8_t count = 0;
    const char *p = s;

    while (*p && count < max_vals) {
        p = rt_skip_ws(p);
        if (!*p) break;

        char *endp = nullptr;
        float v = strtof(p, &endp);
        if (endp == p) break;
        kt[count++] = v;
        p = endp;

        p = rt_skip_ws(p);
        if (*p == ';') p++;
    }

    return count;
}

// ---------------------------------------------------------------------------
// Runtime SMIL extraction context
// ---------------------------------------------------------------------------
struct RtSmilParseResult {
    SmilAnim anims[ASVG_MAX_ANIMS];
    uint8_t num_anims;
    char *svg_template;       // PSRAM — modified SVG with placeholders
    size_t svg_template_size;
};

// ---------------------------------------------------------------------------
// Find the enclosing element's transform attribute and inject placeholder.
// `tag_pos` points to '<animateTransform' or '<animate'.
// We search backwards from tag_pos to find the parent element's opening tag
// and inject a placeholder into its transform or opacity attribute.
//
// Strategy: We work with a mutable output buffer. Instead of modifying in
// place (complex), we do a two-pass approach:
//   Pass 1: Find all animation elements, record positions, build SmilAnim[]
//   Pass 2: Copy SVG to output, skipping animation elements and injecting
//           placeholder attributes into parent elements.
// ---------------------------------------------------------------------------

// Information about one discovered animation tag
struct RtAnimTag {
    size_t tag_start;      // position of '<' in source
    size_t tag_end;        // position after '>' or '/>' in source
    SmilAnim anim;         // parsed animation
    size_t parent_start;   // position of parent '<' in source
    size_t parent_tag_end; // position of '>' that ends parent opening tag
    bool is_transform;     // true = animateTransform, false = animate(opacity)
};

// ---------------------------------------------------------------------------
// Find the start of the parent element that contains position `pos`.
// Simple heuristic: scan backwards for '<' that starts a non-closing tag
// and is not self-closing before `pos`.
// ---------------------------------------------------------------------------
inline size_t rt_find_parent_tag_start(const char *svg, size_t pos) {
    // Count nesting depth going backwards
    int depth = 0;
    size_t i = pos;

    while (i > 0) {
        i--;
        if (svg[i] == '<') {
            if (i + 1 < pos && svg[i + 1] == '/') {
                // Closing tag — increase depth
                depth++;
            } else {
                if (depth > 0) {
                    depth--;
                } else {
                    // Check it's not a self-closing or processing instruction
                    if (svg[i + 1] != '?' && svg[i + 1] != '!') {
                        return i;
                    }
                }
            }
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Find the end of the opening tag at position `start` (find the first '>').
// ---------------------------------------------------------------------------
inline size_t rt_find_tag_end(const char *svg, size_t start, size_t len) {
    for (size_t i = start; i < len; i++) {
        if (svg[i] == '>') return i;
    }
    return len;
}

// ---------------------------------------------------------------------------
// Runtime SMIL parser — main function.
// Parses SVG text, extracts SMIL animations, builds template with placeholders.
// All allocations in PSRAM. Caller must free result->svg_template when done.
// ---------------------------------------------------------------------------
inline bool rt_parse_smil(const char *svg_data, size_t svg_len, RtSmilParseResult *result) {
    memset(result, 0, sizeof(RtSmilParseResult));

    // Pass 1: find all animation tags
    RtAnimTag found_anims[ASVG_MAX_ANIMS];
    uint8_t found_count = 0;

    const char *p = svg_data;
    while (*p && found_count < ASVG_MAX_ANIMS) {
        // Look for <animateTransform or <animate
        const char *at = strstr(p, "<animateTransform");
        const char *an = strstr(p, "<animate");

        // Skip <animateTransform when we find <animate first
        // (but <animate also matches <animateTransform, so be careful)
        const char *match = nullptr;
        bool is_transform = false;

        if (at && an) {
            if (at <= an) {
                match = at;
                is_transform = true;
            } else {
                // Check if `an` is actually `<animateTransform`
                if (strncmp(an, "<animateTransform", 17) == 0) {
                    match = an;
                    is_transform = true;
                } else {
                    match = an;
                    is_transform = false;
                }
            }
        } else if (at) {
            match = at;
            is_transform = true;
        } else if (an) {
            // Make sure it's not <animateTransform or <animateMotion
            if (strncmp(an, "<animateTransform", 17) == 0) {
                match = an;
                is_transform = true;
            } else if (strncmp(an, "<animateMotion", 14) == 0) {
                p = an + 14;
                continue;
            } else {
                match = an;
                is_transform = false;
            }
        } else {
            break;  // no more animation tags
        }

        size_t tag_start = (size_t)(match - svg_data);

        // Find end of this tag (self-closing '/>' or '>')
        const char *tag_p = match;
        const char *tag_end_ptr = nullptr;
        while (*tag_p) {
            if (*tag_p == '/' && *(tag_p + 1) == '>') {
                tag_end_ptr = tag_p + 2;
                break;
            }
            if (*tag_p == '>') {
                // Check if it's a self-closing tag (</animateTransform> shouldn't happen normally)
                tag_end_ptr = tag_p + 1;
                // Look for closing tag if not self-closing
                if (*(tag_p - 1) != '/') {
                    const char *close = is_transform ?
                        strstr(tag_end_ptr, "</animateTransform>") :
                        strstr(tag_end_ptr, "</animate>");
                    if (close) {
                        tag_end_ptr = close + (is_transform ? 19 : 10);
                    }
                }
                break;
            }
            tag_p++;
        }
        if (!tag_end_ptr) break;

        size_t tag_end = (size_t)(tag_end_ptr - svg_data);

        // Parse the animation attributes
        RtAnimTag *at_entry = &found_anims[found_count];
        memset(at_entry, 0, sizeof(RtAnimTag));
        at_entry->tag_start = tag_start;
        at_entry->tag_end = tag_end;
        at_entry->is_transform = is_transform;

        SmilAnim *anim = &at_entry->anim;
        anim->placeholder_id = found_count;

        if (is_transform) {
            char *type_str = rt_get_attr(match, tag_end_ptr, "type");
            if (type_str) {
                if (strcmp(type_str, "rotate") == 0) anim->type = SMIL_ROTATE;
                else if (strcmp(type_str, "translate") == 0) anim->type = SMIL_TRANSLATE;
                else if (strcmp(type_str, "scale") == 0) anim->type = SMIL_SCALE;
                else anim->type = SMIL_TRANSLATE;
                free(type_str);
            } else {
                anim->type = SMIL_TRANSLATE;
            }
        } else {
            // <animate> — check attributeName
            char *attr_name = rt_get_attr(match, tag_end_ptr, "attributeName");
            if (attr_name) {
                if (strcmp(attr_name, "opacity") == 0) {
                    anim->type = SMIL_OPACITY;
                } else {
                    // Unsupported attribute — skip
                    free(attr_name);
                    p = tag_end_ptr;
                    continue;
                }
                free(attr_name);
            } else {
                p = tag_end_ptr;
                continue;
            }
        }

        // Parse values
        char *values_str = rt_get_attr(match, tag_end_ptr, "values");
        if (values_str) {
            anim->num_values = rt_parse_values(values_str, anim->values, ASVG_MAX_VALUES);
            free(values_str);
        }

        // Parse duration
        char *dur_str = rt_get_attr(match, tag_end_ptr, "dur");
        anim->duration_s = rt_parse_duration(dur_str);
        if (dur_str) free(dur_str);

        // Parse begin (delay + repeat gap)
        char *begin_str = rt_get_attr(match, tag_end_ptr, "begin");
        rt_parse_begin(begin_str, &anim->begin_delay_s, &anim->repeat_gap_s);
        if (begin_str) free(begin_str);

        // Parse additive
        char *additive_str = rt_get_attr(match, tag_end_ptr, "additive");
        anim->additive = (additive_str && strcmp(additive_str, "sum") == 0);
        if (additive_str) free(additive_str);

        // Parse keyTimes
        char *kt_str = rt_get_attr(match, tag_end_ptr, "keyTimes");
        if (kt_str) {
            uint8_t kt_count = rt_parse_key_times(kt_str, anim->key_times, ASVG_MAX_VALUES);
            anim->has_key_times = (kt_count > 0);
            free(kt_str);
        }

        // Find parent element
        at_entry->parent_start = rt_find_parent_tag_start(svg_data, tag_start);
        at_entry->parent_tag_end = rt_find_tag_end(svg_data, at_entry->parent_start, svg_len);

        found_count++;
        p = tag_end_ptr;
    }

    if (found_count == 0) {
        ESP_LOGW(ASVG_TAG, "No SMIL animations found in SVG");
        // Still create template (just a copy)
        result->svg_template = (char *)heap_caps_malloc(svg_len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!result->svg_template) return false;
        memcpy(result->svg_template, svg_data, svg_len);
        result->svg_template[svg_len] = '\0';
        result->svg_template_size = svg_len;
        return true;
    }

    // Pass 2: Build output SVG with placeholders
    // Allocate generous output buffer (original + space for placeholders)
    size_t out_capacity = svg_len + found_count * 64 + 256;
    char *out = (char *)heap_caps_malloc(out_capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!out) return false;

    size_t out_pos = 0;
    size_t src_pos = 0;

    // Track which parent elements we've already added placeholders to
    // (multiple animations can share the same parent)
    struct ParentPH {
        size_t parent_start;
        size_t parent_tag_end;
        int transform_ph;   // -1 if none
        int opacity_ph;     // -1 if none
    };
    ParentPH parents[ASVG_MAX_ANIMS];
    uint8_t num_parents = 0;

    // Group animations by parent
    for (uint8_t i = 0; i < found_count; i++) {
        RtAnimTag *at_e = &found_anims[i];
        int parent_idx = -1;
        for (uint8_t j = 0; j < num_parents; j++) {
            if (parents[j].parent_start == at_e->parent_start) {
                parent_idx = j;
                break;
            }
        }
        if (parent_idx < 0) {
            parent_idx = num_parents++;
            parents[parent_idx].parent_start = at_e->parent_start;
            parents[parent_idx].parent_tag_end = at_e->parent_tag_end;
            parents[parent_idx].transform_ph = -1;
            parents[parent_idx].opacity_ph = -1;
        }
        if (at_e->is_transform) {
            parents[parent_idx].transform_ph = at_e->anim.placeholder_id;
        } else {
            parents[parent_idx].opacity_ph = at_e->anim.placeholder_id;
        }
    }

    // Sort animations by tag_start for sequential processing
    // Simple bubble sort (max 16 elements)
    for (uint8_t i = 0; i < found_count; i++) {
        for (uint8_t j = i + 1; j < found_count; j++) {
            if (found_anims[j].tag_start < found_anims[i].tag_start) {
                RtAnimTag tmp = found_anims[i];
                found_anims[i] = found_anims[j];
                found_anims[j] = tmp;
            }
        }
    }

    // Process: copy SVG, skip animation tags, inject placeholders at parent tags
    for (uint8_t i = 0; i < found_count; i++) {
        RtAnimTag *at_e = &found_anims[i];

        // Copy everything from current position to just before this animation tag
        if (at_e->tag_start > src_pos) {
            size_t copy_len = at_e->tag_start - src_pos;

            // Check if any parent tag '>' falls within this range and needs placeholder injection
            for (uint8_t pi = 0; pi < num_parents; pi++) {
                size_t pte = parents[pi].parent_tag_end;
                if (pte >= src_pos && pte < at_e->tag_start) {
                    // Copy up to the '>' of parent tag
                    size_t before_gt = pte - src_pos;
                    if (before_gt > 0 && out_pos + before_gt < out_capacity - 128) {
                        memcpy(out + out_pos, svg_data + src_pos, before_gt);
                        out_pos += before_gt;
                        src_pos += before_gt;
                    }

                    // Inject transform placeholder
                    if (parents[pi].transform_ph >= 0) {
                        int n = snprintf(out + out_pos, out_capacity - out_pos,
                                         " transform=\"__PH%d__\"", parents[pi].transform_ph);
                        if (n > 0) out_pos += n;
                        parents[pi].transform_ph = -2;  // mark as injected
                    }

                    // Inject opacity placeholder
                    if (parents[pi].opacity_ph >= 0) {
                        int n = snprintf(out + out_pos, out_capacity - out_pos,
                                         " opacity=\"__PH%d__\"", parents[pi].opacity_ph);
                        if (n > 0) out_pos += n;
                        parents[pi].opacity_ph = -2;  // mark as injected
                    }

                    // Continue copying
                    copy_len = at_e->tag_start - src_pos;
                }
            }

            if (copy_len > 0 && out_pos + copy_len < out_capacity - 1) {
                memcpy(out + out_pos, svg_data + src_pos, copy_len);
                out_pos += copy_len;
            }
        }

        // Skip the animation tag entirely
        src_pos = at_e->tag_end;
    }

    // Handle any remaining parent placeholder injections after last animation tag
    for (uint8_t pi = 0; pi < num_parents; pi++) {
        size_t pte = parents[pi].parent_tag_end;
        if (pte >= src_pos && (parents[pi].transform_ph >= 0 || parents[pi].opacity_ph >= 0)) {
            size_t before_gt = pte - src_pos;
            if (before_gt > 0 && out_pos + before_gt < out_capacity - 128) {
                memcpy(out + out_pos, svg_data + src_pos, before_gt);
                out_pos += before_gt;
                src_pos += before_gt;
            }
            if (parents[pi].transform_ph >= 0) {
                int n = snprintf(out + out_pos, out_capacity - out_pos,
                                 " transform=\"__PH%d__\"", parents[pi].transform_ph);
                if (n > 0) out_pos += n;
            }
            if (parents[pi].opacity_ph >= 0) {
                int n = snprintf(out + out_pos, out_capacity - out_pos,
                                 " opacity=\"__PH%d__\"", parents[pi].opacity_ph);
                if (n > 0) out_pos += n;
            }
        }
    }

    // Copy remaining SVG text
    if (src_pos < svg_len && out_pos + (svg_len - src_pos) < out_capacity - 1) {
        memcpy(out + out_pos, svg_data + src_pos, svg_len - src_pos);
        out_pos += svg_len - src_pos;
    }
    out[out_pos] = '\0';

    // Build result
    result->svg_template = out;
    result->svg_template_size = out_pos;
    result->num_anims = found_count;
    for (uint8_t i = 0; i < found_count; i++) {
        result->anims[i] = found_anims[i].anim;
    }

    ESP_LOGI(ASVG_TAG, "Runtime SMIL parse: %u animations, template %u bytes",
             (unsigned)found_count, (unsigned)out_pos);
    return true;
}

// ---------------------------------------------------------------------------
// Public API: initialise from filesystem with runtime SMIL parsing.
// Reads SVG file, parses SMIL animations at runtime, and starts animation.
// This is the function to use for animated SVGs on SD card / LittleFS.
// ---------------------------------------------------------------------------
inline bool asvg_init_file_rt(lv_obj_t *canvas_obj,
                               const char *file_path,
                               uint32_t width, uint32_t height,
                               uint32_t frame_delay_ms, bool user_wants_hidden) {
    // Read the SVG file
    FILE *f = fopen(file_path, "r");
    if (!f) {
        ESP_LOGE(ASVG_TAG, "Cannot open: %s", file_path);
        return false;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return false; }

    char *raw_svg = (char *)heap_caps_malloc(sz + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!raw_svg) {
        ESP_LOGE(ASVG_TAG, "Alloc failed for %ld bytes", sz);
        fclose(f);
        return false;
    }

    size_t nread = fread(raw_svg, 1, sz, f);
    fclose(f);
    raw_svg[nread] = '\0';

    ESP_LOGI(ASVG_TAG, "Read %u bytes from %s", (unsigned)nread, file_path);

    // Parse SMIL animations at runtime
    RtSmilParseResult parse_result;
    if (!rt_parse_smil(raw_svg, nread, &parse_result)) {
        ESP_LOGE(ASVG_TAG, "SMIL parse failed for %s", file_path);
        heap_caps_free(raw_svg);
        return false;
    }

    // Free the raw SVG — we now have the template in parse_result
    heap_caps_free(raw_svg);

    if (parse_result.num_anims == 0) {
        ESP_LOGW(ASVG_TAG, "No animations in %s, rendering as static SVG", file_path);
    }

    // Copy animations to persistent PSRAM allocation
    SmilAnim *persistent_anims = nullptr;
    if (parse_result.num_anims > 0) {
        size_t anims_size = sizeof(SmilAnim) * parse_result.num_anims;
        persistent_anims = (SmilAnim *)heap_caps_malloc(anims_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!persistent_anims) {
            heap_caps_free(parse_result.svg_template);
            return false;
        }
        memcpy(persistent_anims, parse_result.anims, anims_size);
    }

    // Initialise the animated SVG widget
    return asvg_init(canvas_obj, parse_result.svg_template, parse_result.svg_template_size,
                      persistent_anims, parse_result.num_anims,
                      width, height, frame_delay_ms, user_wants_hidden);
}

}  // namespace lvgl
}  // namespace esphome

#endif  // USE_ESP32
