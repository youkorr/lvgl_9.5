"""
LVGL v9.4 Arc Label Widget for ESPHome
"""

import esphome.config_validation as cv
from esphome.const import CONF_TEXT, CONF_ROTATION, CONF_COLOR
from ..defines import CONF_END_ANGLE, CONF_START_ANGLE, CONF_RADIUS, CONF_MAIN
from ..helpers import lvgl_components_required
from ..lv_validation import lv_angle_degrees, lv_text, pixels
from ..lvcode import lv
from ..types import LvType
from . import Widget, WidgetType

CONF_ARCLABEL = "arclabel"
CONF_DIRECTION = "direction"
CONF_TEXT_VERTICAL_ALIGN = "text_vertical_align"
CONF_TEXT_HORIZONTAL_ALIGN = "text_horizontal_align"
CONF_RECOLOR = "recolor"
CONF_TEXT_FONT = "text_font"

lv_arclabel_t = LvType("lv_arclabel_t")

SIGNED_ANGLE = cv.int_range(min=-360, max=360)

# Validator for directions and alignments
DIRECTION_OPTIONS = cv.enum({"clockwise": "clockwise", "counter_clockwise": "counter_clockwise"})
VERT_ALIGN_OPTIONS = cv.enum({"leading": "leading", "trailing": "trailing", "center": "center"})
HORIZ_ALIGN_OPTIONS = cv.enum({"leading": "leading", "trailing": "trailing", "center": "center"})

# Arc label schema
ARCLABEL_SCHEMA = cv.Schema({
    cv.Required(CONF_TEXT): lv_text,
    cv.Optional(CONF_RADIUS, default=100): pixels,
    cv.Optional(CONF_START_ANGLE, default=0): SIGNED_ANGLE,
    cv.Optional(CONF_END_ANGLE, default=360): SIGNED_ANGLE,
    cv.Optional(CONF_ROTATION, default=0): SIGNED_ANGLE,
    cv.Optional(CONF_TEXT_COLOR, default="black"): cv.color,
    cv.Optional(CONF_TEXT_FONT, default=None): cv.string,
    cv.Optional(CONF_DIRECTION, default="clockwise"): DIRECTION_OPTIONS,
    cv.Optional(CONF_TEXT_VERTICAL_ALIGN, default="center"): VERT_ALIGN_OPTIONS,
    cv.Optional(CONF_TEXT_HORIZONTAL_ALIGN, default="center"): HORIZ_ALIGN_OPTIONS,
    cv.Optional(CONF_RECOLOR, default=False): cv.boolean,
})

class ArcLabelType(WidgetType):
    def __init__(self):
        super().__init__(
            CONF_ARCLABEL,
            lv_arclabel_t,
            (CONF_MAIN,),
            ARCLABEL_SCHEMA,
            modify_schema={}
        )

    async def to_code(self, w: Widget, config):
        lvgl_components_required.add(CONF_ARCLABEL)

        # Text
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

        # Widget size
        widget_size = radius * 2 + 50
        lv.obj_set_size(w.obj, widget_size, widget_size)

        # Rotation
        lv.obj_set_style_transform_rotation(w.obj, (start_angle + rotation) * 10, 0)

        # Text color
        lv.obj_set_style_text_color(w.obj, config.get(CONF_TEXT_COLOR, "black"), 0)

        # Font
        font_name = config.get(CONF_TEXT_FONT)
        if font_name:
            # Direct mapping ESPHome LVGL fonts
            lv.obj_set_style_text_font(w.obj, getattr(lv, font_name), 0)

        # Direction
        direction = config.get(CONF_DIRECTION, "clockwise")
        if direction == "clockwise":
            lv.arclabel_set_dir(w.obj, lv.LV_ARCLABEL_DIR_CLOCKWISE)
        else:
            lv.arclabel_set_dir(w.obj, lv.LV_ARCLABEL_DIR_COUNTER_CLOCKWISE)

        # Vertical alignment
        vert_align = config.get(CONF_TEXT_VERTICAL_ALIGN, "center")
        vert_map = {
            "leading": lv.LV_ARCLABEL_TEXT_ALIGN_LEADING,
            "trailing": lv.LV_ARCLABEL_TEXT_ALIGN_TRAILING,
            "center": lv.LV_ARCLABEL_TEXT_ALIGN_CENTER,
        }
        lv.arclabel_set_text_vertical_align(w.obj, vert_map[vert_align])

        # Horizontal alignment
        horiz_align = config.get(CONF_TEXT_HORIZONTAL_ALIGN, "center")
        horiz_map = {
            "leading": lv.LV_ARCLABEL_TEXT_ALIGN_LEADING,
            "trailing": lv.LV_ARCLABEL_TEXT_ALIGN_TRAILING,
            "center": lv.LV_ARCLABEL_TEXT_ALIGN_CENTER,
        }
        lv.arclabel_set_text_horizontal_align(w.obj, horiz_map[horiz_align])

        # Recolor
        lv.arclabel_set_recolor(w.obj, config.get(CONF_RECOLOR, False))

    def get_uses(self):
        return ("label",)

arclabel_spec = ArcLabelType()








