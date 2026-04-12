"""
claude_lottie_control — ESPHome component for live-updating Lottie animations.

Exposes the thorvg LottieAnimation extension API (assign, slot gen/apply/del,
tween, segment, quality) as ESPHome automation actions, so YAML can drive
Lottie properties at runtime (e.g. bind a microphone FFT band to the scale
of a layer).

Pattern:
  - component class stores an lv_obj_t* pointing at an existing lv_lottie widget
  - one action per thorvg LottieAnimation method, using cg.register_parented
  - build-time PlatformIO hook patches lv_lottie.c so that the widget
    constructs a tvg::LottieAnimation instead of a plain tvg::Animation
    (required for the extension API to actually work)

Requires, at build time:
  CONFIG_THORVG_LOTTIE_EXPRESSIONS_SUPPORT=y

This pulls in JerryScript (~1.5 MB binary) and is the only way the
`assign()` path ends up doing anything useful — see tvgLottieModel.cpp:650,
which bails out if the target property has no attached expression.

Typical YAML usage:

    external_components:
      - source:
          type: local
          path: components
        components: [claude_lottie_control]

    claude_lottie_control:
      id: orb_ctl_listening
      lottie_id: claude_orb_listening   # id: of the lvgl lottie widget

    sensor:
      - platform: claude_audio_level
        id: claude_bass
        band: bass
        on_value:
          then:
            - claude_lottie.assign:
                id: orb_ctl_listening
                layer: orb_reactive_core
                ix: 201
                var: bass
                val: !lambda 'return x;'
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation
from esphome.components.esp32 import add_idf_sdkconfig_option
from esphome.components.lvgl.widgets.lottie import lv_lottie_t
from esphome.const import CONF_ID

CODEOWNERS = ["@youkorr"]
DEPENDENCIES = ["lvgl"]
MULTI_CONF = True

CONF_LOTTIE_ID = "lottie_id"
CONF_LAYER = "layer"
CONF_IX = "ix"
CONF_VAR = "var"
CONF_VAL = "val"
CONF_SLOT_JSON = "slot_json"
CONF_SLOT_ID = "slot_id"
CONF_MARKER = "marker"
CONF_FROM = "from"
CONF_TO = "to"
CONF_PROGRESS = "progress"
CONF_QUALITY = "quality"

claude_lottie_ns = cg.esphome_ns.namespace("claude_lottie_control")
ClaudeLottieControl = claude_lottie_ns.class_("ClaudeLottieControl", cg.Component)

# ---- Actions ----
AssignAction = claude_lottie_ns.class_("AssignAction", automation.Action)
GenSlotAction = claude_lottie_ns.class_("GenSlotAction", automation.Action)
ApplySlotAction = claude_lottie_ns.class_("ApplySlotAction", automation.Action)
DelSlotAction = claude_lottie_ns.class_("DelSlotAction", automation.Action)
TweenAction = claude_lottie_ns.class_("TweenAction", automation.Action)
SetMarkerAction = claude_lottie_ns.class_("SetMarkerAction", automation.Action)
SetQualityAction = claude_lottie_ns.class_("SetQualityAction", automation.Action)

# Component-level config schema.
# lottie_id references an lv_lottie widget declared in the lvgl: block.
# ESPHome's LVGL component registers each widget variable globally as
# `lv_obj_t *<id>;`, so we can cg.get_variable() it and pass the pointer
# directly into the C++ component via set_target().
CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(ClaudeLottieControl),
        cv.Required(CONF_LOTTIE_ID): cv.use_id(lv_lottie_t),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    # Resolve the lv_lottie widget: the LVGL component declares it globally
    # as `lv_obj_t *<id>;` (see components/lvgl/widgets/__init__.py:162).
    widget_obj = await cg.get_variable(config[CONF_LOTTIE_ID])
    cg.add(var.set_target(widget_obj))

    # Force the thorvg Lottie Expressions support on — pulls in JerryScript
    # (~1.5 MB binary) but is required for the LottieAnimation::assign() path
    # to actually mutate variables at runtime. See tvgLottieModel.cpp: the
    # writables array is only populated when expressions are compiled in.
    add_idf_sdkconfig_option("CONFIG_THORVG_LOTTIE_EXPRESSIONS_SUPPORT", True)

    # Register PlatformIO extra_script that patches the vendored lv_lottie.c
    # so the widget allocates a LottieAnimation instead of a plain Animation.
    # Without this the extension API methods silently reinterpret_cast onto
    # an object with the wrong layout.
    import os
    component_dir = os.path.dirname(__file__)
    build_script_path = os.path.join(component_dir, "claude_lottie_build.py")
    if os.path.exists(build_script_path):
        cg.add_platformio_option(
            "extra_scripts", [f"pre:{build_script_path}"]
        )


# ---- Action registrations ----

LOTTIE_CONTROL_ACTION_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.use_id(ClaudeLottieControl),
})


# assign(layer, ix, var, val)
@automation.register_action(
    "claude_lottie.assign",
    AssignAction,
    LOTTIE_CONTROL_ACTION_SCHEMA.extend({
        cv.Required(CONF_LAYER): cv.templatable(cv.string),
        cv.Required(CONF_IX): cv.templatable(cv.uint32_t),
        cv.Required(CONF_VAR): cv.templatable(cv.string),
        cv.Required(CONF_VAL): cv.templatable(cv.float_),
    }),
)
async def assign_action_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    var = cg.new_Pvariable(action_id, template_arg, parent)
    layer = await cg.templatable(config[CONF_LAYER], args, cg.std_string)
    ix = await cg.templatable(config[CONF_IX], args, cg.uint32)
    vname = await cg.templatable(config[CONF_VAR], args, cg.std_string)
    val = await cg.templatable(config[CONF_VAL], args, cg.float_)
    cg.add(var.set_layer(layer))
    cg.add(var.set_ix(ix))
    cg.add(var.set_var_name(vname))
    cg.add(var.set_val(val))
    return var


# gen_slot(slot_json) -> returns slot id internally (logged at debug level)
@automation.register_action(
    "claude_lottie.gen_slot",
    GenSlotAction,
    LOTTIE_CONTROL_ACTION_SCHEMA.extend({
        cv.Required(CONF_SLOT_JSON): cv.templatable(cv.string),
    }),
)
async def gen_slot_action_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    var = cg.new_Pvariable(action_id, template_arg, parent)
    slot = await cg.templatable(config[CONF_SLOT_JSON], args, cg.std_string)
    cg.add(var.set_slot_json(slot))
    return var


@automation.register_action(
    "claude_lottie.apply_slot",
    ApplySlotAction,
    LOTTIE_CONTROL_ACTION_SCHEMA.extend({
        cv.Required(CONF_SLOT_ID): cv.templatable(cv.uint32_t),
    }),
)
async def apply_slot_action_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    var = cg.new_Pvariable(action_id, template_arg, parent)
    slot_id = await cg.templatable(config[CONF_SLOT_ID], args, cg.uint32)
    cg.add(var.set_slot_id(slot_id))
    return var


@automation.register_action(
    "claude_lottie.del_slot",
    DelSlotAction,
    LOTTIE_CONTROL_ACTION_SCHEMA.extend({
        cv.Required(CONF_SLOT_ID): cv.templatable(cv.uint32_t),
    }),
)
async def del_slot_action_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    var = cg.new_Pvariable(action_id, template_arg, parent)
    slot_id = await cg.templatable(config[CONF_SLOT_ID], args, cg.uint32)
    cg.add(var.set_slot_id(slot_id))
    return var


@automation.register_action(
    "claude_lottie.tween",
    TweenAction,
    LOTTIE_CONTROL_ACTION_SCHEMA.extend({
        cv.Required(CONF_FROM): cv.templatable(cv.float_),
        cv.Required(CONF_TO): cv.templatable(cv.float_),
        cv.Required(CONF_PROGRESS): cv.templatable(cv.float_),
    }),
)
async def tween_action_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    var = cg.new_Pvariable(action_id, template_arg, parent)
    cg.add(var.set_from(await cg.templatable(config[CONF_FROM], args, cg.float_)))
    cg.add(var.set_to(await cg.templatable(config[CONF_TO], args, cg.float_)))
    cg.add(var.set_progress(await cg.templatable(config[CONF_PROGRESS], args, cg.float_)))
    return var


@automation.register_action(
    "claude_lottie.set_marker",
    SetMarkerAction,
    LOTTIE_CONTROL_ACTION_SCHEMA.extend({
        cv.Required(CONF_MARKER): cv.templatable(cv.string),
    }),
)
async def set_marker_action_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    var = cg.new_Pvariable(action_id, template_arg, parent)
    marker = await cg.templatable(config[CONF_MARKER], args, cg.std_string)
    cg.add(var.set_marker(marker))
    return var


@automation.register_action(
    "claude_lottie.set_quality",
    SetQualityAction,
    LOTTIE_CONTROL_ACTION_SCHEMA.extend({
        cv.Required(CONF_QUALITY): cv.templatable(cv.uint8_t),
    }),
)
async def set_quality_action_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    var = cg.new_Pvariable(action_id, template_arg, parent)
    q = await cg.templatable(config[CONF_QUALITY], args, cg.uint8)
    cg.add(var.set_quality(q))
    return var
