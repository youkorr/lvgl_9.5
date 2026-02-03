"""
LVGL ArcLabel Widget for ESPHome 2026+

Supports:
- text (set at creation and dynamically via lambda)
- radius
- start/end angles
- rotation
- direction (clockwise/counter_clockwise)
- vertical/horizontal align
- recolor
- offset
- text_color
"""

import esphome.config_validation as cv
from esphome.const import CONF_ROTATION, CONF_TEXT, CONF_ID
from ..helpers import lvgl_components_required
from ..lv_validation import lv_angle_degrees, lv_text, pixels
from ..lvcode import lv
from ..types import LvType
from . import Widget, WidgetType

# -------------------------------------------------------------------
# Constants
# -------------------------------------------------------------------
CONF_ARCLABEL = "arclabel"
CONF_RADIUS = "radius"
CONF_START_ANGLE = "start_angle"
CONF_END_ANGLE = "end_angle"
CONF_DIRECTION = "direction"
CONF_TEXT_VERTICAL_ALIGN = "text_vertical_align"
CONF_TEXT_HORIZONTAL_ALIGN = "text_horizontal_align"
CONF_RECOLOR = "recolor"
CONF_TEXT_COLOR = "text_color"
CONF_OFFSET = "offset"

lv_arclabel_t = LvType("lv_arclabel_t")

# -------------------------------------------------------------------
# Validators
# -------------------------------------------------------------------
SIGNED_ANGLE = cv.int_range(min=-360, max=360)

DIRECTION = cv.enum({
    "clockwise": lv.LV_ARCLABEL_DIR_CLOCKWISE,
    "counter_clockwise": lv.LV_ARCLABEL_DIR_COUNTER_CLOCKWISE
})

TEXT_ALIGN = cv.enum({
    "leading": lv.LV_ARCLABEL_TEXT_ALIGN_LEADING,
    "center": lv.LV_ARCLABEL_TEXT_ALIGN_CENTER,
    "trailing": lv.LV_ARCLABEL_TEXT_ALIGN_TRAILING
})

# -------------------------------------------------------------------
# ArcLabel schema
# -------------------------------------------------------------------
ARCLABEL_SCHEMA = cv.Schema({
    cv.Required(CONF_TEXT): lv_text,
    cv.Optional(CONF_RADIUS, default=100): pixels,
    cv.Optional(CONF_START_ANGLE, default=0): SIGNED_ANGLE,
    cv.Optional(CONF_END_ANGLE, default=360): SIGNED_ANGLE,
    cv.Optional(CONF_ROTATION, default=0): SIGNED_ANGLE,
    cv.Optional(CONF_TEXT_COLOR, default=0xFFFFFF): cv.color,  # <- correct for ESPHome 2026
    cv.Optional(CONF_DIRECTION, default="clockwise"): DIRECTION,
    cv.Optional(CONF_TEXT_VERTICAL_ALIGN, default="center"): TEXT_ALIGN,
    cv.Optional(CONF_TEXT_HORIZONTAL_ALIGN, default="center"): TEXT_ALIGN,
    cv.Optional(CONF_RECOLOR, default=False): cv.boolean,
    cv.Optional(CONF_OFFSET, default=0): cv.int_,
})

# -------------------------------------------------------------------
# WidgetType
# -------------------------------------------------------------------
class ArcLabelType(WidgetType):
    def __init__(self):
        super().__init__(
            CONF_ARCLABEL,
            lv_arclabel_t,
            (CONF_ID,),
            ARCLABEL_SCHEMA,
            modify_schema={}
        )

    async def to_code(self, w: Widget, config):
        """Generate C++ code for arc label widget configuration"""
        lvgl_components_required.add(CONF_ARCLABEL)

        # Set text (mandatory)
        text = await lv_text.process(config[CONF_TEXT])
        lv.arclabel_set_text(w.obj, text)

        # Radius
        radius = await pixels.process(config.get(CONF_RADIUS, 100))
        lv.arclabel_set_radius(w.obj, radius)

        # Angles
        start_angle = config.get(CONF_START_ANGLE, 0)
        end_angle = config.get(CONF_END_ANGLE, 360)
        rotation = config.get(CONF_ROTATION, 0)
        lv.arclabel_set_angle_size(w.obj, end_angle - start_angle)

        # Widget size (ensure enough room)
        lv.obj_set_size(w.obj, radius * 2 + 50, radius * 2 + 50)

        # Total rotation
        lv.obj_set_style_transform_rotation(w.obj, (start_angle + rotation) * 10, 0)

        # Direction
        lv.arclabel_set_dir(w.obj, lv.const(config.get(CONF_DIRECTION, lv.LV_ARCLABEL_DIR_CLOCKWISE)))

        # Alignments
        lv.arclabel_set_text_vertical_align(
            w.obj, lv.const(config.get(CONF_TEXT_VERTICAL_ALIGN, lv.LV_ARCLABEL_TEXT_ALIGN_CENTER)))
        lv.arclabel_set_text_horizontal_align(
            w.obj, lv.const(config.get(CONF_TEXT_HORIZONTAL_ALIGN, lv.LV_ARCLABEL_TEXT_ALIGN_CENTER)))

        # Recolor
        lv.arclabel_set_recolor(w.obj, config.get(CONF_RECOLOR, False))

        # Offset
        lv.arclabel_set_offset(w.obj, config.get(CONF_OFFSET, 0))

        # Text color
        lv.obj_set_style_text_color(w.obj, config.get(CONF_TEXT_COLOR), 0)

    def get_uses(self):
        """Arc label uses label component"""
        return ("label",)

# -------------------------------------------------------------------
# Register widget
# -------------------------------------------------------------------
arclabel_spec = ArcLabelType()

















