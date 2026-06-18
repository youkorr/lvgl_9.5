import importlib
import logging
from pathlib import Path
import pkgutil

from esphome.automation import build_automation, validate_automation
import esphome.codegen as cg
from esphome.components.const import (
    CONF_BYTE_ORDER,
    CONF_COLOR_DEPTH,
    CONF_DRAW_ROUNDING,
)
from esphome.components.display import DISPLAY_ROTATIONS, Display
from esphome.components.psram import DOMAIN as PSRAM_DOMAIN
import esphome.config_validation as cv
from esphome.const import (
    CONF_AUTO_CLEAR_ENABLED,
    CONF_BUFFER_SIZE,
    CONF_GROUP,
    CONF_ID,
    CONF_LAMBDA,
    CONF_LOG_LEVEL,
    CONF_ON_BOOT,
    CONF_ON_IDLE,
    CONF_PAGES,
    CONF_ROTATION,
    CONF_TIMEOUT,
    CONF_TRIGGER_ID,
    CONF_TYPE,
)
from esphome.core import CORE, ID, Lambda
from esphome.cpp_generator import MockObj
from esphome.final_validate import full_config
from esphome.helpers import write_file_if_changed
from esphome.yaml_util import load_yaml

from . import defines as df, helpers, lv_validation as lvalid, widgets
from .automation import focused_widgets, layers_to_code, lvgl_update, refreshed_widgets
from .encoders import (
    ENCODERS_CONFIG,
    encoders_to_code,
    get_default_group,
    initial_focus_to_code,
)
from .gradient import GRADIENT_SCHEMA, gradients_to_code
from .keypads import KEYPADS_CONFIG, keypads_to_code
from .lv_validation import lv_bool, lv_images_used
from .lvcode import LvContext, LvglComponent, lvgl_static
from .schemas import (
    DISP_BG_SCHEMA,
    FULL_STYLE_SCHEMA,
    STYLE_REMAP,
    WIDGET_TYPES,
    any_widget_schema,
    container_schema,
    obj_schema,
)
from .styles import styles_to_code, theme_to_code
from .touchscreens import touchscreen_schema, touchscreens_to_code
from .trigger import add_on_boot_triggers, generate_triggers
from .types import IdleTrigger, PlainTrigger, lv_font_t, lv_group_t, lv_style_t, lvgl_ns
from .widgets import (
    LvScrActType,
    Widget,
    add_widgets,
    get_screen_active,
    set_obj_properties,
    styles_used,
)

# Import only what we actually use directly in this file
from .widgets.msgbox import MSGBOX_SCHEMA, msgboxes_to_code
from .widgets.obj import obj_spec  # Used in LVGL_SCHEMA
from .widgets.page import (  # page_spec used in LVGL_SCHEMA
    add_pages,
    generate_page_triggers,
    page_spec,
)

_LOGGER = logging.getLogger(__name__)

# Widget registration happens via WidgetType.__init__ in individual widget files
# The imports below trigger creation of the widget types
# Action registration (lvgl.{widget}.update) happens automatically
# in the WidgetType.__init__ method

for module_info in pkgutil.iter_modules(widgets.__path__):
    importlib.import_module(f".widgets.{module_info.name}", package=__package__)

DOMAIN = "lvgl"
DEPENDENCIES = ["display"]
AUTO_LOAD = ["key_provider", "button"]
CODEOWNERS = ["@youkorr"]  # LVGL 9.5.0 implementation with ThorVG enabled by default
HELLO_WORLD_FILE = "hello_world.yaml"
CONF_USE_PPA = "use_ppa"
CONF_USE_PPA_IMG = "use_ppa_img"
CONF_USE_FPS_BENCHMARK = "fps_benchmark"
CONF_USE_PERF_MONITOR = "perf_monitor"


SIMPLE_TRIGGERS = (
    df.CONF_ON_PAUSE,
    df.CONF_ON_RESUME,
    df.CONF_ON_DRAW_START,
    df.CONF_ON_DRAW_END,
)


def as_macro(macro, value):
    if value is None:
        return f"#define {macro}"
    return f"#define {macro} {value}"


LV_CONF_FILENAME = "lv_conf.h"
LV_CONF_H_FORMAT = """\
#pragma once
{}
"""


def generate_lv_conf_h():
    definitions = [as_macro(m, v) for m, v in df.get_data(df.KEY_LV_DEFINES).items()]
    definitions.sort()
    return LV_CONF_H_FORMAT.format("\n".join(definitions))


def multi_conf_validate(configs: list[dict]):
    displays = [config[df.CONF_DISPLAYS] for config in configs]
    # flatten the display list
    display_list = [disp for disps in displays for disp in disps]
    if len(display_list) != len(set(display_list)):
        raise cv.Invalid("A display ID may be used in only one LVGL instance")
    for config in configs:
        for item in (df.CONF_ENCODERS, df.CONF_KEYPADS):
            for enc in config.get(item, ()):
                if CONF_GROUP not in enc:
                    raise cv.Invalid(
                        f"'{item}' must have an explicit group set when using multiple LVGL instances"
                    )
    base_config = configs[0]
    for config in configs[1:]:
        for item in (
            CONF_LOG_LEVEL,
            CONF_COLOR_DEPTH,
            CONF_BYTE_ORDER,
            df.CONF_TRANSPARENCY_KEY,
        ):
            if base_config[item] != config[item]:
                raise cv.Invalid(
                    f"Config item '{item}' must be the same for all LVGL instances"
                )


def final_validation(config_list):
    if len(config_list) != 1:
        multi_conf_validate(config_list)
    global_config = full_config.get()
    for config in config_list:
        if (pages := config.get(CONF_PAGES)) and all(p[df.CONF_SKIP] for p in pages):
            raise cv.Invalid("At least one page must not be skipped")
        # Resolve color_depth before the display loop.
        # If the user didn't set it, auto-detect from the first MIPI DSI display found.
        user_set_depth = CONF_COLOR_DEPTH in config
        if not user_set_depth:
            has_mipi = any(
                global_config.get_config_for_path(
                    global_config.get_path_for_id(did)[:-1]
                ).get("platform", "") == "mipi_dsi"
                for did in config[df.CONF_DISPLAYS]
            )
            if has_mipi:
                config[CONF_COLOR_DEPTH] = 32
                df.LOGGER.info(
                    "MIPI DSI display detected: auto-selecting color_depth=32 (RGB888). "
                    "Set 'color_depth: 16' in your lvgl: config to use RGB565 instead."
                )
            else:
                config[CONF_COLOR_DEPTH] = 16
        for display_id in config[df.CONF_DISPLAYS]:
            path = global_config.get_path_for_id(display_id)[:-1]
            display = global_config.get_config_for_path(path)
            if CONF_LAMBDA in display or CONF_PAGES in display:
                raise cv.Invalid(
                    "Using lambda: or pages: in display config is not compatible with LVGL"
                )
            if display.get(CONF_AUTO_CLEAR_ENABLED) is True:
                raise cv.Invalid(
                    "Using auto_clear_enabled: true in display config not compatible with LVGL"
                )
            if draw_rounding := display.get(CONF_DRAW_ROUNDING):
                config[CONF_DRAW_ROUNDING] = max(
                    draw_rounding, config[CONF_DRAW_ROUNDING]
                )
            # Warn if user explicitly chose 16-bit on MIPI DSI.
            display_platform = display.get("platform", "")
            if user_set_depth and config[CONF_COLOR_DEPTH] == 16 and display_platform == "mipi_dsi":
                df.LOGGER.warning(
                    "color_depth: 16 (RGB565) with MIPI DSI: PPA acceleration may be "
                    "reduced. Consider color_depth: 32 for best performance."
                )
        buffer_frac = config[CONF_BUFFER_SIZE]
        if CORE.is_esp32 and buffer_frac > 0.5 and PSRAM_DOMAIN not in global_config:
            df.LOGGER.warning("buffer_size: may need to be reduced without PSRAM")
        for image_id in lv_images_used:
            path = global_config.get_path_for_id(image_id)[:-1]
            image_conf = global_config.get_config_for_path(path)
            if image_conf[CONF_TYPE] in ("RGBA", "RGB24"):
                raise cv.Invalid(
                    "Using RGBA or RGB24 in image config not compatible with LVGL", path
                )
        for w in focused_widgets:
            path = global_config.get_path_for_id(w)
            widget_conf = global_config.get_config_for_path(path[:-1])
            if (
                df.CONF_ADJUSTABLE in widget_conf
                and not widget_conf[df.CONF_ADJUSTABLE]
            ):
                raise cv.Invalid(
                    "A non adjustable arc may not be focused",
                    path,
                )
        for w in refreshed_widgets:
            path = global_config.get_path_for_id(w)
            widget_conf = global_config.get_config_for_path(path[:-1])
            if not any(isinstance(v, (Lambda, dict)) for v in widget_conf.values()):
                raise cv.Invalid(
                    f"Widget '{w}' does not have any dynamic properties to refresh",
                )
        # Do per-widget type final validation for update actions
        for widget_type, update_configs in df.get_data(df.KEY_UPDATED_WIDGETS).items():
            for conf in update_configs:
                for id_conf in conf.get(CONF_ID, ()):
                    name = id_conf[CONF_ID]
                    path = global_config.get_path_for_id(name)
                    widget_conf = global_config.get_config_for_path(path[:-1])
                    widget_type.final_validate(name, conf, widget_conf, path[1:])


async def to_code(configs):
    config_0 = configs[0]
    # Global configuration
    cg.add_library("lvgl/lvgl", "9.5.0")
    cg.add_define("USE_LVGL")

    # Add build filter to exclude LVGL platform code not needed for ESP32
    # This reduces compilation time and binary size significantly
    build_filter_script = Path(__file__).parent / "lvgl_build_filter.py"
    cg.add_platformio_option("extra_scripts", [f"pre:{build_filter_script}"])

    # Define ESPHOME_ENTITY_BUTTON_COUNT for ESPHome core compatibility
    # application.h requires this symbol even when no button entities exist.
    # When the user configures button entities, ESPHome core already emits the
    # correct count in defines.h, so we must avoid redefining it (which would
    # trigger a -Wmacro-redefined warning). Only add the 0 fallback when no
    # button platform is configured.
    if not CORE.config.get("button"):
        cg.add_define("ESPHOME_ENTITY_BUTTON_COUNT", 0)

    # suppress default enabling of extra widgets
    df.add_define("_LV_KCONFIG_PRESENT")
    # Memory alignment configuration for LVGL 9.5
    df.add_define("LV_DRAW_BUF_STRIDE_ALIGN", "1")  # LVGL default
    # Keep LV_DRAW_BUF_ALIGN at LVGL default (4). Setting higher values
    # causes crashes because LVGL's internal stack/static buffers can't meet
    # stricter alignment. The custom lv_malloc_core() already provides 64-byte
    # aligned heap allocations for the actual draw buffers on ESP32.
    df.add_define("LV_DRAW_BUF_ALIGN", "4")
    use_ppa = config_0.get(CONF_USE_PPA, False)
    use_ppa_img = config_0.get(CONF_USE_PPA_IMG, False)
    use_fps_benchmark = config_0.get(CONF_USE_FPS_BENCHMARK, False)
    use_perf_monitor = config_0.get(CONF_USE_PERF_MONITOR, False)
    # use_ppa_img implies use_ppa (SRM client needs PPA init)
    if use_ppa_img:
        use_ppa = True
    # These are ESP32-only features and pull in code that doesn't exist on other
    # targets: PPA is ESP32-P4 hardware (sdkconfig.h / driver/ppa.h / esp_timer /
    # FreeRTOS), the FPS benchmark uses Espressif's esp_lvgl_adapter, and the
    # perf_monitor overlay relies on a FreeRTOS-only linker wrap. When the build
    # target is not ESP32 (e.g. the host/native SDL build) they are forced off,
    # so the SAME YAML/include file can be reused unchanged on host and ESP32.
    if not CORE.is_esp32 and (use_ppa or use_ppa_img or use_fps_benchmark or use_perf_monitor):
        _LOGGER.warning(
            "lvgl: use_ppa / use_ppa_img / fps_benchmark / perf_monitor are "
            "ESP32-only; ignoring them on this platform (%s).",
            CORE.target_platform,
        )
        use_ppa = use_ppa_img = use_fps_benchmark = use_perf_monitor = False
    if use_ppa:
        # LVGL 9.5 includes the PPA fix (PR #9162) natively.
        # We keep our custom PPA files as a fallback option.
        # PPA evaluate checks buffer alignment at runtime before claiming tasks.
        cg.add_define("USE_LVGL_PPA")
        ppa_dir = Path(__file__).parent / "ppa"
        cg.add_build_flag(f"-I{ppa_dir}")
    if use_ppa_img:
        # Enable PPA SRM hardware rotation for images (0/90/180/270 degrees)
        cg.add_define("LV_USE_PPA_IMG")
    if use_fps_benchmark:
        # Espressif esp_lvgl_adapter FPS sampler (P10/25/50/75/90 report).
        # lvgl_fps_benchmark_wrapper.cpp includes esphome/core/defines.h
        # and conditionally pulls in the .c — ESPHome only auto-compiles
        # .cpp at the component root.
        cg.add_define("USE_LVGL_FPS_BENCHMARK")
    if use_perf_monitor:
        # On-screen FPS/CPU overlay (bottom-right corner, native LVGL widget).
        # Perf monitor is gated by LV_USE_SYSMON in lv_conf_internal.h, which
        # is itself gated by LV_USE_LOG (text rendering for the label).
        df.add_define("LV_USE_LOG", "1")
        df.add_define("LV_USE_SYSMON", "1")
        df.add_define("LV_USE_PERF_MONITOR", "1")
        df.add_define("LV_USE_PERF_MONITOR_POS", "LV_ALIGN_BOTTOM_RIGHT")
        df.add_define("LV_USE_PERF_MONITOR_LOG_MODE", "0")
        # Signal lvgl_build_filter.py to keep src/debugging/sysmon/* sources
        # (excluded by default to save flash).
        cg.add_build_flag("-DLVGL_USE_SYSMON=1")
        # Linker wrap: sysmon reads idle%% via lv_timer_get_idle() under
        # LV_OS_NONE or lv_os_get_idle_percent() under LV_OS_FREERTOS.
        # Wrap both — our matching __wrap_* live in lvgl_esphome.cpp and
        # return 100 - our_cpu_pct so the overlay reads the same value
        # as the '[D][lvgl]: perf:' log (flush wait excluded).
        cg.add_build_flag("-Wl,--wrap=lv_timer_get_idle")
        cg.add_build_flag("-Wl,--wrap=lv_os_get_idle_percent")
    df.add_define("LV_USE_STDLIB_MALLOC", "LV_STDLIB_CUSTOM")

    # ============================================
    # FEATURES DISABLED BY DEFAULT (enabled conditionally below)
    # ============================================
    # ThorVG, SVG, Lottie, Vector Graphics, Float, Matrix are ONLY
    # enabled when the user actually uses svg: or lottie: widgets.
    # This saves ~500KB-1MB of flash on ESP32 devices with limited storage.
    # The conditional activation happens after widget processing (see below).

    # Explicitly disable heavy features by default - they are enabled
    # conditionally after widget processing if the user actually needs them.
    df.add_define("LV_USE_LIBPNG", "0")
    df.add_define("LV_USE_LIBWEBP", "0")

    # Enable FreeRTOS threading for LVGL draw operations.
    # Required by ThorVG / Lottie which render off the main LVGL task and
    # rely on the OS abstraction's mutexes. Switching to LV_OS_NONE here
    # makes those mutexes no-op and crashes the firmware at boot
    # (OTA rolls back).
    # Side effect: LVGL sysmon's CPU%% overlay reads 100%% — a known LVGL
    # quirk with LV_OS_FREERTOS on ESPHome. Ignore the displayed CPU%%;
    # the FPS and ms numbers are still accurate.
    # Off-ESP32 (e.g. the host/native SDL build) there is no FreeRTOS, so use
    # LVGL's no-OS backend instead — otherwise LVGL pulls in FreeRTOS.h.
    df.add_define("LV_USE_OS", "LV_OS_FREERTOS" if CORE.is_esp32 else "LV_OS_NONE")

    # Refresh period: 15 ms ≈ 66 Hz attempt rate (recommended by LVGL
    # community for smoother sysmon readings; doesn't force higher FPS).
    df.add_define("LV_DEF_REFR_PERIOD", "15")

    # LVGL 9.5: Enable blur/frosted glass support (small code, useful for shadows)
    df.add_define("LV_USE_DRAW_SW_BLUR", "1")

    df.add_define(
        "LV_LOG_LEVEL",
        f"LV_LOG_LEVEL_{df.LV_LOG_LEVELS[config_0[CONF_LOG_LEVEL]]}",
    )
    df.add_define("LV_USE_LOG", "1")
    cg.add_define(
        "LVGL_LOG_LEVEL",
        cg.RawExpression(f"ESPHOME_LOG_LEVEL_{config_0[CONF_LOG_LEVEL]}"),
    )
    df.add_define("LV_COLOR_DEPTH", config_0[CONF_COLOR_DEPTH])
    for font in helpers.lv_fonts_used:
        df.add_define(f"LV_FONT_{font.upper()}")

    if config_0[CONF_COLOR_DEPTH] == 16:
        df.add_define(
            "LV_COLOR_16_SWAP",
            "1" if config_0[CONF_BYTE_ORDER] == "big_endian" else "0",
        )
    df.add_define(
        "LV_COLOR_CHROMA_KEY",
        await lvalid.lv_color.process(config_0[df.CONF_TRANSPARENCY_KEY]),
    )
    cg.add_build_flag("-Isrc")

    cg.add_global(lvgl_ns.using)
    for font in helpers.esphome_fonts_used:
        await cg.get_variable(font)
    default_font = config_0[df.CONF_DEFAULT_FONT]
    if not lvalid.is_lv_font(default_font):
        df.add_define(
            "LV_FONT_CUSTOM_DECLARE", f"LV_FONT_DECLARE(*{df.DEFAULT_ESPHOME_FONT})"
        )
        globfont_id = ID(
            df.DEFAULT_ESPHOME_FONT,
            True,
            type=lv_font_t.operator("ptr").operator("const"),
        )
        # static=False because LV_FONT_CUSTOM_DECLARE creates an extern declaration
        cg.new_variable(
            globfont_id,
            MockObj(await lvalid.lv_font.process(default_font), "->").get_lv_font(),
            static=False,
        )
        df.add_define("LV_FONT_DEFAULT", df.DEFAULT_ESPHOME_FONT)
    else:
        df.add_define("LV_FONT_DEFAULT", await lvalid.lv_font.process(default_font))
    cg.add(lvgl_static.esphome_lvgl_init())
    default_group = get_default_group(config_0)

    for config in configs:
        frac = config[CONF_BUFFER_SIZE]
        if frac >= 0.75:
            frac = 1
        elif frac >= 0.375:
            frac = 2
        elif frac > 0.19:
            frac = 4
        elif frac != 0:
            frac = 8
        displays = [
            await cg.get_variable(display) for display in config[df.CONF_DISPLAYS]
        ]
        lv_component = cg.new_Pvariable(
            config[CONF_ID],
            displays,
            frac,
            config[df.CONF_FULL_REFRESH],
            config[CONF_DRAW_ROUNDING],
            config[df.CONF_RESUME_ON_INPUT],
            config[df.CONF_UPDATE_WHEN_DISPLAY_IDLE],
        )
        await cg.register_component(lv_component, config)
        if paused := config[df.CONF_PAUSED]:
            cg.add(lv_component.set_paused(paused, False))
        Widget.create(config[CONF_ID], lv_component, LvScrActType(), config)

        lv_scr_act = get_screen_active(lv_component)
        async with LvContext():
            cg.add(lv_component.set_big_endian(config[CONF_BYTE_ORDER] == "big_endian"))
            if CONF_ROTATION in config:
                cg.add(lv_component.set_lvgl_rotation(config[CONF_ROTATION]))
            await touchscreens_to_code(lv_component, config)
            await encoders_to_code(lv_component, config, default_group)
            await keypads_to_code(lv_component, config, default_group)
            await theme_to_code(config)
            await gradients_to_code(config)
            await styles_to_code(config)
            await set_obj_properties(lv_scr_act, config)
            await add_widgets(lv_scr_act, config)
            await add_pages(lv_component, config)
            await layers_to_code(lv_component, config)
            await lvgl_update(lv_component, config)
            await msgboxes_to_code(lv_component, config)
            # await disp_update(lv_component.get_disp(), config)
    # Set this directly since we are limited in how many methods can be added to the Widget class.
    Widget.widgets_completed = True
    async with LvContext():
        await generate_triggers()
        for config in configs:
            lv_component = await cg.get_variable(config[CONF_ID])
            await generate_page_triggers(config)
            await initial_focus_to_code(config)
            for conf in config.get(CONF_ON_IDLE, ()):
                templ = await cg.templatable(conf[CONF_TIMEOUT], [], cg.uint32)
                idle_trigger = cg.new_Pvariable(
                    conf[CONF_TRIGGER_ID], lv_component, templ
                )
                await build_automation(idle_trigger, [], conf)
            for trigger_name in SIMPLE_TRIGGERS:
                if conf := config.get(trigger_name):
                    trigger_var = cg.new_Pvariable(conf[CONF_TRIGGER_ID])
                    await build_automation(trigger_var, [], conf)
                    cg.add(
                        getattr(
                            lv_component,
                            f"set_{trigger_name.removeprefix('on_')}_trigger",
                        )(trigger_var)
                    )
            await add_on_boot_triggers(config.get(CONF_ON_BOOT, ()))

    # This must be done after all widgets are created
    for comp in helpers.lvgl_components_required:
        cg.add_define(f"USE_LVGL_{comp.upper()}")
    if {
        "transform_rotation",
        "transform_scale",
        "transform_scale_x",
        "transform_scale_y",
    } & styles_used:
        df.add_define("LV_COLOR_SCREEN_TRANSP", "1")
    # ============================================
    # LV_USE_* WIDGET DEFINES
    # ============================================
    # LVGL's lv_conf_internal.h defaults LV_USE_<WIDGET>=1 for all widgets.
    # lv_theme_default.c references widget _class symbols guarded by these defines.
    # If we exclude a widget's source file via the build filter but don't set
    # LV_USE_<WIDGET>=0, the theme still references the symbol -> linker error.
    #
    # IMPORTANT: Use CANONICAL define names (the ones with #ifndef guards in
    # lv_conf_internal.h). LVGL v9.x uses short names as canonical and creates
    # aliases for long names:
    #   #ifndef LV_USE_BTN -> canonical (we can override)
    #   #define LV_USE_BUTTON LV_USE_BTN -> alias (cannot override without warning)
    #
    # ESPHome lv_uses may contain both long names (from widget self.name, e.g. "button")
    # and short names (from get_uses(), e.g. "btn"). Map all to canonical.
    _TO_CANONICAL = {
        "BUTTON": "BTN",
        "BUTTONMATRIX": "BTNMATRIX",
        "IMAGE": "IMG",
        "IMAGEBUTTON": "IMGBTN",
        "ANIMIMAGE": "ANIMIMG",
        "SPANGROUP": "SPAN",
        "METER": "SCALE",
    }
    # All canonical LV_USE_* widget define names in LVGL v9.x
    _ALL_CANONICAL_WIDGETS = {
        "ANIMIMG", "ARC", "BAR", "BTN", "BTNMATRIX",
        "CALENDAR", "CANVAS", "CHART", "CHECKBOX", "DROPDOWN",
        "IMG", "IMGBTN", "KEYBOARD", "LABEL", "LED",
        "LINE", "LIST", "MENU", "MSGBOX", "ROLLER", "SCALE",
        "SLIDER", "SPAN", "SPINBOX", "SPINNER", "SWITCH",
        "TABLE", "TABVIEW", "TEXTAREA", "TILEVIEW", "WIN",
    }

    # Add ESPHome-specific defines; add LV_USE_* only for non-widget entries
    for use in helpers.lv_uses:
        upper = use.upper()
        cg.add_define(f"USE_LVGL_{upper}")
        cg.add_define(f"USE_{upper}")
        canonical = _TO_CANONICAL.get(upper, upper)
        if canonical not in _ALL_CANONICAL_WIDGETS:
            # Non-widget entry (e.g. LOG, THEME_DEFAULT, USER_DATA)
            df.add_define(f"LV_USE_{upper}")

    # Determine which canonical widget defines are needed
    _used_canonical = set()
    for use in helpers.lv_uses:
        canonical = _TO_CANONICAL.get(use.upper(), use.upper())
        if canonical in _ALL_CANONICAL_WIDGETS:
            _used_canonical.add(canonical)

    # lv_theme_default.c references lv_buttonmatrix_class and lv_button_class
    # unconditionally, so they must always be compiled even if not used in the YAML.
    # CANVAS is required because lv_lottie.h (included by lvgl.h) checks for it
    # via #error even when LV_USE_LOTTIE=0.
    _THEME_REQUIRED_WIDGETS = {"BTNMATRIX", "BUTTON", "CANVAS"}
    _used_canonical |= _THEME_REQUIRED_WIDGETS
    # Also add to lv_uses so the build filter includes the source files
    for w in _THEME_REQUIRED_WIDGETS:
        _reverse = {v: k for k, v in _TO_CANONICAL.items()}
        long_name = _reverse.get(w, w).lower()
        helpers.lv_uses.add(long_name)

    # Set LV_USE_*=1 for used widgets, LV_USE_*=0 for unused (canonical names only)
    for widget in _ALL_CANONICAL_WIDGETS:
        df.add_define(f"LV_USE_{widget}", "1" if widget in _used_canonical else "0")

    # ============================================
    # CONDITIONAL HEAVY FEATURES (based on widget usage)
    # ============================================
    # Only enable ThorVG/SVG/Lottie/Vector Graphics if actually needed.
    # This saves ~500KB-1MB of flash on ESP32 devices.
    needs_thorvg = bool(
        {"THORVG_INTERNAL", "SVG", "LOTTIE", "VECTOR_GRAPHIC"} & helpers.lv_uses
    )

    if needs_thorvg:
        df.add_define("LV_USE_FLOAT", "1")
        df.add_define("LV_USE_MATRIX", "1")
        df.add_define("LV_USE_VECTOR_GRAPHIC", "1")
        df.add_define("LV_USE_THORVG_INTERNAL", "1")
        df.add_define("LV_VG_LITE_THORVG_16PIXELS_ALIGN", "1")
        # Large stack for ThorVG rendering
        df.add_define("LV_DRAW_THREAD_STACK_SIZE", "(48 * 1024)")
        # pngdec only needed for ThorVG image pipeline
        cg.add_library("pngdec", "1.0.1")
        # Signal to lvgl_build_filter.py to compile ThorVG sources
        cg.add_build_flag("-DLVGL_USE_THORVG=1")
        df.LOGGER.info("ThorVG enabled (SVG/Lottie widgets detected)")
    else:
        df.add_define("LV_USE_FLOAT", "0")
        df.add_define("LV_USE_MATRIX", "0")
        df.add_define("LV_USE_VECTOR_GRAPHIC", "0")
        df.add_define("LV_USE_THORVG_INTERNAL", "0")
        df.add_define("LV_USE_SVG", "0")
        df.add_define("LV_USE_LOTTIE", "0")
        # Smaller stack when ThorVG is not used
        df.add_define("LV_DRAW_THREAD_STACK_SIZE", "(8 * 1024)")
        df.LOGGER.info(
            "ThorVG disabled (no SVG/Lottie widgets) - saving ~500KB flash"
        )

    # Image decoders: BMP and GIF are small, enable if image widget is used
    # lv_uses stores names as-is from add_lv_use(): lowercase from widgets, uppercase from helpers.py
    if "image" in helpers.lv_uses or "img" in helpers.lv_uses or "animimg" in helpers.lv_uses:
        df.add_define("LV_USE_BMP", "1")
        df.add_define("LV_USE_GIF", "1")
    else:
        df.add_define("LV_USE_BMP", "0")
        df.add_define("LV_USE_GIF", "0")

    # Signal to lvgl_build_filter.py which widgets are used, so it can
    # skip compiling LVGL widget source files that aren't needed.
    # Format: comma-separated list of lowercase LVGL widget names.
    widget_names = ",".join(sorted(use.lower() for use in helpers.lv_uses))
    cg.add_build_flag(f'-DLVGL_WIDGETS_USED=\\"{widget_names}\\"')

    lv_conf_h_file = CORE.relative_src_path(LV_CONF_FILENAME)
    write_file_if_changed(lv_conf_h_file, generate_lv_conf_h())
    cg.add_build_flag("-DLV_CONF_H=1")
    cg.add_build_flag(f'-DLV_CONF_PATH=\\"{LV_CONF_FILENAME}\\"')
    # Add include path for atomic.h shim (needed for LV_USE_OS=LV_OS_FREERTOS on ESP-IDF)
    # Use absolute path so it works when LVGL compiles from .piolibdeps/
    component_dir = Path(__file__).parent
    cg.add_build_flag(f"-I{component_dir}")

    for prop in df.get_remapped_uses():
        df.LOGGER.warning(
            "Property '%s' is deprecated, use '%s' instead", prop, STYLE_REMAP[prop]
        )
    for warning in df.get_warnings():
        df.LOGGER.warning(warning)


def display_schema(config):
    value = cv.ensure_list(cv.use_id(Display))(config)
    value = value or [cv.use_id(Display)(config)]
    if len(set(value)) != len(value):
        raise cv.Invalid("Display IDs must be unique")
    return value


def add_hello_world(config):
    if df.CONF_WIDGETS not in config and CONF_PAGES not in config:
        df.LOGGER.info(
            "No pages or widgets configured, creating default hello_world page"
        )
        hello_world_path = Path(__file__).parent / HELLO_WORLD_FILE
        config[df.CONF_WIDGETS] = any_widget_schema()(load_yaml(hello_world_path))
    return config


def _theme_schema(value):
    return cv.Schema(
        {
            cv.Optional(name): obj_schema(w).extend(FULL_STYLE_SCHEMA)
            for name, w in WIDGET_TYPES.items()
        }
    )(value)


FINAL_VALIDATE_SCHEMA = final_validation

LVGL_SCHEMA = cv.All(
    container_schema(
        obj_spec,
        cv.polling_component_schema("1s")
        .extend(
            {
                cv.GenerateID(CONF_ID): cv.declare_id(LvglComponent),
                cv.GenerateID(df.CONF_DISPLAYS): display_schema,
                cv.Optional(CONF_COLOR_DEPTH): cv.one_of(16, 32),
                # Rotation handled by the LVGL component itself (PPA SRM
                # hardware on ESP32-P4, software loops as fallback). Accepts
                # 0/90/180/270. When set, it overrides the rotation read from
                # the display: component. DSI panels have no MADCTL hardware
                # rotation, so the framebuffer is rotated by the PPA here.
                cv.Optional(CONF_ROTATION): cv.enum(DISPLAY_ROTATIONS, int=True),
                cv.Optional(
                    df.CONF_DEFAULT_FONT, default="montserrat_14"
                ): lvalid.lv_font,
                cv.Optional(df.CONF_FULL_REFRESH, default=False): cv.boolean,
                cv.Optional(
                    df.CONF_UPDATE_WHEN_DISPLAY_IDLE, default=False
                ): cv.boolean,
                cv.Optional(CONF_DRAW_ROUNDING, default=2): cv.positive_int,
                cv.Optional(CONF_BUFFER_SIZE, default=0): cv.percentage,
                cv.Optional(CONF_LOG_LEVEL, default="ERROR"): cv.one_of(
                    *df.LV_LOG_LEVELS, upper=True
                ),
                cv.Optional(CONF_BYTE_ORDER, default="big_endian"): cv.one_of(
                    "big_endian", "little_endian", lower=True
                ),
                cv.Optional(df.CONF_STYLE_DEFINITIONS): cv.ensure_list(
                    cv.Schema({cv.Required(CONF_ID): cv.declare_id(lv_style_t)}).extend(
                        FULL_STYLE_SCHEMA
                    )
                ),
                cv.Optional(CONF_ON_IDLE): validate_automation(
                    {
                        cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(IdleTrigger),
                        cv.Required(CONF_TIMEOUT): cv.templatable(
                            cv.positive_time_period_milliseconds
                        ),
                    }
                ),
                cv.Optional(CONF_PAGES): cv.ensure_list(container_schema(page_spec)),
                **{
                    cv.Optional(x): validate_automation(
                        {
                            cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(PlainTrigger),
                        },
                        single=True,
                    )
                    for x in SIMPLE_TRIGGERS
                },
                cv.Optional(df.CONF_MSGBOXES): cv.ensure_list(MSGBOX_SCHEMA),
                cv.Optional(df.CONF_PAGE_WRAP, default=True): lv_bool,
                cv.Optional(df.CONF_TOP_LAYER): container_schema(obj_spec),
                cv.Optional(df.CONF_BOTTOM_LAYER): container_schema(obj_spec),
                cv.Optional(
                    df.CONF_TRANSPARENCY_KEY, default=0x000400
                ): lvalid.lv_color,
                cv.Optional(df.CONF_THEME): _theme_schema,
                cv.Optional(df.CONF_GRADIENTS): GRADIENT_SCHEMA,
                cv.Optional(df.CONF_TOUCHSCREENS, default=None): touchscreen_schema,
                cv.Optional(df.CONF_ENCODERS, default=None): ENCODERS_CONFIG,
                cv.Optional(df.CONF_KEYPADS, default=None): KEYPADS_CONFIG,
                cv.GenerateID(df.CONF_DEFAULT_GROUP): cv.declare_id(lv_group_t),
                cv.Optional(df.CONF_RESUME_ON_INPUT, default=True): cv.boolean,
                cv.Optional(df.CONF_PAUSED, default=False): cv.boolean,
                cv.Optional(CONF_USE_PPA, default=False): cv.boolean,
                cv.Optional(CONF_USE_PPA_IMG, default=False): cv.boolean,
                cv.Optional(CONF_USE_FPS_BENCHMARK, default=False): cv.boolean,
                cv.Optional(CONF_USE_PERF_MONITOR, default=False): cv.boolean,
            }
        )
        .extend(DISP_BG_SCHEMA),
    ),
    cv.has_at_most_one_key(CONF_PAGES, df.CONF_LAYOUT),
    add_hello_world,
)


def lvgl_config_schema(config):
    """
    Can't use cv.ensure_list here because it converts an empty config to an empty list,
    rather than a default config.
    """
    if not config or isinstance(config, dict):
        return [LVGL_SCHEMA(config)]
    return cv.Schema([LVGL_SCHEMA])(config)


CONFIG_SCHEMA = lvgl_config_schema
