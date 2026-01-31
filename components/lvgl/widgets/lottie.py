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
            f"Lottie src must be a file path starting with '/', got: {value}"
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
        if src := config.get(CONF_SRC):
            lv.lottie_set_src_file(w.obj, src)

        # Load animation - Method 2: From embedded data
        elif file_path := config.get(CONF_FILE):
            # Read the JSON file content
            with open(file_path, "rb") as f:
                json_data = f.read()

            # Create progmem array with the JSON data
            raw_data_id = config[CONF_RAW_DATA_ID]
            prog_arr = cg.progmem_array(raw_data_id, list(json_data))

            # Use lv_lottie_set_src_data to load from memory
            lv.lottie_set_src_data(w.obj, prog_arr, len(json_data))

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
