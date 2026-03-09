"""
LVGL 9.5 Animated SVG Widget for ESPHome

Renders animated SVGs (with SMIL animations) using ThorVG.
Supports the animation types found in weather icon sets like
basmilius/weather-icons:

  - animateTransform type="rotate"
  - animateTransform type="translate"
  - animateTransform type="scale"
  - animate attributeName="opacity"

The SVG is pre-processed at compile time:
  1. SMIL animation elements are parsed and extracted
  2. Animated attributes are replaced with __PHn__ placeholders
  3. At runtime, placeholders are filled with interpolated values
     and the SVG is re-rendered each frame via ThorVG

Usage in ESPHome YAML:

    Method 1 - Embedded in firmware:
    - animated_svg:
        id: weather_icon
        file: "icons/clear-day.svg"
        width: 128
        height: 128
        fps: 10

    Method 2 - File on filesystem (SD card, LittleFS):
    - animated_svg:
        id: weather_icon
        src: "/sdcard/icons/clear-day.svg"
        width: 128
        height: 128
        fps: 10

Requirements:
- LV_USE_THORVG_INTERNAL must be enabled
- LV_USE_VECTOR_GRAPHIC must be enabled
- LV_USE_SVG must be enabled
"""

import re
import xml.etree.ElementTree as ET
from pathlib import Path

from esphome import codegen as cg, config_validation as cv
from esphome.const import CONF_FILE, CONF_HEIGHT, CONF_ID, CONF_RAW_DATA_ID, CONF_WIDTH
from esphome.core import CORE

from ..defines import CONF_MAIN, CONF_SRC, literal
from ..helpers import add_lv_use
from ..lv_validation import size
from ..lvcode import lv_obj
from ..types import LvType, lv_obj_t
from . import Widget, WidgetType

# Global flag – add the #include once
_asvg_include_added = False

CONF_ANIMATED_SVG = "animated_svg"
CONF_FPS = "fps"

lv_canvas_t = LvType("lv_canvas_t")


# ---------------------------------------------------------------------------
# SMIL Animation Parser
# ---------------------------------------------------------------------------

# Namespace map for SVG XML parsing
SVG_NS = {
    'svg': 'http://www.w3.org/2000/svg',
    'xlink': 'http://www.w3.org/1999/xlink',
}


def _parse_duration(dur_str):
    """Parse SVG duration string to seconds. E.g. '6s' -> 6.0, '.67s' -> 0.67, '500ms' -> 0.5"""
    if not dur_str:
        return 1.0
    dur_str = dur_str.strip()
    if dur_str.endswith('ms'):
        return float(dur_str[:-2]) / 1000.0
    if dur_str.endswith('s'):
        return float(dur_str[:-1])
    try:
        return float(dur_str)
    except ValueError:
        return 1.0


def _parse_begin_delay(begin_str):
    """Parse begin attribute to extract initial delay in seconds.

    Handles:
      '0s' -> 0.0
      '.33s' -> 0.33
      '-.33s' -> -0.33
      '0s; x1.end+.33s' -> (0.0, 0.33)  (delay, repeat_gap)
      None -> 0.0
    """
    if not begin_str:
        return 0.0, 0.0

    parts = [p.strip() for p in begin_str.split(';')]

    # Initial delay from first part
    delay = 0.0
    repeat_gap = 0.0

    if parts:
        first = parts[0]
        if first and not any(c in first for c in ['.end', '.begin']):
            delay = _parse_duration(first)

    # Repeat gap from "id.end+Xs" pattern
    for part in parts:
        m = re.search(r'\.end\s*\+\s*([\d.]+)s?', part)
        if m:
            repeat_gap = float(m.group(1))
            break
        m = re.search(r'\.end\s*-\s*([\d.]+)s?', part)
        if m:
            repeat_gap = -float(m.group(1))
            break

    return delay, repeat_gap


def _parse_values(values_str, anim_type):
    """Parse SMIL values attribute into list of (components, count) tuples.

    rotate values: '0 187.5 187.5; 45 187.5 187.5' -> [(0, 187.5, 187.5), (45, 187.5, 187.5)]
    translate values: '0 -60; 0 60' -> [(0, -60, 0), (0, 60, 0)]
    opacity values: '0; 1; 0' -> [(0, 0, 0), (1, 0, 0), (0, 0, 0)]
    """
    if not values_str:
        return []

    result = []
    for val_part in values_str.split(';'):
        val_part = val_part.strip()
        if not val_part:
            continue
        nums = [float(x) for x in val_part.split()]
        while len(nums) < 3:
            nums.append(0.0)
        count = min(len([float(x) for x in val_part.split()]), 3)
        result.append((nums[0], nums[1], nums[2], count))

    return result


def _parse_key_times(key_times_str):
    """Parse keyTimes attribute: '0; .25; 1' -> [0.0, 0.25, 1.0]"""
    if not key_times_str:
        return None
    return [float(x.strip()) for x in key_times_str.split(';') if x.strip()]


class SmilAnimation:
    """Represents one parsed SMIL animation."""

    def __init__(self):
        self.anim_type = None       # 'rotate', 'translate', 'opacity', 'scale'
        self.values = []            # list of (v0, v1, v2, count) tuples
        self.key_times = None       # list of floats or None
        self.duration_s = 1.0       # seconds
        self.begin_delay_s = 0.0    # seconds
        self.repeat_gap_s = 0.0     # seconds between repeats
        self.additive = False       # additive="sum"
        self.placeholder_id = 0     # assigned during processing
        self.parent_element = None  # the XML element this animates


def _extract_animations(svg_text):
    """Parse SVG XML and extract all SMIL animations.

    Returns:
        (modified_svg_text, list_of_SmilAnimation)

    The modified SVG has animation elements removed and placeholders inserted.
    """
    # Register namespaces to preserve them in output
    ET.register_namespace('', 'http://www.w3.org/2000/svg')
    ET.register_namespace('xlink', 'http://www.w3.org/1999/xlink')

    root = ET.fromstring(svg_text)

    animations = []
    placeholder_counter = [0]  # mutable counter

    def process_element(elem):
        """Find and process animation children of this element."""
        anim_children = []
        transform_placeholder = None
        opacity_placeholder = None

        for child in list(elem):
            tag = child.tag
            # Strip namespace
            if '}' in tag:
                tag = tag.split('}', 1)[1]

            if tag == 'animateTransform':
                anim = SmilAnimation()
                anim.parent_element = elem

                anim_type = child.get('type', 'translate')
                anim.anim_type = anim_type

                values_str = child.get('values', '')
                anim.values = _parse_values(values_str, anim_type)

                anim.duration_s = _parse_duration(child.get('dur', '1s'))

                delay, gap = _parse_begin_delay(child.get('begin'))
                anim.begin_delay_s = delay
                anim.repeat_gap_s = gap

                anim.additive = (child.get('additive', '') == 'sum')

                kt_str = child.get('keyTimes')
                anim.key_times = _parse_key_times(kt_str)

                pid = placeholder_counter[0]
                placeholder_counter[0] += 1
                anim.placeholder_id = pid

                animations.append(anim)
                anim_children.append(child)
                transform_placeholder = pid

            elif tag == 'animate':
                attr_name = child.get('attributeName', '')
                if attr_name == 'opacity':
                    anim = SmilAnimation()
                    anim.parent_element = elem
                    anim.anim_type = 'opacity'

                    values_str = child.get('values', '')
                    anim.values = _parse_values(values_str, 'opacity')

                    anim.duration_s = _parse_duration(child.get('dur', '1s'))

                    delay, gap = _parse_begin_delay(child.get('begin'))
                    anim.begin_delay_s = delay
                    anim.repeat_gap_s = gap

                    anim.additive = False

                    kt_str = child.get('keyTimes')
                    anim.key_times = _parse_key_times(kt_str)

                    pid = placeholder_counter[0]
                    placeholder_counter[0] += 1
                    anim.placeholder_id = pid

                    animations.append(anim)
                    anim_children.append(child)
                    opacity_placeholder = pid

            else:
                # Recurse into non-animation children
                process_element(child)

        # Remove animation elements from parent
        for ac in anim_children:
            elem.remove(ac)

        # Insert placeholders into parent attributes
        if transform_placeholder is not None:
            existing_transform = elem.get('transform', '')
            # Append placeholder to existing transform
            elem.set('transform', f"{existing_transform}__PH{transform_placeholder}__")

        if opacity_placeholder is not None:
            # Replace or set opacity attribute with placeholder
            elem.set('opacity', f"__PH{opacity_placeholder}__")

    # Process all elements recursively
    process_element(root)

    # Convert back to string
    modified_svg = ET.tostring(root, encoding='unicode', xml_declaration=False)

    # ET may add ns0/ns1 prefixes – clean them up
    modified_svg = modified_svg.replace('ns0:', '').replace(':ns0', '')
    modified_svg = modified_svg.replace('ns1:', 'xlink:').replace(':ns1', ':xlink')
    # Ensure SVG namespace is present
    if 'xmlns=' not in modified_svg and 'xmlns:' not in modified_svg:
        modified_svg = modified_svg.replace('<svg ', '<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" ', 1)

    return modified_svg, animations


def _anim_type_to_c(anim_type):
    """Convert animation type string to C enum value."""
    mapping = {
        'rotate': 'esphome::lvgl::SMIL_ROTATE',
        'translate': 'esphome::lvgl::SMIL_TRANSLATE',
        'opacity': 'esphome::lvgl::SMIL_OPACITY',
        'scale': 'esphome::lvgl::SMIL_SCALE',
    }
    return mapping.get(anim_type, 'esphome::lvgl::SMIL_TRANSLATE')


def _generate_anim_c_array(animations, array_name):
    """Generate C code for the SmilAnim array."""
    if not animations:
        return f"static const esphome::lvgl::SmilAnim {array_name}[] = {{}};"

    lines = []
    lines.append(f"static const esphome::lvgl::SmilAnim {array_name}[] = {{")

    for anim in animations:
        # Build values array
        values_parts = []
        for val in anim.values[:8]:  # max 8 values
            values_parts.append(
                f"{{{{{val[0]:.4f}f, {val[1]:.4f}f, {val[2]:.4f}f}}, {val[3]}}}"
            )
        # Pad to 8 entries
        while len(values_parts) < 8:
            values_parts.append("{{0, 0, 0}, 0}")
        values_str = ", ".join(values_parts)

        # Build keyTimes array
        if anim.key_times:
            kt_vals = [f"{kt:.4f}f" for kt in anim.key_times[:8]]
            while len(kt_vals) < 8:
                kt_vals.append("0")
            kt_str = ", ".join(kt_vals)
            has_kt = "true"
        else:
            kt_str = "0, 0, 0, 0, 0, 0, 0, 0"
            has_kt = "false"

        lines.append(f"  {{ // placeholder {anim.placeholder_id}: {anim.anim_type}")
        lines.append(f"    {_anim_type_to_c(anim.anim_type)},")
        lines.append(f"    {len(anim.values)},  // num_values")
        lines.append(f"    {{{values_str}}},  // values")
        lines.append(f"    {{{kt_str}}},  // key_times")
        lines.append(f"    {has_kt},  // has_key_times")
        lines.append(f"    {anim.duration_s:.4f}f,  // duration_s")
        lines.append(f"    {anim.begin_delay_s:.4f}f,  // begin_delay_s")
        lines.append(f"    {anim.repeat_gap_s:.4f}f,  // repeat_gap_s")
        lines.append(f"    {'true' if anim.additive else 'false'},  // additive")
        lines.append(f"    {anim.placeholder_id},  // placeholder_id")
        lines.append(f"  }},")

    lines.append("};")
    return "\n".join(lines)


# ---------------------------------------------------------------------------
# SVG file validators
# ---------------------------------------------------------------------------

def asvg_path_validator(value):
    """Validate animated SVG source file path (on ESP32 filesystem)."""
    value = cv.string(value)
    if not value.startswith("/"):
        raise cv.Invalid(
            f"Animated SVG src must be an absolute path starting with '/', got: '{value}'. "
            f"Example: '/sdcard/icons/clear-day.svg'"
        )
    if not value.lower().endswith(".svg"):
        raise cv.Invalid(f"Animated SVG src must be an .svg file, got: '{value}'")
    return value


def asvg_file_validator(value):
    """Validate and resolve local animated SVG file path."""
    value = cv.string(value)
    path = CORE.relative_config_path(value)
    if not Path(path).is_file():
        raise cv.Invalid(f"Animated SVG file not found: {path}")
    return str(path)


def _parse_svg_dimensions(svg_text):
    """Extract width/height from SVG viewBox or width/height attributes."""
    m = re.search(r'viewBox\s*=\s*"([^"]*)"', svg_text)
    if not m:
        m = re.search(r"viewBox\s*=\s*'([^']*)'", svg_text)
    if m:
        parts = m.group(1).split()
        if len(parts) == 4:
            try:
                return int(float(parts[2])), int(float(parts[3]))
            except ValueError:
                pass

    w_match = re.search(r'\bwidth\s*=\s*["\']?([\d.]+)', svg_text)
    h_match = re.search(r'\bheight\s*=\s*["\']?([\d.]+)', svg_text)
    if w_match and h_match:
        try:
            return int(float(w_match.group(1))), int(float(h_match.group(1)))
        except ValueError:
            pass

    return None, None


CONF_ASVG_WIDTH = "asvg_width"
CONF_ASVG_HEIGHT = "asvg_height"


def validate_asvg_source(config):
    """Validate source and extract dimensions."""
    has_src = CONF_SRC in config
    has_file = CONF_FILE in config

    if has_src and has_file:
        raise cv.Invalid("Cannot specify both 'src' and 'file'.")
    if not has_src and not has_file:
        raise cv.Invalid("Must specify either 'src' or 'file'.")

    if has_src:
        if CONF_WIDTH not in config or CONF_HEIGHT not in config:
            raise cv.Invalid(
                "'width' and 'height' are required when using 'src'."
            )

    if has_file:
        file_path = config[CONF_FILE]
        try:
            with open(file_path, "r", encoding="utf-8") as f:
                svg_text = f.read()
            svg_w, svg_h = _parse_svg_dimensions(svg_text)
        except Exception as e:
            raise cv.Invalid(f"Error reading SVG file {file_path}: {e}")

        if CONF_WIDTH not in config and CONF_HEIGHT not in config:
            if svg_w is None or svg_h is None:
                raise cv.Invalid(
                    f"Cannot auto-detect dimensions from {file_path}. "
                    f"Please specify 'width' and 'height'."
                )
            config[CONF_ASVG_WIDTH] = svg_w
            config[CONF_ASVG_HEIGHT] = svg_h
        elif CONF_WIDTH in config and CONF_HEIGHT in config:
            pass
        else:
            raise cv.Invalid("Specify both 'width' and 'height', or neither.")

    return config


ANIMATED_SVG_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_WIDTH): size,
        cv.Optional(CONF_HEIGHT): size,
        cv.Optional(CONF_SRC): asvg_path_validator,
        cv.Optional(CONF_FILE): asvg_file_validator,
        cv.Optional(CONF_FPS, default=10): cv.int_range(min=1, max=30),
        cv.GenerateID(CONF_RAW_DATA_ID): cv.declare_id(cg.uint8),
    }
).add_extra(validate_asvg_source)


class AnimatedSvgType(WidgetType):
    def __init__(self):
        super().__init__(
            CONF_ANIMATED_SVG,
            lv_canvas_t,
            (CONF_MAIN,),
            ANIMATED_SVG_SCHEMA,
            modify_schema={},
            lv_name="canvas",
        )

    def get_uses(self):
        return ("CANVAS", "SVG", "THORVG_INTERNAL", "VECTOR_GRAPHIC")

    async def to_code(self, w: Widget, config):
        global _asvg_include_added

        add_lv_use("CANVAS")
        add_lv_use("SVG")
        add_lv_use("THORVG_INTERNAL")
        add_lv_use("VECTOR_GRAPHIC")

        from ..lvcode import lv_add

        # Determine dimensions
        if CONF_ASVG_WIDTH in config:
            width = config[CONF_ASVG_WIDTH]
            height = config[CONF_ASVG_HEIGHT]
        else:
            width = config[CONF_WIDTH]
            height = config[CONF_HEIGHT]

        # Calculate frame delay from FPS
        fps = config.get(CONF_FPS, 10)
        frame_delay_ms = max(33, 1000 // fps)  # min ~30 FPS

        lv_obj.set_size(w.obj, width, height)

        # Add include once
        if not _asvg_include_added:
            _asvg_include_added = True
            cg.add_global(
                cg.RawStatement('#include "esphome/components/lvgl/animated_svg_loader.h"')
            )

        user_wants_hidden = "true" if config.get("hidden", False) else "false"

        if file_path := config.get(CONF_FILE):
            # ------- Embedded animated SVG -------
            with open(file_path, "r", encoding="utf-8") as f:
                svg_text = f.read()

            # Parse SMIL animations and create template
            modified_svg, animations = _extract_animations(svg_text)

            if not animations:
                # No animations found – fall back to static SVG rendering
                import logging
                logging.getLogger(__name__).warning(
                    f"No SMIL animations found in {file_path}, "
                    f"rendering as static SVG"
                )

            # Embed the modified SVG template
            svg_bytes = modified_svg.encode('utf-8') + b'\x00'
            raw_data_id = config[CONF_RAW_DATA_ID]
            prog_arr = cg.progmem_array(raw_data_id, list(svg_bytes))

            # Generate unique animation array name
            anim_array_name = f"asvg_anims_{w.obj}".replace("->", "_").replace(".", "_")
            anim_c_code = _generate_anim_c_array(animations, anim_array_name)

            # Emit the animation descriptor array
            lv_add(cg.RawStatement(f"\n{anim_c_code}"))

            # Call asvg_init
            lv_add(cg.RawStatement(f"""
    esphome::lvgl::asvg_init({w.obj}, (const char *){prog_arr}, {len(modified_svg.encode('utf-8'))}, {anim_array_name}, {len(animations)}, {width}, {height}, {frame_delay_ms}, {user_wants_hidden});"""))

        elif src := config.get(CONF_SRC):
            # ------- Filesystem animated SVG (SD card, LittleFS) -------
            # Uses runtime SMIL parser to extract animations from SVG at boot.
            # Supports the same animation types as embedded mode:
            #   animateTransform (rotate, translate, scale), animate (opacity)
            lv_add(cg.RawStatement(f"""
    esphome::lvgl::asvg_init_file_rt({w.obj}, "{src}", {width}, {height}, {frame_delay_ms}, {user_wants_hidden});"""))


animated_svg_spec = AnimatedSvgType()
