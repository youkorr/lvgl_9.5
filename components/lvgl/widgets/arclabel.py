"""
LVGL v9.4 Arc Label Widget Implementation

The arc label widget displays text along a curved path (arc).
This is an advanced widget for circular/curved text displays.
"""

import esphome.config_validation as cv
from esphome.const import CONF_ROTATION, CONF_TEXT

from ..defines import (
    CONF_END_ANGLE,
    CONF_MAIN,
    CONF_RADIUS,
    CONF_START_ANGLE,
)
from ..helpers import lvgl_components_required
from ..lv_validation import lv_angle_degrees, lv_int, lv_text, pixels
from ..lvcode import lv
from ..types import LvType
from . import Widget, WidgetType

CONF_ARCLABEL = "arclabel"
CONF_DIRECTION = "direction"
CONF_TEXT_VERTICAL_ALIGN = "text_vertical_align"
CONF_TEXT_HORIZONTAL_ALIGN = "text_horizontal_align"

lv_arclabel_t = LvType("lv_arclabel_t")

# -------------------------------------------------------------------
# Local validator: allow signed angles
# -------------------------------------------------------------------
SIGNED_ANGLE = cv.int_range(min=-360, max=360)

# Direction validator
DIRECTION = cv.enum({
    "clockwise": lv.LV_ARCLABEL_DIR_CLOCKWISE,
    "counter_clockwise": lv.LV_ARCLABEL_DIR_COUNTER_CLOCKWISE,
})

# Text alignment validator
TEXT_ALIGN = cv.enum({
    "leading": lv.LV_ARCLABEL_TEXT_ALIGN_LEADING,
    "center": lv.LV_ARCLABEL_TEXT_ALIGN_CENTER,
    "trailing": lv.LV_ARCLABEL_TEXT_ALIGN_TRAILING,
})

# -------------------------------------------------------------------
# Arc label schema
# -------------------------------------------------------------------
ARCLABEL_SCHEMA = cv.Schema({
    cv.Required(CONF_TEXT): lv_text,
    cv.Optional(CONF_RADIUS, default=100): pixels,
    cv.Optional(CONF_START_ANGLE, default=0): SIGNED_ANGLE,
    cv.Optional(CONF_END_ANGLE, default=360): SIGNED_ANGLE,
    cv.Optional(CONF_ROTATION, default=0): SIGNED_ANGLE,
    cv.Optional(CONF_DIRECTION, default="clockwise"): DIRECTION,
    cv.Optional(CONF_TEXT_VERTICAL_ALIGN, default="center"): TEXT_ALIGN,
    cv.Optional(CONF_TEXT_HORIZONTAL_ALIGN, default="center"): TEXT_ALIGN,
})

# -------------------------------------------------------------------
# WidgetType
# -------------------------------------------------------------------
class ArcLabelType(WidgetType):
    def __init__(self):
        super().__init__(
            CONF_ARCLABEL,
            lv_arclabel_t,
            (CONF_MAIN,),
            ARCLABEL_SCHEMA,
            modify_schema={
                cv.Optional(CONF_TEXT): lv_text,
            },
        )

    async def to_code(self, w: Widget, config):
        """Generate C++ code for arc label widget configuration"""
        lvgl_components_required.add(CONF_ARCLABEL)

        # Set text
        text = await lv_text.process(config[CONF_TEXT])
        lv.arclabel_set_text(w.obj, text)

        # Set radius
        radius = await pixels.process(config.get(CONF_RADIUS, 100))
        lv.arclabel_set_radius(w.obj, radius)

        # Signed angles
        start_angle = config.get(CONF_START_ANGLE, 0)
        end_angle = config.get(CONF_END_ANGLE, 360)
        rotation = config.get(CONF_ROTATION, 0)

        # Arc size
        angle_size = end_angle - start_angle
        lv.arclabel_set_angle_size(w.obj, angle_size)

        # Widget size
        widget_size = radius * 2 + 50
        lv.obj_set_size(w.obj, widget_size, widget_size)

        # Total rotation
        total_rotation = start_angle + rotation
        lv.obj_set_style_transform_rotation(w.obj, total_rotation * 10, 0)

        # Direction
        lv.arclabel_set_dir(
            w.obj,
            lv.const(config.get(CONF_DIRECTION, lv.LV_ARCLABEL_DIR_CLOCKWISE))
        )

        # Text alignment
        lv.arclabel_set_text_vertical_align(
            w.obj,
            lv.const(config.get(CONF_TEXT_VERTICAL_ALIGN, lv.LV_ARCLABEL_TEXT_ALIGN_CENTER))
        )
        lv.arclabel_set_text_horizontal_align(
            w.obj,
            lv.const(config.get(CONF_TEXT_HORIZONTAL_ALIGN, lv.LV_ARCLABEL_TEXT_ALIGN_CENTER))
        )

    async def to_code_update(self, w: Widget, config):
        """Allow updating text dynamically via lvgl.arclabel.update"""
        if CONF_TEXT in config:
            text = await lv_text.process(config[CONF_TEXT])
            lv.arclabel_set_text(w.obj, text)

    def get_uses(self):
        """Arc label uses label component"""
        return ("label",)


# -------------------------------------------------------------------
# Global instance
# -------------------------------------------------------------------
arclabel_spec = ArcLabelType()





























