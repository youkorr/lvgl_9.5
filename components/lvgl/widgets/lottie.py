"""
LVGL 9.4 Lottie Animation Widget for ESPHome

Lottie renders Adobe After Effects animations exported as JSON via Bodymovin.

Requirements:
- LV_USE_LOTTIE must be enabled
- LV_USE_THORVG_INTERNAL must be enabled
- LV_USE_VECTOR_GRAPHIC must be enabled
- LV_USE_CANVAS must be enabled

Usage in ESPHome YAML:
    - lottie:
        id: my_animation
        file: "loading.json"
"""

import json
from pathlib import Path

from esphome import codegen as cg, config_validation as cv
from esphome.const import CONF_FILE, CONF_ID
from esphome.core import CORE, ID

from ..defines import CONF_AUTO_START, CONF_MAIN
from ..helpers import add_lv_use

CONF_LOOP = "loop"
from ..lvcode import lv
from ..types import LvType
from . import Widget, WidgetType

CONF_LOTTIE = "lottie"
CONF_LOTTIE_WIDTH = "lottie_width"
CONF_LOTTIE_HEIGHT = "lottie_height"

lv_lottie_t = LvType("lv_lottie_t")


def lottie_file_validator(value):
    """Validate and resolve local Lottie file path."""
    value = cv.string(value)
    path = CORE.relative_config_path(value)
    if not Path(path).is_file():
        raise cv.Invalid(f"Lottie file not found: {path}")
    return str(path)


def validate_lottie_config(config):
    """Extract dimensions from JSON file."""
    if CONF_FILE not in config:
        raise cv.Invalid("'file' is required for lottie widget")

    file_path = config[CONF_FILE]
    try:
        with open(file_path, "r", encoding="utf-8") as f:
            lottie_data = json.load(f)
            lottie_width = lottie_data.get("w")
            lottie_height = lottie_data.get("h")
            if lottie_width is None or lottie_height is None:
                raise cv.Invalid(f"Lottie JSON missing 'w' or 'h': {file_path}")
            config[CONF_LOTTIE_WIDTH] = int(lottie_width)
            config[CONF_LOTTIE_HEIGHT] = int(lottie_height)
    except json.JSONDecodeError as e:
        raise cv.Invalid(f"Invalid JSON in Lottie file {file_path}: {e}")
    except FileNotFoundError:
        raise cv.Invalid(f"Lottie file not found: {file_path}")
    return config


LOTTIE_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_FILE): lottie_file_validator,
        cv.Optional(CONF_LOOP, default=True): cv.boolean,
        cv.Optional(CONF_AUTO_START, default=True): cv.boolean,
    }
).add_extra(validate_lottie_config)

LOTTIE_MODIFY_SCHEMA = cv.Schema({})


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
        return ("LOTTIE", "THORVG_INTERNAL", "VECTOR_GRAPHIC", "CANVAS")

    async def to_code(self, w: Widget, config):
        add_lv_use("LOTTIE")
        add_lv_use("THORVG_INTERNAL")
        add_lv_use("VECTOR_GRAPHIC")
        add_lv_use("CANVAS")

        width = config[CONF_LOTTIE_WIDTH]
        height = config[CONF_LOTTIE_HEIGHT]
        file_path = config[CONF_FILE]

        # Read JSON file content as bytes
        with open(file_path, "rb") as f:
            json_bytes = f.read()

        # Get unique identifier
        wid = str(config[CONF_ID]).replace("-", "_").replace(".", "_")

        # Create progmem array for JSON data
        json_array_id = ID(f"lottie_data_{wid}", is_declaration=True, type=cg.uint8)
        json_array = cg.progmem_array(json_array_id, list(json_bytes))

        # Allocate render buffer using lv_malloc
        buf_size = width * height * 4
        lottie_buffer = lv.malloc(buf_size)

        # Configure the lottie widget
        lv.lottie_set_buffer(w.obj, width, height, lottie_buffer)

        # Set widget size
        from ..lvcode import lv_obj
        lv_obj.set_size(w.obj, width, height)

        # Load animation data (cast to const char*)
        lv.lottie_set_src_data(
            w.obj,
            cg.RawExpression(f"(const char*){json_array}"),
            len(json_bytes)
        )


lottie_spec = LottieType()
