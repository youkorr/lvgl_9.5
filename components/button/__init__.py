"""
Minimal button component stub for header compatibility.

This component provides the button.h header and the ESPHOME_ENTITY_BUTTON_COUNT
define required by application.h.
"""
import esphome.codegen as cg
import esphome.config_validation as cv

button_ns = cg.esphome_ns.namespace("button")
Button = button_ns.class_("Button", cg.EntityBase)

# CONFIG_SCHEMA is required so ESPHome calls to_code() for this component
# even when it is only AUTO_LOAD'd (no explicit 'button:' entry in YAML).
CONFIG_SCHEMA = cv.Schema({})


async def to_code(config):
    cg.add_global(button_ns.using)
    cg.add_define("ESPHOME_ENTITY_BUTTON_COUNT", 1)
