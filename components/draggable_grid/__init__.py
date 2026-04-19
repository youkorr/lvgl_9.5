"""ESPHome LVGL draggable grid component.

100% declaratif : l'utilisateur declare une liste de cases (x, y) et
une liste de boutons LVGL; le composant genere tout le cablage C++ au
build. Aucun lambda requis cote YAML.

Exemple :

    external_components:
      - source: components

    draggable_grid:
      cell_w: 150
      cell_h: 100
      cells:
        - [10, 220]
        - [200, 220]
        - [380, 220]
        - [10, 345]
        - [200, 345]
        - [380, 345]
        - [10, 480]
        - [200, 480]
        - [380, 480]
      buttons:
        - btn_light
        - btn_alarm
        - btn_test3
        - btn_player
        - btn_camera
        - btn_test4
        - btn_settings
        - btn_test
        - btn_test2
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID
from esphome.components.lvgl.types import lv_pseudo_button_t

CODEOWNERS = ["@youkorr"]
DEPENDENCIES = ["lvgl"]

draggable_grid_ns = cg.esphome_ns.namespace("draggable_grid_cmpt")
DraggableGridComponent = draggable_grid_ns.class_(
    "DraggableGridComponent", cg.Component
)

CONF_CELL_W = "cell_w"
CONF_CELL_H = "cell_h"
CONF_CELLS = "cells"
CONF_BUTTONS = "buttons"


def _cell(value):
    """Validate a single cell entry [x, y]."""
    if not isinstance(value, list) or len(value) != 2:
        raise cv.Invalid("Each cell must be a 2-element list [x, y]")
    return [cv.int_(v) for v in value]


CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(DraggableGridComponent),
        cv.Optional(CONF_CELL_W, default=150): cv.positive_int,
        cv.Optional(CONF_CELL_H, default=100): cv.positive_int,
        cv.Required(CONF_CELLS): cv.ensure_list(_cell),
        cv.Required(CONF_BUTTONS): cv.ensure_list(cv.use_id(lv_pseudo_button_t)),
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

    cg.add_global(cg.RawStatement('#include "draggable_grid.h"'))

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add(var.set_cell_size(config[CONF_CELL_W], config[CONF_CELL_H]))

    for idx, btn_id in enumerate(buttons):
        cx, cy = cells[idx]
        btn = await cg.get_variable(btn_id)
        cg.add(var.add(btn, idx, cx, cy))
