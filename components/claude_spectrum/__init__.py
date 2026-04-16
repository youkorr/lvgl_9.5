"""
claude_spectrum — circular radial FFT spectrum visualiser for LVGL 9.x.

Draws N radial bars into an existing lv_canvas widget at ~25 Hz, driven
by a 256-point FFT of an ESPHome microphone stream. Unlike Lottie it's
bounded by line-rasterisation (GPU-friendly on ESP32-P4 PPA) instead of
vector rendering, so you get a smooth visual even at 400×400+ where a
Lottie animation would saturate the CPU.

The microphone is tapped via `add_data_callback` — it is non-destructive,
so the voice assistant keeps receiving the same samples and the wake
word ("OK Nabu") continues to work. `buffer_skip` further reduces the
cost on the mic task by running the FFT on only 1 out of N buffers.

Typical YAML:

    external_components:
      - source: { type: local, path: components }
        components: [claude_spectrum]

    lvgl:
      pages:
        - id: main_page
          widgets:
            - canvas:
                id: orb_canvas
                width: 400
                height: 400
                transparent: true
                x: 312   # (1024 - 400) / 2
                y: 100

    claude_spectrum:
      id: orb_fft
      canvas_id: orb_canvas
      microphone: external_mic
      width: 400
      height: 400
      num_bars: 48
      radius_inner: 120
      radius_outer: 180
      bar_width: 4
      color_start: 0x4B00FF    # deep purple (low amplitude)
      color_end:   0x00FFE5    # cyan (high amplitude)
      bg_color:    0x000000
      bg_opa:      TRANSP
      update_interval: 40ms
      smoothing: 0.75
      gain: 1.8
      buffer_skip: 2
      mirror: false
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation
from esphome.components import microphone
from esphome.components.lvgl.widgets.canvas import lv_canvas_t
from esphome.const import (
    CONF_ID,
    CONF_MICROPHONE,
    CONF_UPDATE_INTERVAL,
    CONF_WIDTH,
    CONF_HEIGHT,
)

CODEOWNERS = ["@youkorr"]
DEPENDENCIES = ["lvgl", "microphone"]
MULTI_CONF = True

CONF_CANVAS_ID = "canvas_id"
CONF_NUM_BARS = "num_bars"
CONF_RADIUS_INNER = "radius_inner"
CONF_RADIUS_OUTER = "radius_outer"
CONF_BAR_WIDTH = "bar_width"
CONF_COLOR_START = "color_start"
CONF_COLOR_END = "color_end"
CONF_BG_COLOR = "bg_color"
CONF_BG_OPA = "bg_opa"
CONF_SMOOTHING = "smoothing"
CONF_GAIN = "gain"
CONF_AUTO_START = "auto_start"
CONF_BUFFER_SKIP = "buffer_skip"
CONF_ROTATE_SPEED = "rotate_speed"
CONF_MIRROR = "mirror"

claude_spectrum_ns = cg.esphome_ns.namespace("claude_spectrum")
ClaudeSpectrum = claude_spectrum_ns.class_("ClaudeSpectrum", cg.Component)
StartAction = claude_spectrum_ns.class_("StartAction", automation.Action)
StopAction = claude_spectrum_ns.class_("StopAction", automation.Action)


def _hex_color(value):
    """Accept 0xRRGGBB integer or hex string — returns int."""
    if isinstance(value, int):
        if value < 0 or value > 0xFFFFFF:
            raise cv.Invalid("Color must be in range 0x000000..0xFFFFFF")
        return value
    if isinstance(value, str):
        s = value.strip()
        if s.startswith("0x") or s.startswith("0X"):
            return int(s, 16)
        if s.startswith("#"):
            return int(s[1:], 16)
        try:
            return int(s, 0)
        except ValueError as exc:
            raise cv.Invalid(f"Invalid color: {value!r}") from exc
    raise cv.Invalid(f"Invalid color: {value!r}")


def _opacity(value):
    """Accept TRANSP / COVER / 0..255 — returns uint8."""
    if isinstance(value, str):
        v = value.strip().upper()
        if v in ("TRANSP", "TRANSPARENT"):
            return 0
        if v == "COVER":
            return 255
    try:
        n = int(value)
    except (TypeError, ValueError) as exc:
        raise cv.Invalid(f"Invalid opacity: {value!r}") from exc
    if n < 0 or n > 255:
        raise cv.Invalid("Opacity must be 0..255")
    return n


CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(ClaudeSpectrum),
        cv.Required(CONF_CANVAS_ID): cv.use_id(lv_canvas_t),
        cv.Required(CONF_MICROPHONE): cv.use_id(microphone.Microphone),

        cv.Optional(CONF_WIDTH, default=400): cv.int_range(min=32, max=1024),
        cv.Optional(CONF_HEIGHT, default=400): cv.int_range(min=32, max=1024),
        cv.Optional(CONF_NUM_BARS, default=48): cv.int_range(min=4, max=256),
        cv.Optional(CONF_RADIUS_INNER, default=120.0): cv.float_range(min=0, max=1024),
        cv.Optional(CONF_RADIUS_OUTER, default=180.0): cv.float_range(min=0, max=1024),
        cv.Optional(CONF_BAR_WIDTH, default=4): cv.int_range(min=1, max=64),

        cv.Optional(CONF_COLOR_START, default=0x4B00FF): _hex_color,
        cv.Optional(CONF_COLOR_END, default=0x00FFE5): _hex_color,
        cv.Optional(CONF_BG_COLOR, default=0x000000): _hex_color,
        cv.Optional(CONF_BG_OPA, default="TRANSP"): _opacity,

        cv.Optional(CONF_UPDATE_INTERVAL, default="40ms"): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_SMOOTHING, default=0.75): cv.float_range(min=0.0, max=0.99),
        cv.Optional(CONF_GAIN, default=1.8): cv.positive_float,
        cv.Optional(CONF_AUTO_START, default=True): cv.boolean,
        cv.Optional(CONF_BUFFER_SKIP, default=2): cv.int_range(min=1, max=16),
        cv.Optional(CONF_ROTATE_SPEED, default=0.0): cv.float_,
        cv.Optional(CONF_MIRROR, default=False): cv.boolean,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    canvas_obj = await cg.get_variable(config[CONF_CANVAS_ID])
    mic = await cg.get_variable(config[CONF_MICROPHONE])

    cg.add(var.set_target(canvas_obj))
    cg.add(var.set_microphone(mic))
    cg.add(var.set_size(config[CONF_WIDTH], config[CONF_HEIGHT]))
    cg.add(var.set_num_bars(config[CONF_NUM_BARS]))
    cg.add(var.set_radius_inner(config[CONF_RADIUS_INNER]))
    cg.add(var.set_radius_outer(config[CONF_RADIUS_OUTER]))
    cg.add(var.set_bar_width(config[CONF_BAR_WIDTH]))
    cg.add(var.set_color_start(config[CONF_COLOR_START]))
    cg.add(var.set_color_end(config[CONF_COLOR_END]))
    cg.add(var.set_bg_color(config[CONF_BG_COLOR]))
    cg.add(var.set_bg_opa(config[CONF_BG_OPA]))
    cg.add(var.set_update_interval(config[CONF_UPDATE_INTERVAL]))
    cg.add(var.set_smoothing(config[CONF_SMOOTHING]))
    cg.add(var.set_gain(config[CONF_GAIN]))
    cg.add(var.set_auto_start(config[CONF_AUTO_START]))
    cg.add(var.set_buffer_skip(config[CONF_BUFFER_SKIP]))
    cg.add(var.set_rotate_speed(config[CONF_ROTATE_SPEED]))
    cg.add(var.set_mirror(config[CONF_MIRROR]))


# ---- Actions ----

ACTION_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.use_id(ClaudeSpectrum),
})


@automation.register_action("claude_spectrum.start", StartAction, ACTION_SCHEMA)
async def start_action_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, parent)


@automation.register_action("claude_spectrum.stop", StopAction, ACTION_SCHEMA)
async def stop_action_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, parent)
