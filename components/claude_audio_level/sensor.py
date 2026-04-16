"""
claude_audio_level sensor platform.

Usage:

    sensor:
      - platform: claude_audio_level
        id: mic_rms
        name: "Mic RMS"
        internal: true              # don't send to HA API / don't log every update
        microphone: esp32_microphone
        band: level                 # or bass / mid / treble
        update_interval: 150ms
        smoothing: 0.6
        scale: 3.0
        buffer_skip: 3              # FFT on 1/3 of mic buffers (save CPU for wake word)
        delta_gate: 0.01            # suppress publish if value moved less than this

For FFT-based band decomposition, declare several sensors on the same
microphone, one per band — each instance runs its own (cheap) 256-point
FFT on the incoming PCM stream. The component only allocates the FFT
working buffers when band is not 'level'.

NOTE: set `internal: true` if you only want to drive LVGL animations
from the value; this prevents ESPHome from logging every state change
and from broadcasting each value to Home Assistant over the API, which
is what flooded the logs in the previous revision.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import microphone, sensor
from esphome.const import (
    CONF_ID,
    CONF_MICROPHONE,
    CONF_UPDATE_INTERVAL,
)

from . import ClaudeAudioLevel, claude_audio_level_ns

DEPENDENCIES = ["microphone"]
AUTO_LOAD = []

CONF_BAND = "band"
CONF_SMOOTHING = "smoothing"
CONF_AUTO_START = "auto_start"
CONF_SCALE = "scale"
CONF_BUFFER_SKIP = "buffer_skip"
CONF_DELTA_GATE = "delta_gate"

AudioBand = claude_audio_level_ns.enum("AudioBand", is_class=True)
BANDS = {
    "level": AudioBand.LEVEL,
    "bass": AudioBand.BASS,
    "mid": AudioBand.MID,
    "treble": AudioBand.TREBLE,
}

CONFIG_SCHEMA = (
    sensor.sensor_schema(
        ClaudeAudioLevel,
        accuracy_decimals=3,
    )
    .extend(
        {
            cv.Required(CONF_MICROPHONE): cv.use_id(microphone.Microphone),
            cv.Optional(CONF_BAND, default="level"): cv.enum(BANDS, lower=True),
            # 150 ms publish cadence keeps logs readable and HA API traffic
            # reasonable. Visual bindings driving LVGL style transforms do
            # not benefit from faster updates at the human perception level.
            cv.Optional(CONF_UPDATE_INTERVAL, default="150ms"): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_SMOOTHING, default=0.6): cv.float_range(min=0.0, max=0.99),
            cv.Optional(CONF_AUTO_START, default=True): cv.boolean,
            cv.Optional(CONF_SCALE, default=3.0): cv.positive_float,
            # Process only 1 of every N mic buffers — gives CPU back to the
            # voice-assistant task so wake-word detection stays reliable.
            cv.Optional(CONF_BUFFER_SKIP, default=3): cv.int_range(min=1, max=16),
            # Don't publish state if the value moved less than this since
            # last publish (suppresses near-identical log-spam at idle).
            cv.Optional(CONF_DELTA_GATE, default=0.01): cv.float_range(min=0.0, max=1.0),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    var = await sensor.new_sensor(config)
    await cg.register_component(var, config)

    mic = await cg.get_variable(config[CONF_MICROPHONE])
    cg.add(var.set_microphone(mic))
    cg.add(var.set_band(config[CONF_BAND]))
    cg.add(var.set_update_interval(config[CONF_UPDATE_INTERVAL]))
    cg.add(var.set_smoothing(config[CONF_SMOOTHING]))
    cg.add(var.set_auto_start(config[CONF_AUTO_START]))
    cg.add(var.set_scale(config[CONF_SCALE]))
    cg.add(var.set_buffer_skip(config[CONF_BUFFER_SKIP]))
    cg.add(var.set_delta_gate(config[CONF_DELTA_GATE]))
