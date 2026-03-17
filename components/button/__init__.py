"""
Minimal button component stub for header compatibility.

This component provides the button.h header and the ESPHOME_ENTITY_BUTTON_COUNT
define required by application.h. The count is determined at FINAL priority,
after all platform to_code() calls have registered their button entities via
CORE.register_platform_component("button", ...).
"""
import esphome.codegen as cg
from esphome.core import CORE
from esphome.coroutine import CoroPriority, coroutine_with_priority

button_ns = cg.esphome_ns.namespace("button")
Button = button_ns.class_("Button", cg.EntityBase)


@coroutine_with_priority(CoroPriority.CORE)
async def to_code(config):
    cg.add_global(button_ns.using)

    @coroutine_with_priority(CoroPriority.FINAL)
    async def _add_button_count():
        count = max(CORE.platform_counts["button"], 1)
        cg.add_define("ESPHOME_ENTITY_BUTTON_COUNT", count)

    CORE.add_job(_add_button_count)
