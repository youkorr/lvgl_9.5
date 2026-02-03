"""
LVGL v9.4 Arc Label Widget Implementation for ESPHome

Supports:
- Dynamic text updates
- Direction: clockwise / counter_clockwise
- Radial offset
- Vertical / horizontal alignment
- Recolor
- Custom font support via YAML
"""

import esphome.config_validation as cv
from esphome.const import CONF_TEXT, CONF_ROTATION

from ..defines import CONF_END_ANGLE, CONF_MAIN, CONF_RADIUS, CONF_START_ANGLE
from ..helpers import lvgl_components_required
from ..lv_validation import lv_text, lv_int, pixels
from ..lvcode import lv
from ..types import LvType
from . import Widget, WidgetType

# Constants
CONF_ARCLABEL = "arclabel"
CONF_DIRECTION = "direction"
CONF_OFFSET = "offset"
CONF_TEXT_VERTICAL_ALIGN = "text_vertical_align"
CONF_TEXT_HORIZONTAL_ALIGN = "text_horizontal_align"
CONF_RECOLOR = "recolor"
CONF_FONT = "font"

lv_arclabel_t = LvType("lv_arclabel_t")

# Validators
SIGNED_ANGLE = cv.int_range(min=-360, max=360)
DIRECTION = cv.enum({
    "clockwise": "clockwise",
    "counter_clockwise": "counter_clockwise"
})
VERT_ALIGN = cv.enum({
    "leading": "leading",
    "trailing": "trailing",
    "center": "center"
})
HORIZ_ALIGN = cv.enum({
    "left": "left",
    "right": "right",
    "center": "center"
})

# Schema
ARCLABEL_SCHEMA = cv.Schema({
    cv.Required(CONF_TEXT): lv_text,
    cv.Optional(CONF_RADIUS, default=100): pixels,
    cv.Optional(CONF_START_ANGLE, default=0): SIGNED_ANGLE,
    cv.Optional(CONF_END_ANGLE, default=360): SIGNED_ANGLE,
    cv.Optional(CONF_ROTATION, default=0): SIGNED_ANGLE,
    cv.Optional(CONF_DIRECTION, default="clockwise"): DIRECTION,
    cv.Optional(CONF_OFFSET, default=0): pixels,
    cv.Optional(CONF_TEXT_VERTICAL_ALIGN, default="center"): VERT_ALIGN,
    cv.Optional(CONF_TEXT_HORIZONTAL_ALIGN, default="center"): HORIZ_ALIGN,
    cv.Optional(CONF_RECOLOR, default=False): cv.boolean,
    cv.Optional(CONF_FONT): cv.string,  # font name as string
})


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
        lvgl_components_required.add(CONF_ARCLABEL)

        # --- Text ---
        text = await lv_text.process(config[CONF_TEXT])
        lv.arclabel_set_text(w.obj, text)

        # --- Radius ---
        radius = await pixels.process(config.get(CONF_RADIUS, 100))
        lv.arclabel_set_radius(w.obj, radius)

        # --- Angles ---
        start_angle = config.get(CONF_START_ANGLE, 0)
        end_angle = config.get(CONF_END_ANGLE, 360)
        rotation = config.get(CONF_ROTATION, 0)
        angle_size = end_angle - start_angle
        lv.arclabel_set_angle_size(w.obj, angle_size)

        # --- Direction ---
        direction = config.get(CONF_DIRECTION, "clockwise")
        if direction == "clockwise":
            lv.arclabel_set_dir(w.obj, lv.const("LV_ARCLABEL_DIR_CLOCKWISE"))
        else:
            lv.arclabel_set_dir(w.obj, lv.const("LV_ARCLABEL_DIR_COUNTER_CLOCKWISE"))

        # --- Offset ---
        offset = await pixels.process(config.get(CONF_OFFSET, 0))
        lv.arclabel_set_offset(w.obj, offset)

        # --- Vertical Align ---
        vert_align = config.get(CONF_TEXT_VERTICAL_ALIGN, "center")
        lv.arclabel_set_text_vertical_align(
            w.obj,
            {
                "leading": lv.const("LV_ARCLABEL_TEXT_ALIGN_LEADING"),
                "trailing": lv.const("LV_ARCLABEL_TEXT_ALIGN_TRAILING"),
                "center": lv.const("LV_ARCLABEL_TEXT_ALIGN_CENTER"),
            }[vert_align]
        )

        # --- Horizontal Align ---
        horiz_align = config.get(CONF_TEXT_HORIZONTAL_ALIGN, "center")
        lv.arclabel_set_text_horizontal_align(
            w.obj,
            {
                "left": lv.const("LV_ARCLABEL_TEXT_ALIGN_LEFT"),
                "right": lv.const("LV_ARCLABEL_TEXT_ALIGN_RIGHT"),
                "center": lv.const("LV_ARCLABEL_TEXT_ALIGN_CENTER"),
            }[horiz_align]
        )

        # --- Recolor ---
        recolor = config.get(CONF_RECOLOR, False)
        lv.arclabel_set_recolor(w.obj, recolor)

        # --- Font (custom from YAML) ---
        font_name = config.get(CONF_FONT)
        if font_name:
            # Expecting font imported in Python as `lv.font_myfont`
            font_expr = getattr(lv, font_name, None)
            if font_expr:
                lv.obj_set_style_text_font(w.obj, font_expr, lv.LV_PART_MAIN)

        # --- Widget size ---
        widget_size = radius * 2 + 50
        lv.obj_set_size(w.obj, widget_size, widget_size)

        # --- Rotation final ---
        total_rotation = start_angle + rotation
        lv.obj_set_style_transform_rotation(w.obj, total_rotation * 10, 0)

    async def to_code_update(self, w: Widget, config):
        """Support dynamic text update"""
        if CONF_TEXT in config:
            text = await lv_text.process(config[CONF_TEXT])
            lv.arclabel_set_text(w.obj, text)

    def get_uses(self):
        return ("label",)


# Global instance
arclabel_spec = ArcLabelType()







