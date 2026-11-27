# LVGL 9.6 migration (experimental)

Based on the working claude/jolly-lamport-ERZOC branch. The original 9.5 branches
are unchanged. Pins the released lvgl/lvgl 9.6.0 component, sets software draw
unit count explicitly, opts out of upstream native PPA to retain the fork's
custom PPA registration, enables the new LV_USE_THORVG switch for vector
widgets, and preserves the existing allocator and RGB565 flush-byte-order
contract. The old atomic.h shim is no longer installed on ESP-IDF.

Use the same camera and native framebuffer fence configuration as the 9.5
baseline; change only the LVGL external component ref to lvgl-9.6.
CI includes the camera/ortho path and a legacy-PPA/vector build. Compiling is
not proof of device stability, pixel correctness, or performance. Validate
the camera, partial refreshes, touch, SVG/Lottie and legacy PPA separately.
