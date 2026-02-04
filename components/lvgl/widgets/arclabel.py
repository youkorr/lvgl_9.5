"""
LVGL v9.4 Arc Label Widget Implementation for ESPHome

Supports:
- Text along an arc
- Dynamic text update
- Rotation
- Direction (clockwise/counter-clockwise)
- Vertical and horizontal alignment
- Offset
- Recolor
"""

import esphome.config_validation as cv
from esphome.const import CONF_TEXT, CONF_ROTATION

from ..defines import CONF_END_ANGLE, CONF_MAIN, CONF_RADIUS, CONF_START_ANGLE
from ..helpers import lvgl_components_required
from ..lv_validation import lv_angle_degrees, lv_int, lv_text, pixels
from ..lvcode import lv
from ..types import LvType
from . import Widget, WidgetType

CONF_ARCLABEL = "arclabel"
CONF_DIRECTION = "direction"
CONF_TEXT_VERTICAL_ALIGN = "text_vertical_align"
CONF_TEXT_HORIZONTAL_ALIGN = "text_horizontal_align"
CONF_OFFSET = "offset"
CONF_RECOLOR = "recolor"

lv_arclabel_t = LvType("lv_arclabel_t")

# -------------------------------------------------------------------
# Local validator: allow signed angles
# -------------------------------------------------------------------
SIGNED_ANGLE = cv.int_range(min=-360, max=360)

# Direction options
DIRECTION_OPTIONS = {
    "clockwise": lv.LV_ARCLABEL_DIR_CLOCKWISE,
    "counter_clockwise": lv.LV_ARCLABEL_DIR_COUNTER_CLOCKWISE,
}

# Vertical alignment options
VERTICAL_ALIGN_OPTIONS = {
    "top": lv.LV_ARCLABEL_TEXT_ALIGN_LEADING,
    "center": lv.LV_ARCLABEL_TEXT_ALIGN_CENTER,
    "bottom": lv.LV_ARCLABEL_TEXT_ALIGN_TRAILING,
}

# Horizontal alignment options
HORIZONTAL_ALIGN_OPTIONS = {
    "leading": lv.LV_ARCLABEL_TEXT_ALIGN_LEADING,
    "center": lv.LV_ARCLABEL_TEXT_ALIGN_CENTER,
    "trailing": lv.LV_ARCLABEL_TEXT_ALIGN_TRAILING,
}

# Arc label schema
ARCLABEL_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_TEXT): lv_text,
        cv.Optional(CONF_RADIUS, default=100): pixels,
        cv.Optional(CONF_START_ANGLE, default=0): SIGNED_ANGLE,
        cv.Optional(CONF_END_ANGLE, default=360): SIGNED_ANGLE,
        cv.Optional(CONF_ROTATION, default=0): SIGNED_ANGLE,
        cv.Optional(CONF_DIRECTION, default="clockwise"): cv.one_of(*DIRECTION_OPTIONS),
        cv.Optional(CONF_TEXT_VERTICAL_ALIGN, default="center"): cv.one_of(*VERTICAL_ALIGN_OPTIONS),
        cv.Optional(CONF_TEXT_HORIZONTAL_ALIGN, default="center"): cv.one_of(*HORIZONTAL_ALIGN_OPTIONS),
        cv.Optional(CONF_OFFSET, default=0): pixels,
        cv.Optional(CONF_RECOLOR, default=False): cv.boolean,
    }
)


class ArcLabelType(WidgetType):
    def __init__(self):
        super().__init__(
            CONF_ARCLABEL,
            lv_arclabel_t,
            (CONF_MAIN,),
            ARCLABEL_SCHEMA,
            modify_schema={cv.Optional(CONF_TEXT): lv_text},
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

        # Set angles
        start_angle = config.get(CONF_START_ANGLE, 0)
        end_angle = config.get(CONF_END_ANGLE, 360)
        rotation = config.get(CONF_ROTATION, 0)
        lv.arclabel_set_angle_size(w.obj, end_angle - start_angle)

        # Widget size
        widget_size = radius * 2 + 50
        lv.obj_set_size(w.obj, widget_size, widget_size)

        # Set rotation
        total_rotation = start_angle + rotation
        lv.obj_set_style_transform_rotation(w.obj, total_rotation * 10, 0)

        # Set direction
        lv.arclabel_set_dir(w.obj, DIRECTION_OPTIONS[config.get(CONF_DIRECTION, "clockwise")])

        # Set vertical & horizontal alignment
        lv.arclabel_set_text_vertical_align(w.obj, VERTICAL_ALIGN_OPTIONS[config.get(CONF_TEXT_VERTICAL_ALIGN, "center")])
        lv.arclabel_set_text_horizontal_align(w.obj, HORIZONTAL_ALIGN_OPTIONS[config.get(CONF_TEXT_HORIZONTAL_ALIGN, "center")])

        # Set offset
        offset = await pixels.process(config.get(CONF_OFFSET, 0))
        lv.arclabel_set_offset(w.obj, offset)

        # Set recolor
        lv.arclabel_set_recolor(w.obj, config.get(CONF_RECOLOR, False))

    async def to_code_update(self, w: Widget, config):
        """Allow updating text dynamically"""
        if CONF_TEXT in config:
            text = await lv_text.process(config[CONF_TEXT])
            lv.arclabel_set_text(w.obj, text)

    def get_uses(self):
        """Arc label uses label component"""
        return ("label",)


# Global instance
arclabel_spec = ArcLabelType()






























