"""
claude_audio_level sensor platform.

Usage:

    sensor:
      - platform: claude_audio_level
        id: mic_rms
        name: "Mic RMS"
        microphone: esp32_microphone
        band: level                 # or bass / mid / treble
        update_interval: 33ms
        smoothing: 0.6
        scale: 3.0

For FFT-based band decomposition, declare several sensors on the same
microphone, one per band — each instance runs its own (cheap) 256-point
FFT on the incoming PCM stream. The component only allocates the FFT
working buffers when band is not 'level'.
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
            cv.Optional(CONF_UPDATE_INTERVAL, default="33ms"): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_SMOOTHING, default=0.6): cv.float_range(min=0.0, max=0.99),
            cv.Optional(CONF_AUTO_START, default=True): cv.boolean,
            cv.Optional(CONF_SCALE, default=3.0): cv.positive_float,
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
