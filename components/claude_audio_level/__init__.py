"""
claude_audio_level — ESPHome custom component.

A sensor that taps into an existing microphone, computes the RMS audio
level of each incoming buffer, applies exponential smoothing, and
publishes the result as a normalized float (0.0 .. 1.0+) at a
configurable interval.

Intended use: bind the sensor to an LVGL Lottie widget's transform_scale
to make a voice-assistant orb pulse with the user's voice in real time.

See sensor.py for the sensor platform, and claude_audio_level.cpp for
the implementation details.
"""

import esphome.codegen as cg

CODEOWNERS = ["@youkorr"]

claude_audio_level_ns = cg.esphome_ns.namespace("claude_audio_level")
ClaudeAudioLevel = claude_audio_level_ns.class_("ClaudeAudioLevel", cg.Component)
