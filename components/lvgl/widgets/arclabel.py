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

# Arc label schema
ARCLABEL_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_TEXT): lv_text,
        cv.Optional(CONF_RADIUS, default=100): pixels,
        cv.Optional(CONF_START_ANGLE, default=0): lv_signed_angle_degrees,
        cv.Optional(CONF_END_ANGLE, default=360): lv_signed_angle_degrees,
        cv.Optional(CONF_ROTATION, default=0): lv_signed_angle_degrees,
    }
)


class ArcLabelType(WidgetType):
    def __init__(self):
        super().__init__(
            CONF_ARCLABEL,
            lv_arclabel_t,
            (CONF_MAIN,),
            ARCLABEL_SCHEMA,
            modify_schema={},
        )

    async def to_code(self, w: Widget, config):
        """Generate C++ code for arc label widget configuration"""
        lvgl_components_required.add(CONF_ARCLABEL)

        # Set text
        text = await lv_text.process(config[CONF_TEXT])
        lv.arclabel_set_text(w.obj, text)

        # Set radius
        radius = await pixels.process(config[CONF_RADIUS])
        lv.arclabel_set_radius(w.obj, radius)

        # Set angle size (total arc span from start to end)
        # Note: LVGL arclabel doesn't have set_start_angle function
        # The arc always starts at 0°, we use rotation to position it
        start_angle = await lv_angle_degrees.process(config[CONF_START_ANGLE])
        end_angle = await lv_angle_degrees.process(config[CONF_END_ANGLE])
        angle_size = end_angle - start_angle
        lv.arclabel_set_angle_size(w.obj, angle_size)

        # Set widget size to contain the arc
        # Size should be at least 2*radius to fit the full arc
        widget_size = radius * 2 + 20  # Add padding
        lv.obj_set_size(w.obj, widget_size, widget_size)

        # Combine start_angle and rotation to get final rotation
        # The arc starts at 0° by default, so we rotate it to start_angle
        rotation = await lv_angle_degrees.process(config[CONF_ROTATION])
        total_rotation = start_angle + rotation
        # LVGL uses 0.1 degree units for rotation
        lv.obj_set_style_transform_rotation(w.obj, total_rotation * 10, 0)

    def get_uses(self):
        """Arc label uses label component"""
        return ("label",)


arclabel_spec = ArcLabelType()
