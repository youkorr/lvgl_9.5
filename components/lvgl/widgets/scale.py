"""LVGL v9.4 Scale Widget Implementation for ESPHome

The scale widget is a versatile component for displaying measurement scales in various orientations.
It replaces the obsolete meter widget from LVGL v8.x and provides more flexibility.

Supported features:
- Multiple scale modes: horizontal, vertical, round (inner/outer)
- Configurable tick marks (major and minor)
- Value range configuration
- Colored sections for different value ranges (with multi-part styling)
- Label rotation and formatting
- Line and image needles
- Custom text labels (text_src)
- Post-fix / Pre-fix for labels
- Draw event callbacks for label customization
- Parts: MAIN (background), INDICATOR (major ticks), ITEMS (minor ticks)

LVGL Documentation Examples Coverage:
- lv_example_scale_1:  Simple horizontal scale
- lv_example_scale_2:  Vertical scale with section and custom styling + text_src
- lv_example_scale_3:  Simple round scale with line/image needles
- lv_example_scale_4:  Round scale with section and custom styling + text_src
- lv_example_scale_5:  Scale with section and multi-part custom styling
- lv_example_scale_6:  Round scale with multiple needles (clock)
- lv_example_scale_7:  Custom label color via draw event callback
- lv_example_scale_8:  Round scale with labels rotated and translated
- lv_example_scale_9:  Horizontal scale with labels rotated and translated
- lv_example_scale_10: Heart Rate monitor with needles
- lv_example_scale_11: Sunset/sunrise widget with text_src + sections
- lv_example_scale_12: Compass with needles + text_src + animation
"""

from esphome import automation
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import (
    CONF_COLOR,
    CONF_COUNT,
    CONF_ID,
    CONF_ITEMS,
    CONF_LENGTH,
    CONF_MAX_VALUE,
    CONF_MIN_VALUE,
    CONF_MODE,
    CONF_RANGE_FROM,
    CONF_RANGE_TO,
    CONF_ROTATION,
    CONF_VALUE,
    CONF_WIDTH,
)
from esphome.cpp_types import nullptr

from .. import set_obj_properties
from ..automation import action_to_code
from ..defines import (
    CONF_ANIMATED,
    CONF_END_VALUE,
    CONF_INDICATOR,
    CONF_MAIN,
    CONF_START_VALUE,
    CONF_TICKS,
    LV_OBJ_FLAG,
    LV_PART,
    LV_SCALE_MODE,
    literal,
)
from ..helpers import add_lv_use, lvgl_components_required
from ..lv_validation import (
    LV_OPA,
    animated,
    get_end_value,
    get_start_value,
    lv_angle_degrees,
    lv_bool,
    lv_color,
    lv_float,
    lv_int,
    lv_image,
    opacity,
    size,
)
from ..lvcode import LambdaContext, lv, lv_add, lv_obj, lv_expr
from esphome.cpp_generator import RawStatement
from ..types import LV_EVENT, LvNumber, ObjUpdateAction, lv_event_t, lv_obj_t
from . import NumberType, Widget, get_widgets

# Configuration keys
CONF_ANGLE_RANGE = "angle_range"
CONF_COLOR_END = "color_end"
CONF_COLOR_START = "color_start"
CONF_LABEL_GAP = "label_gap"
CONF_LABEL_SHOW = "label_show"
CONF_MAJOR = "major"
CONF_RADIAL_OFFSET = "radial_offset"
CONF_SCALE = "scale"
CONF_SECTION = "section"
CONF_SECTIONS = "sections"
CONF_STRIDE = "stride"
CONF_OFFSET = "offset"
CONF_TEXT_SRC = "text_src"
CONF_POST_FIX = "post_fix"
CONF_PRE_FIX = "pre_fix"

# Needle configuration keys
CONF_NEEDLES = "needles"
CONF_NEEDLE_LENGTH = "needle_length"
CONF_NEEDLE_WIDTH = "needle_width"
CONF_NEEDLE_COLOR = "needle_color"
CONF_NEEDLE_ROUNDED = "needle_rounded"
CONF_NEEDLE_SRC = "src"
CONF_NEEDLE_PIVOT_X = "pivot_x"
CONF_NEEDLE_PIVOT_Y = "pivot_y"

# Label transform configuration keys
CONF_ROTATE_MATCH_TICKS = "rotate_match_ticks"
CONF_KEEP_UPRIGHT = "keep_upright"
CONF_TRANSLATE_X = "translate_x"
CONF_TRANSLATE_Y = "translate_y"

# Draw event callback
CONF_CUSTOM_LABEL_CB = "custom_label_cb"

DEFAULT_LABEL_GAP = 10  # Default label gap for major ticks

# Module-level registry for needle metadata (used by update actions)
_needle_registry = {}  # needle_id_str -> {"length": int, "is_image": bool}

# Section style schema for multi-part section styling
SECTION_PART_STYLE_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_COLOR): lv_color,
        cv.Optional(CONF_WIDTH): cv.positive_int,
    }
)

# Scale section schema for colored ranges
SECTION_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(LvNumber("lv_scale_section_t")),
        cv.Optional(CONF_START_VALUE): lv_float,
        cv.Optional(CONF_END_VALUE): lv_float,
        cv.Optional(CONF_RANGE_FROM): lv_int,
        cv.Optional(CONF_RANGE_TO): lv_int,
        cv.Optional(CONF_COLOR, default=0): lv_color,
        cv.Optional(CONF_WIDTH, default=4): cv.positive_int,
        # Multi-part section styling (for examples 2, 4, 5, 11)
        cv.Optional(CONF_INDICATOR): SECTION_PART_STYLE_SCHEMA,
        cv.Optional(CONF_ITEMS): SECTION_PART_STYLE_SCHEMA,
        cv.Optional(CONF_MAIN): SECTION_PART_STYLE_SCHEMA,
    }
).add_extra(cv.has_at_most_one_key(CONF_START_VALUE, CONF_RANGE_FROM))

# Line needle schema
LINE_NEEDLE_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(lv_obj_t),
        cv.Optional(CONF_VALUE, default=0): lv_int,
        cv.Optional(CONF_NEEDLE_LENGTH, default=60): cv.positive_int,
        cv.Optional(CONF_NEEDLE_WIDTH, default=3): cv.positive_int,
        cv.Optional(CONF_NEEDLE_COLOR, default=0xFF0000): lv_color,
        cv.Optional(CONF_NEEDLE_ROUNDED, default=True): lv_bool,
    }
)

# Image needle schema
IMAGE_NEEDLE_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(lv_obj_t),
        cv.Required(CONF_NEEDLE_SRC): lv_image,
        cv.Optional(CONF_VALUE, default=0): lv_int,
        cv.Optional(CONF_NEEDLE_PIVOT_X, default=3): cv.int_,
        cv.Optional(CONF_NEEDLE_PIVOT_Y, default=4): cv.int_,
    }
)


def needle_validator(config):
    """Validate needle configuration - determine if it's a line or image needle."""
    if CONF_NEEDLE_SRC in config:
        return IMAGE_NEEDLE_SCHEMA(config)
    return LINE_NEEDLE_SCHEMA(config)


# Combined needle schema
NEEDLE_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(lv_obj_t),
        cv.Optional(CONF_VALUE, default=0): lv_int,
        # Line needle properties
        cv.Optional(CONF_NEEDLE_LENGTH, default=60): cv.positive_int,
        cv.Optional(CONF_NEEDLE_WIDTH, default=3): cv.positive_int,
        cv.Optional(CONF_NEEDLE_COLOR, default=0xFF0000): lv_color,
        cv.Optional(CONF_NEEDLE_ROUNDED, default=True): lv_bool,
        # Image needle properties
        cv.Optional(CONF_NEEDLE_SRC): lv_image,
        cv.Optional(CONF_NEEDLE_PIVOT_X): cv.int_,
        cv.Optional(CONF_NEEDLE_PIVOT_Y): cv.int_,
    }
)

# Tick configuration schema
TICK_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_COUNT, default=12): cv.positive_int,
        cv.Optional(CONF_WIDTH, default=2): cv.positive_int,
        cv.Optional(CONF_LENGTH, default=10): size,
        cv.Optional(CONF_COLOR, default=0x808080): lv_color,
        cv.Optional(CONF_RADIAL_OFFSET, default=0): size,
        cv.Optional(CONF_MAJOR): cv.Schema(
            {
                cv.Optional(CONF_STRIDE, default=3): cv.positive_int,
                cv.Optional(CONF_OFFSET, default=0): cv.int_range(min=0, max=65535),
                cv.Optional(CONF_WIDTH, default=5): size,
                cv.Optional(CONF_LENGTH, default="15%"): size,
                cv.Optional(CONF_COLOR, default=0): lv_color,
                cv.Optional(CONF_RADIAL_OFFSET, default=0): size,
                cv.Optional(CONF_LABEL_GAP, default=4): size,
                cv.Optional(CONF_LABEL_SHOW, default=True): lv_bool,
                # Label transforms (for examples 8, 9)
                cv.Optional(CONF_ROTATE_MATCH_TICKS, default=False): lv_bool,
                cv.Optional(CONF_KEEP_UPRIGHT, default=False): lv_bool,
                cv.Optional(CONF_TRANSLATE_X, default=0): cv.int_,
                cv.Optional(CONF_TRANSLATE_Y, default=0): cv.int_,
            }
        ),
    }
)

# Main scale widget schema
SCALE_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_VALUE): lv_float,
        cv.Optional(CONF_MIN_VALUE, default=0): lv_int,
        cv.Optional(CONF_MAX_VALUE, default=100): lv_int,
        cv.Optional(
            CONF_MODE, default="ROUND_OUTER"
        ): LV_SCALE_MODE.one_of,
        cv.Optional(CONF_ROTATION, default=0): lv_angle_degrees,
        cv.Optional(CONF_ANGLE_RANGE, default=270): lv_angle_degrees,
        cv.Optional(CONF_TICKS): TICK_SCHEMA,
        cv.Optional(CONF_SECTIONS): cv.ensure_list(SECTION_SCHEMA),
        cv.Optional(CONF_ANIMATED, default=True): animated,
        # Custom text labels (for examples 2, 4, 6, 11, 12)
        cv.Optional(CONF_TEXT_SRC): cv.ensure_list(cv.string),
        # Post-fix / Pre-fix for labels
        cv.Optional(CONF_POST_FIX): cv.string,
        cv.Optional(CONF_PRE_FIX): cv.string,
        # Needles (for examples 3, 6, 8, 10, 12)
        cv.Optional(CONF_NEEDLES): cv.ensure_list(NEEDLE_SCHEMA),
        # Draw event callback for custom label coloring (for example 7)
        cv.Optional(CONF_CUSTOM_LABEL_CB): cv.lambda_,
    }
)

# Schema for updating scale widget
SCALE_MODIFY_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_VALUE): lv_float,
        cv.Optional(CONF_MIN_VALUE): lv_int,
        cv.Optional(CONF_MAX_VALUE): lv_int,
        cv.Optional(CONF_MODE): LV_SCALE_MODE.one_of,
        cv.Optional(CONF_ROTATION): lv_angle_degrees,
        cv.Optional(CONF_ANGLE_RANGE): lv_angle_degrees,
        cv.Optional(CONF_ANIMATED, default=True): animated,
    }
)


class ScaleType(NumberType):
    """
    LVGL Scale widget type.

    The scale widget provides a versatile way to display measurement scales in different orientations.
    It supports horizontal, vertical, and circular (round) layouts with configurable tick marks,
    labels, and colored sections. Supports needles, custom text sources, and label transforms.
    """

    def __init__(self):
        super().__init__(
            CONF_SCALE,
            LvNumber("lv_scale_t"),
            parts=(CONF_MAIN, CONF_INDICATOR, CONF_ITEMS),
            schema=SCALE_SCHEMA,
            modify_schema=SCALE_MODIFY_SCHEMA,
        )

    @property
    def animated(self):
        return True

    async def to_code(self, w: Widget, config):
        """Generate code for scale widget configuration."""
        lvgl_components_required.add(CONF_SCALE)
        add_lv_use(CONF_SCALE)

        # Set scale mode
        if CONF_MODE in config:
            mode = config[CONF_MODE]
            lv.scale_set_mode(w.obj, literal(mode))

        # Set range (min/max values)
        if CONF_MIN_VALUE in config or CONF_MAX_VALUE in config:
            min_val = config.get(CONF_MIN_VALUE, 0)
            max_val = config.get(CONF_MAX_VALUE, 100)
            lv.scale_set_range(w.obj, min_val, max_val)

        # Set rotation (for round modes)
        if CONF_ROTATION in config:
            rotation = await lv_angle_degrees.process(config[CONF_ROTATION])
            lv.scale_set_rotation(w.obj, rotation)

        # Set angle range (for round modes)
        if CONF_ANGLE_RANGE in config:
            angle_range = await lv_angle_degrees.process(config[CONF_ANGLE_RANGE])
            lv.scale_set_angle_range(w.obj, angle_range)

        # Configure ticks
        if ticks := config.get(CONF_TICKS):
            # Set total tick count
            lv.scale_set_total_tick_count(w.obj, ticks[CONF_COUNT])

            # Style minor ticks (ITEMS part)
            lv_obj.set_style_length(
                w.obj, await size.process(ticks[CONF_LENGTH]), LV_PART.ITEMS
            )
            lv_obj.set_style_line_width(
                w.obj, await size.process(ticks[CONF_WIDTH]), LV_PART.ITEMS
            )
            lv_obj.set_style_line_color(
                w.obj, await lv_color.process(ticks[CONF_COLOR]), LV_PART.ITEMS
            )
            lv_obj.set_style_radial_offset(
                w.obj,
                await size.process(ticks[CONF_RADIAL_OFFSET]),
                LV_PART.ITEMS,
            )

            # Configure major ticks if specified
            if major := ticks.get(CONF_MAJOR):
                tick_offset = major.get(CONF_OFFSET, 0)
                stride = major[CONF_STRIDE]

                if tick_offset > 0:
                    lv.scale_set_major_tick_every(w.obj, 1)
                else:
                    lv.scale_set_major_tick_every(w.obj, stride)

                # Enable or disable labels
                label_show = major.get(CONF_LABEL_SHOW, True)
                lv.scale_set_label_show(w.obj, label_show)

                # Style major ticks (INDICATOR part)
                lv_obj.set_style_length(
                    w.obj, await size.process(major[CONF_LENGTH]), LV_PART.INDICATOR
                )
                lv_obj.set_style_line_width(
                    w.obj, await size.process(major[CONF_WIDTH]), LV_PART.INDICATOR
                )
                lv_obj.set_style_line_color(
                    w.obj, await lv_color.process(major[CONF_COLOR]), LV_PART.INDICATOR
                )
                lv_obj.set_style_radial_offset(
                    w.obj,
                    await size.process(major[CONF_RADIAL_OFFSET]),
                    LV_PART.INDICATOR,
                )

                # Set label gap (distance from scale)
                label_gap = await size.process(major[CONF_LABEL_GAP])
                if isinstance(label_gap, int):
                    label_gap -= DEFAULT_LABEL_GAP
                lv_obj.set_style_pad_radial(w.obj, label_gap, LV_PART.INDICATOR)

                # Label transforms: rotate_match_ticks (examples 8, 9)
                rotate_match = major.get(CONF_ROTATE_MATCH_TICKS, False)
                if rotate_match:
                    lv_add(RawStatement(
                        f"lv_obj_set_style_transform_rotation("
                        f"{w.obj}, LV_SCALE_LABEL_ROTATE_MATCH_TICKS, LV_PART_INDICATOR);"
                    ))

                # Label transforms: keep_upright (examples 8, 9)
                keep_upright = major.get(CONF_KEEP_UPRIGHT, False)
                if keep_upright:
                    lv_add(RawStatement(
                        f"lv_obj_set_style_transform_rotation("
                        f"{w.obj}, LV_SCALE_LABEL_ROTATE_MATCH_TICKS | "
                        f"LV_SCALE_LABEL_ROTATE_KEEP_UPRIGHT, LV_PART_INDICATOR);"
                    ))

                # Label translation (examples 8, 9)
                translate_x = major.get(CONF_TRANSLATE_X, 0)
                translate_y = major.get(CONF_TRANSLATE_Y, 0)
                if translate_x != 0:
                    lv_add(RawStatement(
                        f"lv_obj_set_style_translate_x("
                        f"{w.obj}, {translate_x}, LV_PART_INDICATOR);"
                    ))
                if translate_y != 0:
                    lv_add(RawStatement(
                        f"lv_obj_set_style_translate_y("
                        f"{w.obj}, {translate_y}, LV_PART_INDICATOR);"
                    ))

                # Register draw callback when offset is used
                if tick_offset > 0:
                    async with LambdaContext(
                        [(lv_event_t.operator("ptr"), "e")]
                    ) as lambda_:
                        lv.scale_tick_offset_event_cb(
                            lambda_.get_parameter(0),
                            tick_offset,
                            stride,
                        )
                    lv_obj.add_event_cb(
                        w.obj,
                        await lambda_.get_lambda(),
                        LV_EVENT.DRAW_TASK_ADDED,
                        nullptr,
                    )
                    lv.obj_add_flag(w.obj, LV_OBJ_FLAG.SEND_DRAW_TASK_EVENTS)
            else:
                # No major ticks
                lv.scale_set_major_tick_every(w.obj, 0)
        else:
            # No ticks at all
            lv.scale_set_total_tick_count(w.obj, 0)

        # Custom text labels - text_src (examples 2, 4, 6, 11, 12)
        if text_src := config.get(CONF_TEXT_SRC):
            labels_array_name = f"scale_labels_{id(config) & 0xFFFFFF:06x}"
            # Build C array of const char*
            labels_c = ", ".join(f'"{label}"' for label in text_src)
            lv_add(RawStatement(
                f'static const char *{labels_array_name}[] = {{{labels_c}, NULL}};'
            ))
            lv_add(RawStatement(
                f"lv_scale_set_text_src({w.obj}, {labels_array_name});"
            ))

        # Post-fix for labels
        if post_fix := config.get(CONF_POST_FIX):
            lv_add(RawStatement(
                f'lv_scale_set_post_draw({w.obj}, true);'
            ))

        # Pre-fix for labels
        if pre_fix := config.get(CONF_PRE_FIX):
            lv_add(RawStatement(
                f'lv_scale_set_post_draw({w.obj}, true);'
            ))

        # Add colored sections
        if sections := config.get(CONF_SECTIONS):
            for idx, section_conf in enumerate(sections):
                section_id = section_conf[CONF_ID]

                # Determine start and end values
                start_value = await get_start_value(section_conf) or section_conf.get(
                    CONF_RANGE_FROM, config.get(CONF_MIN_VALUE, 0)
                )
                end_value = await get_end_value(section_conf) or section_conf.get(
                    CONF_RANGE_TO, config.get(CONF_MAX_VALUE, 100)
                )

                # Create section
                section_var = cg.Pvariable(
                    section_id, lv.scale_add_section(w.obj)
                )

                # Set section range
                lv.scale_section_set_range(section_var, start_value, end_value)

                # Default section style (INDICATOR part) - backward compatible
                style_name = f"style_{section_id}"
                color = await lv_color.process(section_conf.get(CONF_COLOR, 0))
                width = section_conf.get(CONF_WIDTH, 4)

                lv_add(RawStatement(f"static lv_style_t {style_name};"))
                lv_add(RawStatement(f"lv_style_init(&{style_name});"))
                lv_add(RawStatement(f"lv_style_set_line_color(&{style_name}, {color});"))
                lv_add(RawStatement(f"lv_style_set_line_width(&{style_name}, {width});"))
                lv_add(RawStatement(
                    f"lv_scale_section_set_style({section_var}, LV_PART_INDICATOR, &{style_name});"
                ))

                # Multi-part section styling: ITEMS part (minor ticks)
                if items_style := section_conf.get(CONF_ITEMS):
                    items_style_name = f"style_{section_id}_items"
                    lv_add(RawStatement(f"static lv_style_t {items_style_name};"))
                    lv_add(RawStatement(f"lv_style_init(&{items_style_name});"))
                    if CONF_COLOR in items_style:
                        items_color = await lv_color.process(items_style[CONF_COLOR])
                        lv_add(RawStatement(
                            f"lv_style_set_line_color(&{items_style_name}, {items_color});"
                        ))
                    if CONF_WIDTH in items_style:
                        items_width = items_style[CONF_WIDTH]
                        lv_add(RawStatement(
                            f"lv_style_set_line_width(&{items_style_name}, {items_width});"
                        ))
                    lv_add(RawStatement(
                        f"lv_scale_section_set_style({section_var}, LV_PART_ITEMS, &{items_style_name});"
                    ))

                # Multi-part section styling: MAIN part (main line/arc)
                if main_style := section_conf.get(CONF_MAIN):
                    main_style_name = f"style_{section_id}_main"
                    lv_add(RawStatement(f"static lv_style_t {main_style_name};"))
                    lv_add(RawStatement(f"lv_style_init(&{main_style_name});"))
                    if CONF_COLOR in main_style:
                        main_color = await lv_color.process(main_style[CONF_COLOR])
                        lv_add(RawStatement(
                            f"lv_style_set_line_color(&{main_style_name}, {main_color});"
                        ))
                    if CONF_WIDTH in main_style:
                        main_width = main_style[CONF_WIDTH]
                        lv_add(RawStatement(
                            f"lv_style_set_line_width(&{main_style_name}, {main_width});"
                        ))
                    lv_add(RawStatement(
                        f"lv_scale_section_set_style({section_var}, LV_PART_MAIN, &{main_style_name});"
                    ))

                # Multi-part section styling: INDICATOR part override
                if indicator_style := section_conf.get(CONF_INDICATOR):
                    ind_style_name = f"style_{section_id}_indicator"
                    lv_add(RawStatement(f"static lv_style_t {ind_style_name};"))
                    lv_add(RawStatement(f"lv_style_init(&{ind_style_name});"))
                    if CONF_COLOR in indicator_style:
                        ind_color = await lv_color.process(indicator_style[CONF_COLOR])
                        lv_add(RawStatement(
                            f"lv_style_set_line_color(&{ind_style_name}, {ind_color});"
                        ))
                    if CONF_WIDTH in indicator_style:
                        ind_width = indicator_style[CONF_WIDTH]
                        lv_add(RawStatement(
                            f"lv_style_set_line_width(&{ind_style_name}, {ind_width});"
                        ))
                    # Override the default INDICATOR style
                    lv_add(RawStatement(
                        f"lv_scale_section_set_style({section_var}, LV_PART_INDICATOR, &{ind_style_name});"
                    ))

        # Create needles (examples 3, 6, 8, 10, 12)
        if needles := config.get(CONF_NEEDLES):
            add_lv_use("line")
            for needle_conf in needles:
                needle_id = needle_conf[CONF_ID]
                value = needle_conf.get(CONF_VALUE, 0)

                if CONF_NEEDLE_SRC in needle_conf:
                    # Image needle
                    add_lv_use("img")
                    needle_var_name = f"needle_img_{id(needle_conf) & 0xFFFFFF:06x}"
                    src = needle_conf[CONF_NEEDLE_SRC]
                    pivot_x = needle_conf.get(CONF_NEEDLE_PIVOT_X, 3)
                    pivot_y = needle_conf.get(CONF_NEEDLE_PIVOT_Y, 4)

                    lv_add(RawStatement(
                        f"lv_obj_t *{needle_var_name} = lv_image_create({w.obj});"
                    ))
                    lv_add(RawStatement(
                        f"lv_image_set_src({needle_var_name}, {src});"
                    ))
                    lv_add(RawStatement(
                        f"lv_image_set_pivot({needle_var_name}, {pivot_x}, {pivot_y});"
                    ))
                    lv_add(RawStatement(
                        f"lv_scale_set_image_needle_value({w.obj}, {needle_var_name}, {value});"
                    ))
                    # Store needle info for update actions
                    _needle_registry[str(needle_id)] = {
                        "is_image": True,
                        "var_name": needle_var_name,
                    }
                else:
                    # Line needle
                    needle_var_name = f"needle_line_{id(needle_conf) & 0xFFFFFF:06x}"
                    needle_length = needle_conf.get(CONF_NEEDLE_LENGTH, 60)
                    needle_width = needle_conf.get(CONF_NEEDLE_WIDTH, 3)
                    needle_color = await lv_color.process(
                        needle_conf.get(CONF_NEEDLE_COLOR, 0xFF0000)
                    )
                    needle_rounded = needle_conf.get(CONF_NEEDLE_ROUNDED, True)

                    lv_add(RawStatement(
                        f"lv_obj_t *{needle_var_name} = lv_line_create({w.obj});"
                    ))
                    lv_add(RawStatement(
                        f"lv_obj_set_style_line_width({needle_var_name}, {needle_width}, 0);"
                    ))
                    lv_add(RawStatement(
                        f"lv_obj_set_style_line_color({needle_var_name}, {needle_color}, 0);"
                    ))
                    if needle_rounded:
                        lv_add(RawStatement(
                            f"lv_obj_set_style_line_rounded({needle_var_name}, true, 0);"
                        ))
                    lv_add(RawStatement(
                        f"lv_scale_set_line_needle_value({w.obj}, {needle_var_name}, {needle_length}, {value});"
                    ))
                    # Store length as a C static variable for update actions
                    lv_add(RawStatement(
                        f"static int32_t {needle_var_name}_len = {needle_length};"
                    ))
                    # Store needle info for update actions
                    _needle_registry[str(needle_id)] = {
                        "is_image": False,
                        "var_name": needle_var_name,
                        "length": needle_length,
                    }

        # Custom draw event callback for label customization (example 7)
        if custom_cb := config.get(CONF_CUSTOM_LABEL_CB):
            async with LambdaContext(
                [(lv_event_t.operator("ptr"), "e")]
            ) as lambda_:
                lv_add(RawStatement(str(custom_cb)))
            lv_obj.add_event_cb(
                w.obj,
                await lambda_.get_lambda(),
                LV_EVENT.DRAW_TASK_ADDED,
                nullptr,
            )
            lv.obj_add_flag(w.obj, LV_OBJ_FLAG.SEND_DRAW_TASK_EVENTS)

        # Set initial value if provided
        value = await get_start_value(config)
        if value is not None:
            pass


scale_spec = ScaleType()


@automation.register_action(
    "lvgl.scale.update",
    ObjUpdateAction,
    SCALE_MODIFY_SCHEMA.extend(
        {
            cv.Required(CONF_ID): cv.use_id(LvNumber("lv_scale_t")),
        }
    ),
)
async def scale_update_to_code(config, action_id, template_arg, args):
    """Handle scale update actions."""
    widgets = await get_widgets(config)

    async def update_scale(w: Widget):
        if CONF_MODE in config:
            mode = config[CONF_MODE]
            lv.scale_set_mode(w.obj, literal(mode))

        if CONF_MIN_VALUE in config or CONF_MAX_VALUE in config:
            min_val = config.get(CONF_MIN_VALUE, w.type.get_min(w.config))
            max_val = config.get(CONF_MAX_VALUE, w.type.get_max(w.config))
            lv.scale_set_range(w.obj, min_val, max_val)

        if CONF_ROTATION in config:
            rotation = await lv_angle_degrees.process(config[CONF_ROTATION])
            lv.scale_set_rotation(w.obj, rotation)

        if CONF_ANGLE_RANGE in config:
            angle_range = await lv_angle_degrees.process(config[CONF_ANGLE_RANGE])
            lv.scale_set_angle_range(w.obj, angle_range)

    return await action_to_code(widgets, update_scale, action_id, template_arg, args, config)


@automation.register_action(
    "lvgl.scale.section.update",
    ObjUpdateAction,
    cv.Schema(
        {
            cv.Required(CONF_ID): cv.use_id(LvNumber("lv_scale_section_t")),
            cv.Optional(CONF_START_VALUE): lv_float,
            cv.Optional(CONF_END_VALUE): lv_float,
            cv.Optional(CONF_RANGE_FROM): lv_int,
            cv.Optional(CONF_RANGE_TO): lv_int,
            cv.Optional(CONF_COLOR): lv_color,
            cv.Optional(CONF_WIDTH): cv.positive_int,
        }
    ).add_extra(cv.has_at_most_one_key(CONF_START_VALUE, CONF_RANGE_FROM)),
)
async def section_update_to_code(config, action_id, template_arg, args):
    """Handle scale section update actions."""
    widgets = await get_widgets(config)

    # Track style counter for unique names
    style_counter = [0]

    async def update_section(w: Widget):
        # Update section range
        start_value = await get_start_value(config)
        end_value = await get_end_value(config)

        if start_value is not None and end_value is not None:
            lv.scale_section_set_range(w.obj, start_value, end_value)
        elif start_value is not None:
            lv.scale_section_set_range(w.obj, start_value, start_value)

        # Update section style using lv_style_t
        if CONF_COLOR in config or CONF_WIDTH in config:
            style_name = f"scale_section_update_style_{style_counter[0]}"
            style_counter[0] += 1

            lv_add(RawStatement(f"static lv_style_t {style_name};"))
            lv_add(RawStatement(f"lv_style_init(&{style_name});"))

            if CONF_COLOR in config:
                color = await lv_color.process(config[CONF_COLOR])
                lv_add(RawStatement(f"lv_style_set_line_color(&{style_name}, {color});"))

            if CONF_WIDTH in config:
                width = config[CONF_WIDTH]
                lv_add(RawStatement(f"lv_style_set_line_width(&{style_name}, {width});"))

            lv_add(RawStatement(
                f"lv_scale_section_set_style({w.obj}, LV_PART_INDICATOR, &{style_name});"
            ))

    return await action_to_code(widgets, update_section, action_id, template_arg, args, config)


# Needle update action schema
CONF_SCALE_ID = "scale_id"
CONF_NEEDLE_ID = "needle_id"

NEEDLE_UPDATE_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_SCALE_ID): cv.use_id(LvNumber("lv_scale_t")),
        cv.Required(CONF_NEEDLE_ID): cv.string,
        cv.Required(CONF_VALUE): lv_int,
        cv.Optional(CONF_NEEDLE_LENGTH): cv.positive_int,
    }
)


@automation.register_action(
    "lvgl.scale.needle.update",
    ObjUpdateAction,
    NEEDLE_UPDATE_SCHEMA,
)
async def needle_update_to_code(config, action_id, template_arg, args):
    """Handle scale needle update actions.

    Updates the value of a line or image needle on a scale widget.
    The scale_id identifies the parent scale, and needle_id identifies which needle to update.
    """
    # Get the scale widget using scale_id
    scale_widgets = await get_widgets(config, CONF_SCALE_ID)
    needle_id_str = config[CONF_NEEDLE_ID]

    async def update_needle(w: Widget):
        value = config.get(CONF_VALUE, 0)
        needle_info = _needle_registry.get(needle_id_str)

        if needle_info is None:
            return

        var_name = needle_info["var_name"]
        if needle_info["is_image"]:
            lv_add(RawStatement(
                f"lv_scale_set_image_needle_value({w.obj}, {var_name}, {value});"
            ))
        else:
            # For line needles, use stored length or override
            length = config.get(CONF_NEEDLE_LENGTH, needle_info.get("length", 60))
            lv_add(RawStatement(
                f"lv_scale_set_line_needle_value({w.obj}, {var_name}, {length}, {value});"
            ))

    return await action_to_code(
        scale_widgets, update_needle, action_id, template_arg, args, config
    )
