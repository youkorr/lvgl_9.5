"""ESPHome LVGL draggable grid component.

100% declaratif : l'utilisateur declare une liste de cases (x, y) et
une liste de boutons LVGL; le composant genere tout le cablage C++ au
build. Aucun lambda requis cote YAML.

Exemple :

    external_components:
      - source: components

    draggable_grid:
      id: my_grid
      cell_w: 150
      cell_h: 100
      cells:
        - [10, 220]
        - [200, 220]
        - [380, 220]
      buttons:
        - btn_light              # string : draggable par defaut
        - id: btn_alarm          # dict : draggable par defaut
        - id: btn_settings       # pinne : garde on_press / on_long_press
          draggable: false       # YAML natif, pas dans le reflow

    # Trigger externe : entree en edit mode depuis un autre bouton
    # (utile quand le bouton lui-meme est pinne ou non-draggable)
    lvgl:
      ...
      widgets:
        - button:
            id: btn_edit_trigger
            on_long_press:
              - draggable_grid.toggle_edit_mode: my_grid
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation
from esphome.const import CONF_ID, CONF_VALUE
from esphome.components.lvgl.types import lv_pseudo_button_t

CODEOWNERS = ["@youkorr"]
DEPENDENCIES = ["lvgl"]

draggable_grid_ns = cg.esphome_ns.namespace("draggable_grid_cmpt")
DraggableGridComponent = draggable_grid_ns.class_(
    "DraggableGridComponent", cg.Component
)
ToggleEditModeAction = draggable_grid_ns.class_(
    "ToggleEditModeAction", automation.Action
)
SetEditModeAction = draggable_grid_ns.class_(
    "SetEditModeAction", automation.Action
)

CONF_CELL_W = "cell_w"
CONF_CELL_H = "cell_h"
CONF_CELLS = "cells"
CONF_BUTTONS = "buttons"
CONF_DRAGGABLE = "draggable"


def _cell(value):
    """Validate a single cell entry [x, y]."""
    if not isinstance(value, list) or len(value) != 2:
        raise cv.Invalid("Each cell must be a 2-element list [x, y]")
    return [cv.int_(v) for v in value]


_BUTTON_DICT_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_ID): cv.use_id(lv_pseudo_button_t),
        cv.Optional(CONF_DRAGGABLE, default=True): cv.boolean,
    }
)


def _button_entry(value):
    """Accepte :
    - une id LVGL nue (draggable=True par defaut)
    - un dict {id: <btn_id>, draggable: true|false}
    """
    if isinstance(value, dict):
        return _BUTTON_DICT_SCHEMA(value)
    return _BUTTON_DICT_SCHEMA({CONF_ID: value})


CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(DraggableGridComponent),
        cv.Optional(CONF_CELL_W, default=150): cv.positive_int,
        cv.Optional(CONF_CELL_H, default=100): cv.positive_int,
        cv.Required(CONF_CELLS): cv.ensure_list(_cell),
        cv.Required(CONF_BUTTONS): cv.ensure_list(_button_entry),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    cells = config[CONF_CELLS]
    buttons = config[CONF_BUTTONS]
    if len(cells) != len(buttons):
        raise cv.Invalid(
            f"'cells' ({len(cells)}) et 'buttons' ({len(buttons)}) "
            "doivent avoir la meme longueur"
        )

    cg.add_global(
        cg.RawStatement(
            '#include "esphome/components/draggable_grid/draggable_grid.h"'
        )
    )

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add(var.set_cell_size(config[CONF_CELL_W], config[CONF_CELL_H]))

    for idx, entry in enumerate(buttons):
        cx, cy = cells[idx]
        btn = await cg.get_variable(entry[CONF_ID])
        cg.add(var.add(btn, idx, cx, cy, entry[CONF_DRAGGABLE]))


# ---------------------------------------------------------------
# Actions : permettent d'entrer / sortir du edit mode depuis
# n'importe quel trigger YAML (on_press, on_long_press, etc.).
# ---------------------------------------------------------------
_ACTION_BASE_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.use_id(DraggableGridComponent),
    }
)


@automation.register_action(
    "draggable_grid.toggle_edit_mode",
    ToggleEditModeAction,
    _ACTION_BASE_SCHEMA,
)
async def toggle_edit_mode_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, parent)


@automation.register_action(
    "draggable_grid.set_edit_mode",
    SetEditModeAction,
    _ACTION_BASE_SCHEMA.extend(
        {
            cv.Required(CONF_VALUE): cv.templatable(cv.boolean),
        }
    ),
)
async def set_edit_mode_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    var = cg.new_Pvariable(action_id, template_arg, parent)
    template_ = await cg.templatable(config[CONF_VALUE], args, bool)
    cg.add(var.set_value(template_))
    return var
