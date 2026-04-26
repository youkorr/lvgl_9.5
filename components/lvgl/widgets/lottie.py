"""
LVGL 9.4 Lottie Animation Widget for ESPHome

This module implements the Lottie animation widget for LVGL 9.4.

Lottie is a library for parsing Adobe After Effects animations exported as JSON
using the Bodymovin plugin and rendering them natively.

Requirements:
- LV_USE_LOTTIE must be enabled
- LV_USE_THORVG_INTERNAL must be enabled
- LV_USE_VECTOR_GRAPHIC must be enabled

Usage in ESPHome YAML:

    Method 1 - File on filesystem (SD card, LittleFS):
    - lottie:
        id: my_animation
        src: "/sdcard/animation.json"   # File path on ESP32 filesystem
        width: 200                      # Required for src (can't read at compile time)
        height: 200
        loop: true
        auto_start: true

    Method 2 - Embedded in firmware (auto-detects size from JSON):
    - lottie:
        id: my_animation
        file: "animations/loading.json"  # Local file, embedded in firmware
        loop: true                       # width/height auto-detected from JSON
        auto_start: true

    Method 3 - Embedded with resize (render at custom size for screen layout):
    - lottie:
        id: my_animation
        file: "animations/loading.json"  # Source is 300x300 in JSON
        width: 150                       # Render at 150x150 instead
        height: 150
        loop: true
        auto_start: true

Actions:
    - lvgl.lottie.start: my_animation
    - lvgl.lottie.stop: my_animation
    - lvgl.lottie.pause: my_animation

Note: ThorVG parsing requires a large stack (32KB+). On ESP32, the loading is
deferred to a FreeRTOS task with stack allocated in PSRAM to avoid stack overflow.
"""

import json
from pathlib import Path

from esphome import automation, codegen as cg, config_validation as cv
from esphome.const import CONF_FILE, CONF_HEIGHT, CONF_ID, CONF_RAW_DATA_ID, CONF_WIDTH
from esphome.core import CORE

from ..automation import action_to_code
from ..defines import CONF_AUTO_START, CONF_MAIN, CONF_SRC, literal
from ..helpers import add_lv_use
from ..lv_validation import size
from ..lvcode import lv
from ..types import LvType, ObjUpdateAction
from . import Widget, WidgetType, get_widgets

# Global flag to track if include has been added
_lottie_include_added = False

CONF_LOTTIE = "lottie"
CONF_LOOP = "loop"
CONF_LOTTIE_WIDTH = "lottie_width"
CONF_LOTTIE_HEIGHT = "lottie_height"
CONF_MARKERS = "markers"
CONF_MARKER = "marker"
CONF_ONE_SHOT = "one_shot"
CONF_ON_MARKER_COMPLETE = "on_marker_complete"
CONF_INITIAL_MARKER = "initial_marker"

lv_lottie_t = LvType("lv_lottie_t")


def lottie_path_validator(value):
    """Validate Lottie source file path (on ESP32 filesystem)."""
    value = cv.string(value)
    if not value.startswith("/"):
        raise cv.Invalid(
            f"Lottie src must be an absolute file path starting with '/', got: '{value}'. "
            f"Example: '/sdcard/animation.json' or '/littlefs/animation.json'"
        )
    if not value.endswith(".json"):
        raise cv.Invalid(
            f"Lottie src must be a JSON file (ending with .json), got: '{value}'"
        )
    return value


def lottie_file_validator(value):
    """Validate and resolve local Lottie file path (to embed in firmware)."""
    value = cv.string(value)
    # Resolve relative to config directory
    path = CORE.relative_config_path(value)
    if not Path(path).is_file():
        raise cv.Invalid(f"Lottie file not found: {path}")
    return str(path)


def _parse_markers_from_json(lottie_data):
    """Return list of (name, start_frame, end_frame) tuples from a Lottie JSON.

    Mirrors the LottieFiles "Interactivity" marker convention so that
    play_marker / on_marker_complete can address each segment by name.
    """
    out = []
    for m in lottie_data.get("markers", []) or []:
        name = m.get("cm")
        if not name:
            continue
        start = int(m.get("tm", 0))
        dur = int(m.get("dr", 0))
        out.append((str(name), start, start + dur))
    return out


def _find_local_lottie(src_path):
    """Locate a JSON sibling of the YAML for marker auto-extraction.

    The user's `src:` typically points at a runtime mount (`/sdcard/...`,
    `/littlefs/...`) that is not visible from the host running esphome.
    Try several common locations relative to the YAML config dir before
    giving up:
      * the raw path with the leading '/' stripped
      * the path with a known mount prefix removed
      * just the basename (when the user keeps the JSON next to the YAML)
    Returns the first existing Path or None.
    """
    candidates = []
    raw = src_path.lstrip("/")
    candidates.append(raw)
    for prefix in ("sdcard/", "littlefs/", "storage/", "fs/", "spiffs/"):
        if raw.startswith(prefix):
            candidates.append(raw[len(prefix):])
    candidates.append(Path(src_path).name)

    seen = set()
    for c in candidates:
        if c in seen:
            continue
        seen.add(c)
        p = Path(CORE.relative_config_path(c))
        if p.is_file():
            return p
    return None


def validate_lottie_source(config):
    """Validate source and extract dimensions / markers from JSON."""
    has_src = CONF_SRC in config
    has_file = CONF_FILE in config

    if has_src and has_file:
        raise cv.Invalid("Cannot specify both 'src' and 'file'. Use 'src' for filesystem path or 'file' for embedded.")
    if not has_src and not has_file:
        raise cv.Invalid("Must specify either 'src' (filesystem path) or 'file' (embedded in firmware).")

    # For src method, width and height are required
    if has_src:
        if CONF_WIDTH not in config or CONF_HEIGHT not in config:
            raise cv.Invalid("'width' and 'height' are required when using 'src' (filesystem path). Cannot auto-detect dimensions at compile time.")

        # Markers: if a local copy of the JSON is reachable from the YAML
        # config dir (sibling of the YAML, sdcard/littlefs mirror, etc.)
        # we read its `markers` array so play_marker can address segments
        # by name without forcing the user to retype them in YAML.
        local = _find_local_lottie(config[CONF_SRC])
        if local is not None:
            try:
                with open(local, "r", encoding="utf-8") as f:
                    config["_markers_auto"] = _parse_markers_from_json(json.load(f))
            except (json.JSONDecodeError, OSError):
                pass  # silently ignore – user can still declare markers manually

    # For file method, auto-detect dimensions and markers from JSON
    if has_file:
        file_path = config[CONF_FILE]
        try:
            with open(file_path, "r", encoding="utf-8") as f:
                lottie_data = json.load(f)
                # Extract dimensions from Lottie JSON
                lottie_width = lottie_data.get("w")
                lottie_height = lottie_data.get("h")
                if lottie_width is None or lottie_height is None:
                    raise cv.Invalid(f"Lottie JSON file missing 'w' or 'h' dimensions: {file_path}")
                # Only use auto-detected dimensions if user didn't specify custom size
                if CONF_WIDTH not in config or CONF_HEIGHT not in config:
                    config[CONF_LOTTIE_WIDTH] = int(lottie_width)
                    config[CONF_LOTTIE_HEIGHT] = int(lottie_height)
                config["_markers_auto"] = _parse_markers_from_json(lottie_data)
        except json.JSONDecodeError as e:
            raise cv.Invalid(f"Invalid JSON in Lottie file {file_path}: {e}")
        except Exception as e:
            raise cv.Invalid(f"Error reading Lottie file {file_path}: {e}")

    return config


MARKER_SCHEMA = cv.Schema(
    {
        cv.Required("name"): cv.string_strict,
        cv.Required("start_frame"): cv.positive_int,
        cv.Required("end_frame"): cv.positive_int,
    }
)

LOTTIE_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_WIDTH): size,
        cv.Optional(CONF_HEIGHT): size,
        cv.Optional(CONF_SRC): lottie_path_validator,
        cv.Optional(CONF_FILE): lottie_file_validator,
        cv.Optional(CONF_LOOP, default=True): cv.boolean,
        cv.Optional(CONF_AUTO_START, default=True): cv.boolean,
        cv.Optional(CONF_INITIAL_MARKER): cv.string_strict,
        cv.Optional(CONF_MARKERS): cv.ensure_list(MARKER_SCHEMA),
        cv.Optional(CONF_ON_MARKER_COMPLETE): automation.validate_automation(
            {cv.GenerateID(): cv.declare_id(automation.Trigger.template(cg.std_string))}
        ),
        cv.GenerateID(CONF_RAW_DATA_ID): cv.declare_id(cg.uint8),
    }
).add_extra(validate_lottie_source)

LOTTIE_MODIFY_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_SRC): lottie_path_validator,
        cv.Optional(CONF_LOOP): cv.boolean,
    }
)


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
        return ("LOTTIE", "THORVG_INTERNAL", "VECTOR_GRAPHIC")

    async def to_code(self, w: Widget, config):
        global _lottie_include_added

        add_lv_use("LOTTIE")
        add_lv_use("THORVG_INTERNAL")
        add_lv_use("VECTOR_GRAPHIC")

        from ..lvcode import lv_obj, lv_add

        # Get dimensions - user-specified override auto-detected from JSON
        if CONF_WIDTH in config and CONF_HEIGHT in config:
            width = config[CONF_WIDTH]
            height = config[CONF_HEIGHT]
        elif CONF_LOTTIE_WIDTH in config:
            width = config[CONF_LOTTIE_WIDTH]
            height = config[CONF_LOTTIE_HEIGHT]
        else:
            width = config[CONF_WIDTH]
            height = config[CONF_HEIGHT]

        # Set widget size
        lv_obj.set_size(w.obj, width, height)

        # Note: Widget visibility during async load is managed by lottie_loader.h
        # (user's 'hidden' config is saved and restored after rendering)

        # Add include for lottie loader helper (once)
        if not _lottie_include_added:
            _lottie_include_added = True
            cg.add_global(cg.RawStatement('#include "esphome/components/lvgl/lottie_loader.h"'))

        # Get loop, auto_start, and hidden config
        do_loop = "true" if config.get(CONF_LOOP, True) else "false"
        do_auto_start = "true" if config.get(CONF_AUTO_START, True) else "false"
        user_wants_hidden = "true" if config.get("hidden", False) else "false"

        # --- Markers (LottieFiles "Interactivity" segments) ---
        # User-declared markers in YAML take priority; otherwise fall back to
        # the markers auto-extracted from the JSON during validation.
        markers = config.get(CONF_MARKERS) or [
            {"name": n, "start_frame": s, "end_frame": e}
            for (n, s, e) in config.get("_markers_auto", [])
        ]
        markers_arg = "nullptr"
        marker_count = 0
        if markers:
            marker_count = len(markers)
            obj_id = str(w.obj).replace(".", "_").replace("->", "_")
            marker_array_name = f"_lottie_markers_{obj_id}"
            entries = ", ".join(
                f'{{"{m["name"]}", {int(m["start_frame"])}, {int(m["end_frame"])}}}'
                for m in markers
            )
            cg.add_global(cg.RawStatement(
                f"static const esphome::lvgl::LottieMarker {marker_array_name}[] = {{ {entries} }};"
            ))
            markers_arg = marker_array_name

        # Optional initial marker: at first launch, the render task narrows
        # start/end_frame to this segment so auto_start plays one state
        # instead of cycling through the whole timeline.
        initial_marker = config.get(CONF_INITIAL_MARKER)
        initial_arg = f'"{initial_marker}"' if initial_marker else "nullptr"

        # Use lottie_init() which handles PSRAM allocation, screen events, and task launch
        if src := config.get(CONF_SRC):
            lv_add(cg.RawStatement(f"""
    esphome::lvgl::lottie_init({w.obj}, nullptr, 0, "{src}", {width}, {height}, {do_loop}, {do_auto_start}, {user_wants_hidden}, {markers_arg}, {marker_count}, {initial_arg});"""))
        elif file_path := config.get(CONF_FILE):
            with open(file_path, "rb") as f:
                json_data = f.read()
            json_data_with_null = json_data + b'\x00'
            raw_data_id = config[CONF_RAW_DATA_ID]
            prog_arr = cg.progmem_array(raw_data_id, list(json_data_with_null))
            lv_add(cg.RawStatement(f"""
    esphome::lvgl::lottie_init({w.obj}, {prog_arr}, {len(json_data)}, nullptr, {width}, {height}, {do_loop}, {do_auto_start}, {user_wants_hidden}, {markers_arg}, {marker_count}, {initial_arg});"""))

        # --- on_marker_complete trigger ----------------------------------
        # When the user provides at least one automation under
        # `on_marker_complete:`, register a C trampoline that fires every
        # configured automation with the marker name as `x`.
        on_complete_list = config.get(CONF_ON_MARKER_COMPLETE) or []
        if on_complete_list:
            obj_id = str(w.obj).replace(".", "_").replace("->", "_")
            trampoline_name = f"_lottie_on_complete_{obj_id}"
            triggers = []
            for auto_conf in on_complete_list:
                trig = cg.new_Pvariable(auto_conf[CONF_ID])
                await automation.build_automation(trig, [(cg.std_string, "x")], auto_conf)
                triggers.append(str(trig))
            calls = " ".join(f"{t}->trigger(name);" for t in triggers)
            cg.add_global(cg.RawStatement(
                f"static void {trampoline_name}(void *arg, const char *marker) {{"
                f" std::string name = marker ? marker : \"\"; {calls} }}"
            ))
            lv_add(cg.RawStatement(f"""
    {{
      auto *_ctx = (esphome::lvgl::LottieContext *)lv_obj_get_user_data({w.obj});
      if (_ctx) esphome::lvgl::lottie_set_on_complete(_ctx, {trampoline_name}, nullptr);
    }}"""))


lottie_spec = LottieType()


@automation.register_action(
    "lvgl.lottie.start",
    ObjUpdateAction,
    cv.maybe_simple_value(
        {
            cv.Required(CONF_ID): cv.use_id(lv_lottie_t),
        },
        key=CONF_ID,
    ),
    synchronous=True,
)
async def lottie_start(config, action_id, template_arg, args):
    """Start or resume the Lottie animation."""
    widget = await get_widgets(config)

    async def do_start(w: Widget):
        lv.anim_start(lv.lottie_get_anim(w.obj))

    return await action_to_code(widget, do_start, action_id, template_arg, args)


@automation.register_action(
    "lvgl.lottie.stop",
    ObjUpdateAction,
    cv.maybe_simple_value(
        {
            cv.Required(CONF_ID): cv.use_id(lv_lottie_t),
        },
        key=CONF_ID,
    ),
    synchronous=True,
)
async def lottie_stop(config, action_id, template_arg, args):
    """Stop the Lottie animation and reset to beginning."""
    widget = await get_widgets(config)

    async def do_stop(w: Widget):
        lv.anim_delete(w.obj, literal("NULL"))

    return await action_to_code(widget, do_stop, action_id, template_arg, args)


@automation.register_action(
    "lvgl.lottie.pause",
    ObjUpdateAction,
    cv.maybe_simple_value(
        {
            cv.Required(CONF_ID): cv.use_id(lv_lottie_t),
        },
        key=CONF_ID,
    ),
    synchronous=True,
)
async def lottie_pause(config, action_id, template_arg, args):
    """Pause the Lottie animation at current frame."""
    widget = await get_widgets(config)

    async def do_pause(w: Widget):
        lv.anim_delete(w.obj, literal("NULL"))

    return await action_to_code(widget, do_pause, action_id, template_arg, args)


# --------------------------------------------------------------------------
# lvgl.lottie.play_marker — LottieFiles "Interactivity" *Click event.
# Switches the active animation segment to the named marker and restarts.
# --------------------------------------------------------------------------
@automation.register_action(
    "lvgl.lottie.play_marker",
    ObjUpdateAction,
    cv.Schema(
        {
            cv.Required(CONF_ID): cv.use_id(lv_lottie_t),
            cv.Required(CONF_MARKER): cv.templatable(cv.string_strict),
            cv.Optional(CONF_ONE_SHOT, default=False): cv.templatable(cv.boolean),
        }
    ),
    synchronous=True,
)
async def lottie_play_marker(config, action_id, template_arg, args):
    """Play a named marker — fires *Complete callback when one_shot=true."""
    widget = await get_widgets(config)

    # marker: accept either a Python literal or a !lambda expression.
    # Static literal is emitted as a C string literal (no .c_str()).
    # Lambdas are processed and their std::string return value gets .c_str().
    marker_val = config[CONF_MARKER]
    if isinstance(marker_val, str):
        marker_call = f'"{marker_val}"'
    else:
        marker_expr = await cg.templatable(marker_val, args, cg.std_string)
        marker_call = f"({marker_expr}).c_str()"

    # one_shot: same idea — Python booleans → C++ "true"/"false" literal,
    # lambdas → expression that evaluates to bool.
    one_shot_val = config[CONF_ONE_SHOT]
    if isinstance(one_shot_val, bool):
        one_shot_call = "true" if one_shot_val else "false"
    else:
        one_shot_expr = await cg.templatable(one_shot_val, args, bool)
        one_shot_call = f"({one_shot_expr})"

    async def do_play(w: Widget):
        # Resolve at runtime from the LV object's user_data slot, where
        # lottie_init() stored the LottieContext pointer.
        cg.add(cg.RawExpression(
            f"({{ auto *_ctx = (esphome::lvgl::LottieContext *)lv_obj_get_user_data({w.obj});"
            f" if (_ctx) esphome::lvgl::lottie_play_marker(_ctx, "
            f"{marker_call}, {one_shot_call}); }})"
        ))

    return await action_to_code(widget, do_play, action_id, template_arg, args)
