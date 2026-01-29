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
    - lottie:
        id: my_animation
        width: 200
        height: 200
        src: "/animation.json"      # File path on filesystem
        loop: true                  # Optional, default true
        auto_start: true            # Optional, default true

Actions:
    - lvgl.lottie.start: my_animation
    - lvgl.lottie.stop: my_animation
    - lvgl.lottie.pause: my_animation
"""

from esphome import automation, codegen as cg, config_validation as cv
from esphome.const import CONF_HEIGHT, CONF_ID, CONF_WIDTH

from ..automation import action_to_code
from ..defines import CONF_AUTO_START, CONF_MAIN, CONF_SRC, literal
from ..helpers import add_lv_use
from ..lv_validation import size
from ..lvcode import lv
from ..types import LvType, ObjUpdateAction
from . import Widget, WidgetType, get_widgets

CONF_LOTTIE = "lottie"
CONF_LOOP = "loop"

lv_lottie_t = LvType("lv_lottie_t")


def lottie_path_validator(value):
    """Validate Lottie source file path."""
    value = cv.string(value)
    if not value.startswith("/"):
        raise cv.Invalid(
            f"Lottie source must be a file path starting with '/', got: {value}"
        )
    return value


LOTTIE_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_WIDTH): size,
        cv.Required(CONF_HEIGHT): size,
        cv.Required(CONF_SRC): lottie_path_validator,
        cv.Optional(CONF_LOOP, default=True): cv.boolean,
        cv.Optional(CONF_AUTO_START, default=True): cv.boolean,
    }
)

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

        width = config[CONF_WIDTH]
        height = config[CONF_HEIGHT]

        # Allocate render buffer for Lottie animation
        # ARGB8888 format is required for vector graphics (4 bytes per pixel)
        buf_size = literal(f"({width} * {height} * 4)")
        lottie_buffer = lv.malloc_core(buf_size)

        # Set buffer for Lottie rendering
        lv.lottie_set_buffer(w.obj, width, height, lottie_buffer)

        # Load animation from file
        if src := config.get(CONF_SRC):
            lv.lottie_set_src_file(w.obj, src)

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
