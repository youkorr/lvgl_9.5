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

lv_arclabel_t = LvType("lv_arclabel_t")

# -------------------------------------------------------------------
# Local validator: allow signed angles (do NOT touch lv_angle_degrees)
# -------------------------------------------------------------------


# Arc label schema
# Arc label schema
ARCLABEL_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_TEXT): lv_text,
        cv.Optional(CONF_RADIUS, default=100): pixels,
        cv.Optional(CONF_START_ANGLE, default=0): lv_angle_degrees,
        cv.Optional(CONF_END_ANGLE, default=360): lv_angle_degrees,
        cv.Optional(CONF_ROTATION, default=0): lv_angle_degrees,
    }



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

        # Set radius (use default if not provided)
        radius = await pixels.process(config.get(CONF_RADIUS, 100))
        lv.arclabel_set_radius(w.obj, radius)

        # Signed angles (use defaults if not provided)
        start_angle = config.get(CONF_START_ANGLE, 0)
        end_angle = config.get(CONF_END_ANGLE, 360)
        rotation = config.get(CONF_ROTATION, 0)

        # Arc size (span)
        angle_size = end_angle - start_angle
        lv.arclabel_set_angle_size(w.obj, angle_size)

        # Widget size
        widget_size = radius * 2 + 50  # padding
        lv.obj_set_size(w.obj, widget_size, widget_size)

        # Final rotation (LVGL uses 0.1° units)
        total_rotation = start_angle + rotation
        lv.obj_set_style_transform_rotation(w.obj, total_rotation * 10, 0)

    async def to_code_update(self, w: Widget, config):
        """Allow updating text dynamically via lvgl.arclabel.update"""
        if CONF_TEXT in config:
            text = await lv_text.process(config[CONF_TEXT])
            lv.arclabel_set_text(w.obj, text)

    def get_uses(self):
        """Arc label uses label component"""
        return ("label",)


# Global instance
arclabel_spec = ArcLabelType()











































