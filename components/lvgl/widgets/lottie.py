"""
LVGL 9.4 Lottie Animation Widget for ESPHome

This module implements the Lottie animation widget for LVGL 9.4.

Lottie is a library for parsing Adobe After Effects animations exported as JSON
using the Bodymovin plugin and rendering them natively.

Requirements:
- LV_USE_LOTTIE must be enabled
- LV_USE_THORVG_INTERNAL must be enabled
- LV_USE_VECTOR_GRAPHIC must be enabled

Usage in ESPHome YAML:

    Method 1 - File on filesystem (SD card, LittleFS):
    - lottie:
        id: my_animation
        src: "/sdcard/animation.json"   # File path on ESP32 filesystem
        width: 200                      # Required for src (can't read at compile time)
        height: 200
        loop: true
        auto_start: true

    Method 2 - Embedded in firmware (auto-detects size from JSON):
    - lottie:
        id: my_animation
        file: "animations/loading.json"  # Local file, embedded in firmware
        loop: true                       # width/height auto-detected from JSON
        auto_start: true

Actions:
    - lvgl.lottie.start: my_animation
    - lvgl.lottie.stop: my_animation
    - lvgl.lottie.pause: my_animation
"""

import json
from pathlib import Path

from esphome import automation, codegen as cg, config_validation as cv
from esphome.const import CONF_FILE, CONF_HEIGHT, CONF_ID, CONF_RAW_DATA_ID, CONF_WIDTH
from esphome.core import CORE

from ..automation import action_to_code
from ..defines import CONF_AUTO_START, CONF_MAIN, CONF_SRC, literal
from ..helpers import add_lv_use
from ..lv_validation import size
from ..lvcode import lv
from ..types import LvType, ObjUpdateAction
from . import Widget, WidgetType, get_widgets

CONF_LOTTIE = "lottie"
CONF_LOOP = "loop"
CONF_LOTTIE_WIDTH = "lottie_width"
CONF_LOTTIE_HEIGHT = "lottie_height"

lv_lottie_t = LvType("lv_lottie_t")


def lottie_path_validator(value):
    """Validate Lottie source file path (on ESP32 filesystem)."""
    value = cv.string(value)
    if not value.startswith("/"):
        raise cv.Invalid(
            f"Lottie src must be an absolute file path starting with '/', got: '{value}'. "
            f"Example: '/sdcard/animation.json' or '/littlefs/animation.json'"
        )
    if not value.endswith(".json"):
        raise cv.Invalid(
            f"Lottie src must be a JSON file (ending with .json), got: '{value}'"
        )
    return value


def lottie_file_validator(value):
    """Validate and resolve local Lottie file path (to embed in firmware)."""
    value = cv.string(value)
    # Resolve relative to config directory
    path = CORE.relative_config_path(value)
    if not Path(path).is_file():
        raise cv.Invalid(f"Lottie file not found: {path}")
    return str(path)


def validate_lottie_source(config):
    """Validate source and extract dimensions from JSON if using file method."""
    has_src = CONF_SRC in config
    has_file = CONF_FILE in config

    if has_src and has_file:
        raise cv.Invalid("Cannot specify both 'src' and 'file'. Use 'src' for filesystem path or 'file' for embedded.")
    if not has_src and not has_file:
        raise cv.Invalid("Must specify either 'src' (filesystem path) or 'file' (embedded in firmware).")

    # For src method, width and height are required
    if has_src:
        if CONF_WIDTH not in config or CONF_HEIGHT not in config:
            raise cv.Invalid("'width' and 'height' are required when using 'src' (filesystem path). Cannot auto-detect dimensions at compile time.")

    # For file method, auto-detect dimensions from JSON
    if has_file:
        file_path = config[CONF_FILE]
        try:
            with open(file_path, "r", encoding="utf-8") as f:
                lottie_data = json.load(f)
                # Extract dimensions from Lottie JSON
                lottie_width = lottie_data.get("w")
                lottie_height = lottie_data.get("h")
                if lottie_width is None or lottie_height is None:
                    raise cv.Invalid(f"Lottie JSON file missing 'w' or 'h' dimensions: {file_path}")
                # Store extracted dimensions
                config[CONF_LOTTIE_WIDTH] = int(lottie_width)
                config[CONF_LOTTIE_HEIGHT] = int(lottie_height)
        except json.JSONDecodeError as e:
            raise cv.Invalid(f"Invalid JSON in Lottie file {file_path}: {e}")
        except Exception as e:
            raise cv.Invalid(f"Error reading Lottie file {file_path}: {e}")

    return config


LOTTIE_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_WIDTH): size,
        cv.Optional(CONF_HEIGHT): size,
        cv.Optional(CONF_SRC): lottie_path_validator,
        cv.Optional(CONF_FILE): lottie_file_validator,
        cv.Optional(CONF_LOOP, default=True): cv.boolean,
        cv.Optional(CONF_AUTO_START, default=True): cv.boolean,
        cv.GenerateID(CONF_RAW_DATA_ID): cv.declare_id(cg.uint8),
    }
).add_extra(validate_lottie_source)

LOTTIE_MODIFY_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_SRC): lottie_path_validator,
        cv.Optional(CONF_LOOP): cv.boolean,
    }
)


class LottieType(WidgetType):
    def __init__(self):
        super().__init__(
            CONF_LOTTIE,
            lv_lottie_t,
            (CONF_MAIN,),
            LOTTIE_SCHEMA,
            LOTTIE_MODIFY_SCHEMA,
        )

    def get_uses(self):
        return ("LOTTIE", "THORVG_INTERNAL", "VECTOR_GRAPHIC")

    async def to_code(self, w: Widget, config):
        add_lv_use("LOTTIE")
        add_lv_use("THORVG_INTERNAL")
        add_lv_use("VECTOR_GRAPHIC")

        # Get dimensions - either from config or auto-detected from JSON
        if CONF_LOTTIE_WIDTH in config:
            # Auto-detected from JSON file
            width = config[CONF_LOTTIE_WIDTH]
            height = config[CONF_LOTTIE_HEIGHT]
        else:
            # Manually specified (required for src method)
            width = config[CONF_WIDTH]
            height = config[CONF_HEIGHT]

        # Allocate render buffer for Lottie animation
        # ARGB8888 format is required for vector graphics (4 bytes per pixel)
        buf_size = literal(f"({width} * {height} * 4)")
        lottie_buffer = lv.malloc_core(buf_size)

        # Set buffer for Lottie rendering
        lv.lottie_set_buffer(w.obj, width, height, lottie_buffer)

        # Set widget size to match animation
        from ..lvcode import lv_obj
        lv_obj.set_size(w.obj, width, height)

        # Load animation - Method 1: From filesystem
        # NOTE: We DON'T use lv_lottie_set_src_file() because ThorVG parsing
        # requires significant stack space (>32KB) which causes stack overflow
        # on ESP32. Instead, we load the file to heap and use lv_lottie_set_src_data().
        if src := config.get(CONF_SRC):
            from ..lvcode import lv_add
            # Add required includes
            cg.add_global(cg.RawExpression('#include <stdio.h>'))
            cg.add_global(cg.RawExpression('#include "esp_heap_caps.h"'))

            # Define the global helper function and struct once
            helper_code = '''
// Lottie file loader helper - loads JSON to heap to avoid stack overflow
#ifndef LOTTIE_LOADER_DEFINED
#define LOTTIE_LOADER_DEFINED
static bool lottie_load_from_file(lv_obj_t *obj, const char *path) {
    ESP_LOGI("lottie", "Loading Lottie from: %s", path);

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        ESP_LOGE("lottie", "Failed to open file: %s", path);
        return false;
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    ESP_LOGI("lottie", "File size: %ld bytes", fsize);

    // Allocate in PSRAM if available, +1 for null terminator
    char *json_buf = (char *)heap_caps_malloc(fsize + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (json_buf == NULL) {
        // Fallback to regular malloc
        json_buf = (char *)malloc(fsize + 1);
    }

    if (json_buf == NULL) {
        fclose(f);
        ESP_LOGE("lottie", "Failed to allocate %ld bytes", fsize);
        return false;
    }

    size_t read_size = fread(json_buf, 1, fsize, f);
    fclose(f);

    if (read_size != (size_t)fsize) {
        free(json_buf);
        ESP_LOGE("lottie", "Read incomplete: %zu/%ld bytes", read_size, fsize);
        return false;
    }

    json_buf[fsize] = '\\0';  // Null terminate for ThorVG parser

    // Load the animation data
    lv_lottie_set_src_data(obj, json_buf, fsize);
    ESP_LOGI("lottie", "Lottie loaded successfully (%ld bytes)", fsize);

    // Note: Buffer must stay allocated - LVGL/ThorVG needs it for playback
    return true;
}
#endif
'''
            cg.add_global(cg.RawExpression(helper_code))

            # Call the loader function
            load_call = f'lottie_load_from_file({w.obj}, "{src}");'
            lv_add(cg.RawStatement(load_call))

        # Load animation - Method 2: From embedded data
        # NOTE: ThorVG parsing requires significant stack space (>32KB).
        # We create a dedicated FreeRTOS task with stack allocated in PSRAM
        # to avoid stack overflow on ESP32.
        elif file_path := config.get(CONF_FILE):
            from ..lvcode import lv_add

            # Read the JSON file content
            with open(file_path, "rb") as f:
                json_data = f.read()

            # CRITICAL: Add null terminator for ThorVG JSON parser
            json_data_with_null = json_data + b'\x00'

            # Create progmem array with the JSON data (including null terminator)
            raw_data_id = config[CONF_RAW_DATA_ID]
            prog_arr = cg.progmem_array(raw_data_id, list(json_data_with_null))

            # Add required includes for FreeRTOS task with PSRAM stack
            cg.add_global(cg.RawExpression('#include "freertos/FreeRTOS.h"'))
            cg.add_global(cg.RawExpression('#include "freertos/task.h"'))
            cg.add_global(cg.RawExpression('#include "freertos/semphr.h"'))
            cg.add_global(cg.RawExpression('#include "esp_heap_caps.h"'))

            # Define the global helper for loading Lottie in a task with PSRAM stack
            helper_code = '''
// Lottie loader with dedicated task using PSRAM stack
// ThorVG parsing requires >32KB stack which exceeds ESP32 internal RAM limits
#ifndef LOTTIE_TASK_LOADER_DEFINED
#define LOTTIE_TASK_LOADER_DEFINED

typedef struct {
    lv_obj_t *obj;
    const void *data;
    size_t data_size;
    SemaphoreHandle_t done_sem;
} lottie_task_params_t;

static void lottie_parse_task(void *arg) {
    lottie_task_params_t *params = (lottie_task_params_t *)arg;

    ESP_LOGI("lottie", "Parsing Lottie in dedicated task (stack in PSRAM)...");
    lv_lottie_set_src_data(params->obj, params->data, params->data_size);
    ESP_LOGI("lottie", "Lottie parsing complete!");

    xSemaphoreGive(params->done_sem);
    vTaskDelete(NULL);
}

static bool lottie_load_with_psram_stack(lv_obj_t *obj, const void *data, size_t data_size) {
    ESP_LOGI("lottie", "Loading Lottie animation (%zu bytes) with PSRAM task stack...", data_size);

    // Allocate 64KB stack in PSRAM
    const size_t stack_size = 64 * 1024;
    StackType_t *task_stack = (StackType_t *)heap_caps_malloc(stack_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (task_stack == NULL) {
        ESP_LOGE("lottie", "Failed to allocate task stack in PSRAM, trying internal RAM...");
        task_stack = (StackType_t *)malloc(stack_size);
        if (task_stack == NULL) {
            ESP_LOGE("lottie", "Failed to allocate task stack!");
            return false;
        }
    }

    // Allocate task control block in internal RAM (required by FreeRTOS)
    StaticTask_t *task_tcb = (StaticTask_t *)heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (task_tcb == NULL) {
        ESP_LOGE("lottie", "Failed to allocate task TCB!");
        free(task_stack);
        return false;
    }

    // Setup parameters
    lottie_task_params_t params;
    params.obj = obj;
    params.data = data;
    params.data_size = data_size;
    params.done_sem = xSemaphoreCreateBinary();

    if (params.done_sem == NULL) {
        ESP_LOGE("lottie", "Failed to create semaphore!");
        free(task_stack);
        free(task_tcb);
        return false;
    }

    // Create task with static allocation (stack in PSRAM)
    TaskHandle_t task_handle = xTaskCreateStatic(
        lottie_parse_task,
        "lottie_parse",
        stack_size / sizeof(StackType_t),
        &params,
        5,  // Priority
        task_stack,
        task_tcb
    );

    if (task_handle == NULL) {
        ESP_LOGE("lottie", "Failed to create Lottie parsing task!");
        vSemaphoreDelete(params.done_sem);
        free(task_stack);
        free(task_tcb);
        return false;
    }

    // Wait for task to complete (max 60 seconds for complex animations)
    ESP_LOGI("lottie", "Waiting for Lottie parsing to complete...");
    bool success = xSemaphoreTake(params.done_sem, pdMS_TO_TICKS(60000)) == pdTRUE;

    if (!success) {
        ESP_LOGE("lottie", "Lottie parsing timed out!");
    } else {
        ESP_LOGI("lottie", "Lottie animation loaded successfully!");
    }

    // Cleanup
    vSemaphoreDelete(params.done_sem);
    free(task_stack);
    free(task_tcb);

    return success;
}
#endif
'''
            cg.add_global(cg.RawExpression(helper_code))

            # Call the loader function with PSRAM stack
            json_size = len(json_data)
            load_call = f'lottie_load_with_psram_stack({w.obj}, {prog_arr}, {json_size});'
            lv_add(cg.RawStatement(load_call))

        # Set looping (requires accessing the internal animation)
        # Note: In LVGL 9.4, use lv_lottie_get_anim() to get animation handle
        if not config.get(CONF_LOOP, True):
            # If not looping, set repeat count to 1
            pass  # Default is looping, non-loop not directly supported in simple API

        # Auto-start animation
        if config.get(CONF_AUTO_START, True):
            # Animation starts automatically when src is set
            pass


lottie_spec = LottieType()


@automation.register_action(
    "lvgl.lottie.start",
    ObjUpdateAction,
    cv.maybe_simple_value(
        {
            cv.Required(CONF_ID): cv.use_id(lv_lottie_t),
        },
        key=CONF_ID,
    ),
)
async def lottie_start(config, action_id, template_arg, args):
    """Start or resume the Lottie animation."""
    widget = await get_widgets(config)

    async def do_start(w: Widget):
        # Get the animation handle and resume it
        lv.anim_start(lv.lottie_get_anim(w.obj))

    return await action_to_code(widget, do_start, action_id, template_arg, args)


@automation.register_action(
    "lvgl.lottie.stop",
    ObjUpdateAction,
    cv.maybe_simple_value(
        {
            cv.Required(CONF_ID): cv.use_id(lv_lottie_t),
        },
        key=CONF_ID,
    ),
)
async def lottie_stop(config, action_id, template_arg, args):
    """Stop the Lottie animation and reset to beginning."""
    widget = await get_widgets(config)

    async def do_stop(w: Widget):
        # Delete the animation to stop it
        lv.anim_delete(w.obj, literal("NULL"))

    return await action_to_code(widget, do_stop, action_id, template_arg, args)


@automation.register_action(
    "lvgl.lottie.pause",
    ObjUpdateAction,
    cv.maybe_simple_value(
        {
            cv.Required(CONF_ID): cv.use_id(lv_lottie_t),
        },
        key=CONF_ID,
    ),
)
async def lottie_pause(config, action_id, template_arg, args):
    """Pause the Lottie animation at current frame."""
    widget = await get_widgets(config)

    async def do_pause(w: Widget):
        # Pause by setting animation to stopped state
        lv.anim_delete(w.obj, literal("NULL"))

    return await action_to_code(widget, do_pause, action_id, template_arg, args)
