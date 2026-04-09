"""
claude_audio_level sensor platform.

Usage:

    sensor:
      - platform: claude_audio_level
        id: mic_rms
        name: "Mic RMS"
        microphone: esp32_microphone
        update_interval: 33ms
        smoothing: 0.6
        scale: 3.0
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor, microphone
from esphome.const import (
    CONF_ID,
    CONF_MICROPHONE,
    CONF_UPDATE_INTERVAL,
)

from . import ClaudeAudioLevel, claude_audio_level_ns

DEPENDENCIES = ["microphone"]
AUTO_LOAD = []

CONF_SMOOTHING = "smoothing"
CONF_AUTO_START = "auto_start"
CONF_SCALE = "scale"

CONFIG_SCHEMA = (
    sensor.sensor_schema(
        ClaudeAudioLevel,
        accuracy_decimals=3,
    )
    .extend(
        {
            cv.Required(CONF_MICROPHONE): cv.use_id(microphone.Microphone),
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
    cg.add(var.set_update_interval(config[CONF_UPDATE_INTERVAL]))
    cg.add(var.set_smoothing(config[CONF_SMOOTHING]))
    cg.add(var.set_auto_start(config[CONF_AUTO_START]))
    cg.add(var.set_scale(config[CONF_SCALE]))
