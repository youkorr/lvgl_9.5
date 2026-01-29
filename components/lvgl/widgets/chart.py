"""
LVGL v9.4 Chart Widget Implementation

The chart widget displays data visualization with support for:
- LINE charts: Connected line series
- BAR charts: Vertical or horizontal bars
- SCATTER charts: Point-based data
- Multiple series per chart
- Configurable axes and division lines
- Cursors for point selection
"""

from esphome import automation
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import (
    CONF_ID,
    CONF_MAX_VALUE,
    CONF_MIN_VALUE,
    CONF_MODE,
    CONF_TYPE,
    CONF_VALUE,
    CONF_DIRECTION,
)
from esphome.core import Lambda
from esphome.cpp_generator import RawStatement

from ..automation import action_to_code
from ..defines import (
    CONF_CURSOR,
    CONF_INDICATOR,
    CONF_ITEMS,
    CONF_MAIN,
    CONF_SCROLLBAR,
    call_lambda,
    literal,
)
from ..helpers import lvgl_components_required
from ..lv_validation import lv_color, lv_int
from ..lvcode import lv, lv_add, lv_assign, lv_expr, lv_Pvariable
from ..types import LvType, ObjUpdateAction
from . import Widget, WidgetType, get_widgets

CONF_CHART = "chart"
CONF_SERIES = "series"
CONF_POINT_COUNT = "point_count"
CONF_POINTS = "points"
CONF_X_AXIS = "x_axis"
CONF_Y_AXIS = "y_axis"
CONF_DIV_LINE_COUNT = "div_line_count"
CONF_AXIS_PRIMARY_Y = "axis_primary_y"
CONF_AXIS_SECONDARY_Y = "axis_secondary_y"
CONF_AXIS_PRIMARY_X = "axis_primary_x"
CONF_AXIS_SECONDARY_X = "axis_secondary_x"
CONF_UPDATE_MODE = "update_mode"
CONF_COLOR = "color"

# Chart type with on_value support for pressed point detection
lv_chart_t = LvType(
    "lv_chart_t",
    largs=[(cg.int32, "point_index")],
    lvalue=lambda w: lv_expr.chart_get_pressed_point(w.obj),
    has_on_value=True,
)
# Chart series type - use pointer type since lv_chart_series_t is forward-declared in LVGL
lv_chart_series_t_ptr = cg.global_ns.struct("lv_chart_series_t").operator("ptr")
# Chart cursor type - use pointer type since lv_chart_cursor_t is forward-declared in LVGL
lv_chart_cursor_t_ptr = cg.global_ns.struct("lv_chart_cursor_t").operator("ptr")

# Cursor directions
CURSOR_DIRECTIONS = {
    "NONE": "LV_DIR_NONE",
    "LEFT": "LV_DIR_LEFT",
    "RIGHT": "LV_DIR_RIGHT",
    "TOP": "LV_DIR_TOP",
    "BOTTOM": "LV_DIR_BOTTOM",
    "HOR": "LV_DIR_HOR",
    "VER": "LV_DIR_VER",
    "ALL": "LV_DIR_ALL",
}

# Chart types
CHART_TYPE_NONE = "NONE"
CHART_TYPE_LINE = "LINE"
CHART_TYPE_BAR = "BAR"
CHART_TYPE_SCATTER = "SCATTER"

CHART_TYPES = {
    CHART_TYPE_NONE: "LV_CHART_TYPE_NONE",
    CHART_TYPE_LINE: "LV_CHART_TYPE_LINE",
    CHART_TYPE_BAR: "LV_CHART_TYPE_BAR",
    CHART_TYPE_SCATTER: "LV_CHART_TYPE_SCATTER",
}

# Update modes
UPDATE_MODE_SHIFT = "SHIFT"
UPDATE_MODE_CIRCULAR = "CIRCULAR"

UPDATE_MODES = {
    UPDATE_MODE_SHIFT: "LV_CHART_UPDATE_MODE_SHIFT",
    UPDATE_MODE_CIRCULAR: "LV_CHART_UPDATE_MODE_CIRCULAR",
}

# Axis options for series
CONF_AXIS = "axis"
CHART_AXES = {
    "PRIMARY_Y": "LV_CHART_AXIS_PRIMARY_Y",
    "SECONDARY_Y": "LV_CHART_AXIS_SECONDARY_Y",
    "PRIMARY_X": "LV_CHART_AXIS_PRIMARY_X",
    "SECONDARY_X": "LV_CHART_AXIS_SECONDARY_X",
}

# Additional configuration keys
CONF_DIV_LINE_COUNT_HOR = "div_line_count_hor"
CONF_DIV_LINE_COUNT_VER = "div_line_count_ver"
CONF_X_POINTS = "x_points"
CONF_Y_POINTS = "y_points"
CONF_POINT_INDEX = "point_index"
CONF_X_VALUE = "x_value"
CONF_Y_VALUE = "y_value"

# Cursor configuration keys
CONF_CURSORS = "cursors"
CONF_CURSOR_ID = "cursor_id"

# Axis configuration (note: ticks/labels not supported in LVGL 9.x - use scale widget instead)
AXIS_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_MIN_VALUE): lv_int,
        cv.Optional(CONF_MAX_VALUE): lv_int,
        cv.Optional(CONF_DIV_LINE_COUNT): cv.positive_int,
    }
)

# Cursor configuration
CURSOR_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_ID): cv.declare_id(lv_chart_cursor_t_ptr),
        cv.Optional(CONF_COLOR, default=0xFF0000): lv_color,
        cv.Optional(CONF_DIRECTION, default="ALL"): cv.enum(CURSOR_DIRECTIONS, upper=True),
    }
)

# Series configuration
SERIES_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_ID): cv.declare_id(lv_chart_series_t_ptr),
        cv.Optional(CONF_COLOR): lv_color,
        cv.Optional(CONF_AXIS, default="PRIMARY_Y"): cv.enum(CHART_AXES, upper=True),
        cv.Optional(CONF_POINTS): cv.ensure_list(lv_int),  # Y points (or both for LINE/BAR)
        cv.Optional(CONF_X_POINTS): cv.ensure_list(lv_int),  # X points for SCATTER
        cv.Optional(CONF_Y_POINTS): cv.ensure_list(lv_int),  # Y points for SCATTER
    }
)

# Main chart schema
CHART_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_TYPE, default=CHART_TYPE_LINE): cv.enum(CHART_TYPES, upper=True),
        cv.Optional(CONF_POINT_COUNT, default=10): cv.positive_int,
        cv.Optional(CONF_UPDATE_MODE, default=UPDATE_MODE_SHIFT): cv.enum(
            UPDATE_MODES, upper=True
        ),
        cv.Optional(CONF_DIV_LINE_COUNT_HOR, default=3): cv.positive_int,
        cv.Optional(CONF_DIV_LINE_COUNT_VER, default=5): cv.positive_int,
        cv.Optional(CONF_SERIES): cv.ensure_list(SERIES_SCHEMA),
        # Axes configuration
        cv.Optional(CONF_AXIS_PRIMARY_Y): AXIS_SCHEMA,
        cv.Optional(CONF_AXIS_SECONDARY_Y): AXIS_SCHEMA,
        cv.Optional(CONF_AXIS_PRIMARY_X): AXIS_SCHEMA,
        cv.Optional(CONF_AXIS_SECONDARY_X): AXIS_SCHEMA,
        # Cursors for point selection
        cv.Optional(CONF_CURSORS): cv.ensure_list(CURSOR_SCHEMA),
    }
)


class ChartType(WidgetType):
    def __init__(self):
        super().__init__(
            CONF_CHART,
            lv_chart_t,
            (CONF_MAIN, CONF_ITEMS, CONF_INDICATOR, CONF_CURSOR, CONF_SCROLLBAR),
            CHART_SCHEMA,
            modify_schema={},
        )

    async def to_code(self, w: Widget, config):
        """Generate C++ code for chart widget configuration"""
        lvgl_components_required.add(CONF_CHART)

        # Set chart type
        chart_type = CHART_TYPES[config[CONF_TYPE]]
        lv.chart_set_type(w.obj, literal(chart_type))

        # Set point count
        point_count = config[CONF_POINT_COUNT]
        lv.chart_set_point_count(w.obj, point_count)

        # Set update mode
        update_mode = UPDATE_MODES[config[CONF_UPDATE_MODE]]
        lv.chart_set_update_mode(w.obj, literal(update_mode))

        # Set division line counts (horizontal and vertical grid lines)
        div_hor = config[CONF_DIV_LINE_COUNT_HOR]
        div_ver = config[CONF_DIV_LINE_COUNT_VER]
        lv.chart_set_div_line_count(w.obj, div_hor, div_ver)

        # Configure axes
        await self._configure_axis(
            w, config, CONF_AXIS_PRIMARY_Y, "LV_CHART_AXIS_PRIMARY_Y"
        )
        await self._configure_axis(
            w, config, CONF_AXIS_SECONDARY_Y, "LV_CHART_AXIS_SECONDARY_Y"
        )
        await self._configure_axis(
            w, config, CONF_AXIS_PRIMARY_X, "LV_CHART_AXIS_PRIMARY_X"
        )
        await self._configure_axis(
            w, config, CONF_AXIS_SECONDARY_X, "LV_CHART_AXIS_SECONDARY_X"
        )

        # Add series
        if series_list := config.get(CONF_SERIES):
            for series in series_list:
                await self._add_series(w, series)

        # Add cursors
        if cursor_list := config.get(CONF_CURSORS):
            for cursor in cursor_list:
                await self._add_cursor(w, cursor)

    async def _configure_axis(self, w: Widget, config, axis_key, axis_const):
        """Configure a specific axis range"""
        if axis_config := config.get(axis_key):
            axis_literal = literal(axis_const)

            # Set range if specified
            if CONF_MIN_VALUE in axis_config and CONF_MAX_VALUE in axis_config:
                min_val = await lv_int.process(axis_config[CONF_MIN_VALUE])
                max_val = await lv_int.process(axis_config[CONF_MAX_VALUE])
                lv.chart_set_range(w.obj, axis_literal, min_val, max_val)

    async def _add_series(self, w: Widget, series_config):
        """Add a data series to the chart"""
        # Get series color if specified, otherwise use default
        if CONF_COLOR in series_config:
            color = await lv_color.process(series_config[CONF_COLOR])
        else:
            color = literal("lv_palette_main(LV_PALETTE_RED)")

        # Get axis for this series
        axis = CHART_AXES[series_config[CONF_AXIS]]

        # Declare series pointer variable and add series to chart
        series_id = series_config[CONF_ID]
        series_var = lv_Pvariable(cg.global_ns.struct("lv_chart_series_t"), series_id)
        lv_assign(
            series_var,
            lv_expr.chart_add_series(w.obj, color, literal(axis)),
        )

        # Set initial points - for SCATTER charts use x_points/y_points
        x_points = series_config.get(CONF_X_POINTS)
        y_points = series_config.get(CONF_Y_POINTS)

        if x_points and y_points:
            # Scatter chart with X/Y coordinates - use lv_chart_get_x_array/y_array
            # to access arrays via public API (lv_chart_series_t is incomplete type)
            for i, (x_val, y_val) in enumerate(zip(x_points, y_points)):
                x = await lv_int.process(x_val)
                y = await lv_int.process(y_val)
                lv_add(RawStatement(f"lv_chart_get_x_array({w.obj}, {series_var})[{i}] = {x};"))
                lv_add(RawStatement(f"lv_chart_get_y_array({w.obj}, {series_var})[{i}] = {y};"))
            lv.chart_refresh(w.obj)
        elif points := series_config.get(CONF_POINTS):
            # LINE/BAR chart with Y values only
            for point_value in points:
                point = await lv_int.process(point_value)
                lv.chart_set_next_value(w.obj, series_var, point)

    async def _add_cursor(self, w: Widget, cursor_config):
        """Add a cursor to the chart for point selection"""
        # Get cursor color
        color = await lv_color.process(cursor_config[CONF_COLOR])

        # Get cursor direction
        direction = CURSOR_DIRECTIONS[cursor_config[CONF_DIRECTION]]

        # Declare cursor pointer variable and add cursor to chart
        cursor_id = cursor_config[CONF_ID]
        cursor_var = lv_Pvariable(cg.global_ns.struct("lv_chart_cursor_t"), cursor_id)
        lv_assign(
            cursor_var,
            lv_expr.chart_add_cursor(w.obj, color, literal(direction)),
        )

    def get_uses(self):
        """Chart widget uses label for axis labels"""
        return ("label",)


chart_spec = ChartType()

CONF_SERIES_ID = "series_id"

# Schema for set_next_value action
CHART_SET_NEXT_VALUE_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_ID): cv.use_id(lv_chart_t),
        cv.Required(CONF_SERIES_ID): cv.use_id(lv_chart_series_t_ptr),
        cv.Required(CONF_VALUE): cv.templatable(cv.int_),
    }
)


@automation.register_action(
    "lvgl.chart.set_next_value",
    ObjUpdateAction,
    CHART_SET_NEXT_VALUE_SCHEMA,
)
async def chart_set_next_value(config, action_id, template_arg, args):
    """Add a data point to a chart series using SHIFT or CIRCULAR mode"""
    widgets = await get_widgets(config)
    series = await cg.get_variable(config[CONF_SERIES_ID])
    value = config[CONF_VALUE]

    async def do_set_next_value(w: Widget):
        if isinstance(value, Lambda):
            val = await cg.process_lambda(value, [], return_type=cg.int32)
            lv.chart_set_next_value(w.obj, series, call_lambda(val))
        else:
            lv.chart_set_next_value(w.obj, series, value)
        lv.chart_refresh(w.obj)

    return await action_to_code(widgets, do_set_next_value, action_id, template_arg, args)


@automation.register_action(
    "lvgl.chart.refresh",
    ObjUpdateAction,
    cv.maybe_simple_value(
        {
            cv.Required(CONF_ID): cv.use_id(lv_chart_t),
        },
        key=CONF_ID,
    ),
)
async def chart_refresh(config, action_id, template_arg, args):
    """Refresh the chart to update display after data changes"""
    widgets = await get_widgets(config)

    async def do_refresh(w: Widget):
        lv.chart_refresh(w.obj)

    return await action_to_code(widgets, do_refresh, action_id, template_arg, args)


# Schema for set_value_by_id action (for scatter charts and animations)
CHART_SET_VALUE_BY_ID_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_ID): cv.use_id(lv_chart_t),
        cv.Required(CONF_SERIES_ID): cv.use_id(lv_chart_series_t_ptr),
        cv.Required(CONF_POINT_INDEX): cv.templatable(cv.int_),
        cv.Required(CONF_VALUE): cv.templatable(cv.int_),
    }
)


@automation.register_action(
    "lvgl.chart.set_value_by_id",
    ObjUpdateAction,
    CHART_SET_VALUE_BY_ID_SCHEMA,
)
async def chart_set_value_by_id(config, action_id, template_arg, args):
    """Set a specific point value by index (useful for animations)"""
    widgets = await get_widgets(config)
    series = await cg.get_variable(config[CONF_SERIES_ID])
    point_index = config[CONF_POINT_INDEX]
    value = config[CONF_VALUE]

    async def do_set_value(w: Widget):
        if isinstance(point_index, Lambda):
            idx = await cg.process_lambda(point_index, [], return_type=cg.int32)
            idx_val = call_lambda(idx)
        else:
            idx_val = point_index

        if isinstance(value, Lambda):
            val = await cg.process_lambda(value, [], return_type=cg.int32)
            val_val = call_lambda(val)
        else:
            val_val = value

        lv.chart_set_value_by_id(w.obj, series, idx_val, val_val)
        lv.chart_refresh(w.obj)

    return await action_to_code(widgets, do_set_value, action_id, template_arg, args)


# Schema for scatter chart set_value_by_id2 action (X and Y values)
CHART_SET_VALUE_BY_ID2_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_ID): cv.use_id(lv_chart_t),
        cv.Required(CONF_SERIES_ID): cv.use_id(lv_chart_series_t_ptr),
        cv.Required(CONF_POINT_INDEX): cv.templatable(cv.int_),
        cv.Required(CONF_X_VALUE): cv.templatable(cv.int_),
        cv.Required(CONF_Y_VALUE): cv.templatable(cv.int_),
    }
)


@automation.register_action(
    "lvgl.chart.set_value_by_id2",
    ObjUpdateAction,
    CHART_SET_VALUE_BY_ID2_SCHEMA,
)
async def chart_set_value_by_id2(config, action_id, template_arg, args):
    """Set X and Y values for scatter chart point by index.

    Uses direct array access since lv_chart_set_value_by_id2 may not exist
    in all LVGL versions.
    """
    widgets = await get_widgets(config)
    series = await cg.get_variable(config[CONF_SERIES_ID])
    point_index = config[CONF_POINT_INDEX]
    x_value = config[CONF_X_VALUE]
    y_value = config[CONF_Y_VALUE]

    async def do_set_value2(w: Widget):
        if isinstance(point_index, Lambda):
            idx = await cg.process_lambda(point_index, [], return_type=cg.int32)
            idx_val = call_lambda(idx)
        else:
            idx_val = point_index

        if isinstance(x_value, Lambda):
            xv = await cg.process_lambda(x_value, [], return_type=cg.int32)
            x_val = call_lambda(xv)
        else:
            x_val = x_value

        if isinstance(y_value, Lambda):
            yv = await cg.process_lambda(y_value, [], return_type=cg.int32)
            y_val = call_lambda(yv)
        else:
            y_val = y_value

        # Use lv_chart_get_x_array/y_array to access arrays via public API
        # (lv_chart_series_t is an incomplete/forward-declared type)
        lv_add(RawStatement(f"lv_chart_get_x_array({w.obj}, {series})[{idx_val}] = {x_val};"))
        lv_add(RawStatement(f"lv_chart_get_y_array({w.obj}, {series})[{idx_val}] = {y_val};"))
        lv.chart_refresh(w.obj)

    return await action_to_code(widgets, do_set_value2, action_id, template_arg, args)


# Schema for set_series_color action (for dynamic bar recoloring)
CONF_SERIES_COLOR = "series_color"
CHART_SET_SERIES_COLOR_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_ID): cv.use_id(lv_chart_t),
        cv.Required(CONF_SERIES_ID): cv.use_id(lv_chart_series_t_ptr),
        cv.Required(CONF_SERIES_COLOR): lv_color,
    }
)


@automation.register_action(
    "lvgl.chart.set_series_color",
    ObjUpdateAction,
    CHART_SET_SERIES_COLOR_SCHEMA,
)
async def chart_set_series_color(config, action_id, template_arg, args):
    """Change the color of a chart series dynamically"""
    widgets = await get_widgets(config)
    series = await cg.get_variable(config[CONF_SERIES_ID])
    color = await lv_color.process(config[CONF_SERIES_COLOR])

    async def do_set_color(w: Widget):
        lv.chart_set_series_color(w.obj, series, color)
        lv.chart_refresh(w.obj)

    return await action_to_code(widgets, do_set_color, action_id, template_arg, args)


# Schema for set_cursor_point action (move cursor to a point)
CHART_SET_CURSOR_POINT_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_ID): cv.use_id(lv_chart_t),
        cv.Required(CONF_CURSOR_ID): cv.use_id(lv_chart_cursor_t_ptr),
        cv.Required(CONF_SERIES_ID): cv.use_id(lv_chart_series_t_ptr),
        cv.Required(CONF_POINT_INDEX): cv.templatable(cv.int_),
    }
)


@automation.register_action(
    "lvgl.chart.set_cursor_point",
    ObjUpdateAction,
    CHART_SET_CURSOR_POINT_SCHEMA,
)
async def chart_set_cursor_point(config, action_id, template_arg, args):
    """Move a cursor to a specific point on a series"""
    widgets = await get_widgets(config)
    cursor = await cg.get_variable(config[CONF_CURSOR_ID])
    series = await cg.get_variable(config[CONF_SERIES_ID])
    point_index = config[CONF_POINT_INDEX]

    async def do_set_cursor(w: Widget):
        if isinstance(point_index, Lambda):
            idx = await cg.process_lambda(point_index, [], return_type=cg.int32)
            idx_val = call_lambda(idx)
        else:
            idx_val = point_index

        lv.chart_set_cursor_point(w.obj, cursor, series, idx_val)
        lv.chart_refresh(w.obj)

    return await action_to_code(widgets, do_set_cursor, action_id, template_arg, args)
