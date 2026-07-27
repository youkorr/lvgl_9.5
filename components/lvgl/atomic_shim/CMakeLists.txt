# Header-only ESP-IDF component providing an "atomic.h" shim for the managed
# lvgl/lvgl 9.5.0 component (its lv_freertos.c does #include "atomic.h", which
# does not exist on ESP-IDF -- the real header is <freertos/atomic.h>).
#
# Registering INCLUDE_DIRS only exposes the header to components that REQUIRE
# this one, and we cannot edit the managed lvgl component's requirements. So we
# also append this directory to the GLOBAL compile options as an -I flag, which
# reaches every component (including __idf_lvgl) at compile time.
idf_component_register(INCLUDE_DIRS ".")
idf_build_set_property(COMPILE_OPTIONS "-I${CMAKE_CURRENT_LIST_DIR}" APPEND)
