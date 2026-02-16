from esphome import automation
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_DATE, CONF_ID, CONF_YEAR
from esphome.core import Lambda

from ..automation import action_to_code
from ..defines import CONF_ITEMS, CONF_MAIN, call_lambda, literal
from ..helpers import add_lv_use, lvgl_components_required
from ..lv_validation import lv_int
from ..lvcode import LocalVariable, LvConditional, lv, lv_add
from ..types import LvCompound, LvType, ObjUpdateAction
from . import Widget, WidgetType, get_widgets

CONF_CALENDAR = "calendar"
CONF_TODAY_DATE = "today_date"
CONF_SHOWED_DATE = "showed_date"
CONF_HIGHLIGHTED_DATES = "highlighted_dates"
CONF_MONTH = "month"
CONF_DAY = "day"
CONF_HEADER_MODE = "header_mode"
CONF_DAY_NAMES = "day_names"

# Header type constants matching LVGL documentation
HEADER_ARROW = "arrow"
HEADER_DROPDOWN = "dropdown"
HEADER_NONE = "none"

# Calendar returns selected date as year, month, day
lv_calendar_t = LvType(
    "LvCalendarType",
    parents=(LvCompound,),
    largs=[
        (cg.uint16, "year"),
        (cg.uint8, "month"),
        (cg.uint8, "day"),
    ],
    lvalue=lambda w: [
        w.var.get_selected_year(),
        w.var.get_selected_month(),
        w.var.get_selected_day(),
    ],
    has_on_value=True,
)


def date_schema(required=False):
    """Schema for date specification (year, month, day)"""
    return cv.Schema(
        {
            cv.Required(CONF_YEAR) if required else cv.Optional(CONF_YEAR): cv.int_range(
                min=1970, max=2099
            ),
            cv.Required(CONF_MONTH) if required else cv.Optional(CONF_MONTH): cv.int_range(
                min=1, max=12
            ),
            cv.Required(CONF_DAY) if required else cv.Optional(CONF_DAY): cv.int_range(
                min=1, max=31
            ),
        }
    )


def date_schema_templatable():
    """Schema for date specification with lambda support (for runtime updates)"""
    return cv.Schema(
        {
            cv.Optional(CONF_YEAR): cv.templatable(cv.int_),
            cv.Optional(CONF_MONTH): cv.templatable(cv.int_),
            cv.Optional(CONF_DAY): cv.templatable(cv.int_),
        }
    )


# Schema for runtime updates (header and day_names are creation-time only)
CALENDAR_MODIFY_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_TODAY_DATE): date_schema(),
        cv.Optional(CONF_SHOWED_DATE): date_schema(),
        cv.Optional(CONF_HIGHLIGHTED_DATES): cv.ensure_list(date_schema(required=True)),
    }
)

# Full schema for widget creation
CALENDAR_SCHEMA = CALENDAR_MODIFY_SCHEMA.extend(
    {
        cv.Optional(CONF_HEADER_MODE, default=HEADER_ARROW): cv.one_of(
            HEADER_ARROW, HEADER_DROPDOWN, HEADER_NONE, lower=True
        ),
        cv.Optional(CONF_DAY_NAMES): cv.All(
            cv.ensure_list(cv.string), cv.Length(min=7, max=7)
        ),
    }
)


class CalendarType(WidgetType):
    def __init__(self):
        super().__init__(
            CONF_CALENDAR,
            lv_calendar_t,
            (CONF_MAIN, CONF_ITEMS),
            CALENDAR_SCHEMA,
            modify_schema=CALENDAR_MODIFY_SCHEMA,
            lv_name="calendar",
        )

    async def to_code(self, w: Widget, config):
        """Generate code for calendar widget"""
        lvgl_components_required.add("CALENDAR")

        # Add header for month navigation (per LVGL docs:
        # https://docs.lvgl.io/master/widgets/calendar.html)
        header = config.get(CONF_HEADER_MODE, HEADER_ARROW)
        if header == HEADER_ARROW:
            add_lv_use("CALENDAR_HEADER_ARROW")
            lv.calendar_header_arrow_create(w.obj)
        elif header == HEADER_DROPDOWN:
            add_lv_use("CALENDAR_HEADER_DROPDOWN")
            lv.calendar_header_dropdown_create(w.obj)
            # LVGL default year list only goes to 2025. Set a wider range.
            wid = str(config[CONF_ID])
            year_list = "\\n".join(str(y) for y in range(2036, 2019, -1))
            lv_add(cg.RawExpression(
                f'static const char * {wid}_year_list = "{year_list}"'
            ))
            lv.calendar_header_dropdown_set_year_list(
                w.obj, cg.RawExpression(f"{wid}_year_list")
            )

        # Set custom day names (array of 7 strings)
        if day_names := config.get(CONF_DAY_NAMES):
            wid = str(config[CONF_ID])
            names_str = ", ".join(f'"{name}"' for name in day_names)
            lv_add(cg.RawExpression(
                f"static const char * {wid}_day_names[] = {{{names_str}}}"
            ))
            lv.calendar_set_day_names(
                w.obj, cg.RawExpression(f"{wid}_day_names")
            )

        # Set today's date
        if today := config.get(CONF_TODAY_DATE):
            year = await lv_int.process(today.get(CONF_YEAR, 2024))
            month = await lv_int.process(today.get(CONF_MONTH, 1))
            day = await lv_int.process(today.get(CONF_DAY, 1))
            lv.calendar_set_today_date(w.obj, year, month, day)

        # Set showed date (initial display month)
        # LVGL 9.4 API: lv_calendar_set_month_shown(obj, year, month) - no day param
        if showed := config.get(CONF_SHOWED_DATE):
            year = await lv_int.process(showed.get(CONF_YEAR, 2024))
            month = await lv_int.process(showed.get(CONF_MONTH, 1))
            lv.calendar_set_month_shown(w.obj, year, month)

        # Set highlighted dates
        if highlighted := config.get(CONF_HIGHLIGHTED_DATES):
            dates_count = len(highlighted)
            if dates_count > 0:
                wid = str(config[CONF_ID])
                dates_elements = []
                for date in highlighted:
                    year = date[CONF_YEAR]
                    month = date[CONF_MONTH]
                    day = date[CONF_DAY]
                    dates_elements.append(f"{{{year}, {month}, {day}}}")
                dates_array_str = "{" + ", ".join(dates_elements) + "}"
                lv_add(cg.RawExpression(
                    f"static lv_calendar_date_t {wid}_highlighted_dates[] = {dates_array_str}"
                ))
                lv.calendar_set_highlighted_dates(
                    w.obj,
                    cg.RawExpression(f"{wid}_highlighted_dates"),
                    dates_count,
                )

    def get_uses(self):
        return ("calendar",)


calendar_spec = CalendarType()


@automation.register_action(
    "lvgl.calendar.update",
    ObjUpdateAction,
    cv.Schema(
        {
            cv.Required(CONF_ID): cv.use_id(lv_calendar_t),
            cv.Optional(CONF_TODAY_DATE): date_schema_templatable(),
            cv.Optional(CONF_SHOWED_DATE): date_schema_templatable(),
            cv.Optional(CONF_HIGHLIGHTED_DATES): cv.ensure_list(date_schema(required=True)),
        }
    ),
)
async def calendar_update_to_code(config, action_id, template_arg, args):
    """Handle calendar update action"""
    widgets = await get_widgets(config)

    async def process_date_field(value, default):
        """Process a date field that may be a lambda or a static value"""
        if isinstance(value, Lambda):
            return call_lambda(
                await cg.process_lambda(value, [], return_type=cg.int32)
            )
        return await lv_int.process(value if value is not None else default)

    async def do_calendar_update(w: Widget):
        # Update today's date
        if today := config.get(CONF_TODAY_DATE):
            year = await process_date_field(today.get(CONF_YEAR), 2024)
            month = await process_date_field(today.get(CONF_MONTH), 1)
            day = await process_date_field(today.get(CONF_DAY), 1)
            # Guard against invalid dates (e.g. SNTP not yet synced returns 0)
            has_lambda = any(
                isinstance(today.get(k), Lambda)
                for k in (CONF_YEAR, CONF_MONTH, CONF_DAY)
            )
            if has_lambda:
                with LocalVariable("_td_y", cg.int32, year, modifier="") as y_var:
                    with LvConditional(literal(f"{y_var} > 0")):
                        lv.calendar_set_today_date(w.obj, y_var, month, day)
            else:
                lv.calendar_set_today_date(w.obj, year, month, day)

        # Update showed date
        # LVGL 9.4 API: lv_calendar_set_month_shown(obj, year, month) - no day param
        if showed := config.get(CONF_SHOWED_DATE):
            year = await process_date_field(showed.get(CONF_YEAR), 2024)
            month = await process_date_field(showed.get(CONF_MONTH), 1)
            # Guard against invalid dates (e.g. SNTP not yet synced returns 0)
            has_lambda = any(
                isinstance(showed.get(k), Lambda)
                for k in (CONF_YEAR, CONF_MONTH)
            )
            if has_lambda:
                with LocalVariable("_sd_y", cg.int32, year, modifier="") as y_var:
                    with LvConditional(literal(f"{y_var} > 0")):
                        lv.calendar_set_month_shown(w.obj, y_var, month)
            else:
                lv.calendar_set_month_shown(w.obj, year, month)

        # Update highlighted dates
        if highlighted := config.get(CONF_HIGHLIGHTED_DATES):
            dates_count = len(highlighted)
            if dates_count > 0:
                wid = str(config[CONF_ID])
                dates_elements = []
                for date in highlighted:
                    year = date[CONF_YEAR]
                    month = date[CONF_MONTH]
                    day = date[CONF_DAY]
                    dates_elements.append(f"{{{year}, {month}, {day}}}")
                dates_array_str = "{" + ", ".join(dates_elements) + "}"
                lv_add(cg.RawExpression(
                    f"static lv_calendar_date_t {wid}_hl_dates_upd[] = {dates_array_str}"
                ))
                lv.calendar_set_highlighted_dates(
                    w.obj,
                    cg.RawExpression(f"{wid}_hl_dates_upd"),
                    dates_count,
                )

    return await action_to_code(
        widgets, do_calendar_update, action_id, template_arg, args, config
    )
