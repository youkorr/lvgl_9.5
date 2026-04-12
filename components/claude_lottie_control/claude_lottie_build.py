"""
PlatformIO pre-build script for claude_lottie_control.

Patches lv_lottie.c so that the widget allocates a thorvg LottieAnimation
instead of a plain Animation. This is required because LVGL's upstream
lv_lottie.c calls tvg_animation_new() which returns a tvg::Animation, but
the thorvg LottieAnimation extension API (gen/apply_slot, assign, tween,
segment, quality) requires the underlying object to have been constructed
as a tvg::LottieAnimation.

Without this patch the tvg_lottie_animation_* C API functions do a
reinterpret_cast to LottieAnimation* on an object that was actually created
as a plain Animation, and attempting to call Lottie-specific methods (that
access pImpl->picture->loader via the LottieAnimation vtable-less helpers)
is undefined behavior — the code silently reads garbage or crashes.

The patch is idempotent: it detects if it has already been applied by
looking for a marker string inserted into the source file.
"""
import os
import re

Import("env")

# Marker inserted into lv_lottie.c once patched. Detecting this lets us
# safely run the pre-script on every build without corrupting the source
# or re-writing it when nothing has changed.
_PATCH_MARKER = "/* CLAUDE_LOTTIE_PATCH_APPLIED */"


def _candidate_paths():
    """Yield possible locations where lv_lottie.c could live in the build.

    PlatformIO drops git-sourced libraries under .pio/libdeps/<env>/lvgl/.
    ESP-IDF components live under the project or framework tree.
    """
    project_dir = env["PROJECT_DIR"]
    build_dir = env.subst("$BUILD_DIR")
    platform = env.PioPlatform()

    # PlatformIO lib_deps location
    libdeps_dir = env.subst("$PROJECT_LIBDEPS_DIR")
    pio_env = env.subst("$PIOENV")
    yield os.path.join(libdeps_dir, pio_env, "lvgl", "src", "widgets", "lottie", "lv_lottie.c")
    yield os.path.join(libdeps_dir, pio_env, "lvgl", "src", "widgets", "lv_lottie.c")

    # ESP-IDF component directory (less common for ESPHome, but possible)
    yield os.path.join(project_dir, "components", "lvgl", "src", "widgets", "lottie", "lv_lottie.c")

    # .piolibdeps fallback
    yield os.path.join(project_dir, ".piolibdeps", "lvgl", "src", "widgets", "lottie", "lv_lottie.c")


def _find_lv_lottie_c():
    """Locate lv_lottie.c, preferring the first candidate that exists."""
    for path in _candidate_paths():
        if os.path.isfile(path):
            return path

    # Last resort: walk the project's libdeps dir looking for the file.
    libdeps_dir = env.subst("$PROJECT_LIBDEPS_DIR")
    if os.path.isdir(libdeps_dir):
        for root, _dirs, files in os.walk(libdeps_dir):
            if "lv_lottie.c" in files:
                return os.path.join(root, "lv_lottie.c")

    return None


def patch_lv_lottie(source=None, target=None, env=None):
    """Rewrite lv_lottie.c to use tvg_lottie_animation_new().

    Called as both an env callback and directly during SConscript eval,
    hence the optional unused source/target/env params.
    """
    path = _find_lv_lottie_c()
    if path is None:
        print("[claude_lottie] WARNING: lv_lottie.c not found — patch skipped. "
              "The extension API will NOT work until this file is patched.")
        return

    with open(path, "r", encoding="utf-8") as f:
        content = f.read()

    if _PATCH_MARKER in content:
        # Already patched on a previous build.
        return

    # LVGL's lv_lottie_constructor calls tvg_animation_new() to allocate
    # the ThorVG animation handle. We rewrite this single call site to
    # construct a LottieAnimation instead — which is a subclass that
    # provides the Lottie-specific extension methods while remaining
    # layout-compatible with Animation.
    #
    # We only rewrite the specific call site (not all occurrences) by
    # anchoring on the assignment to a member named `tvg_anim`, which is
    # the LVGL 9.x field on lv_lottie_t.
    pattern = re.compile(
        r"(->tvg_anim\s*=\s*)tvg_animation_new\s*\(\s*\)",
        re.MULTILINE,
    )
    new_content, n = pattern.subn(r"\1tvg_lottie_animation_new()", content)

    if n == 0:
        # Fallback: replace any bare `tvg_animation_new()` call in the
        # file — lv_lottie.c only has one, so this is safe.
        pattern2 = re.compile(r"tvg_animation_new\s*\(\s*\)")
        new_content, n = pattern2.subn("tvg_lottie_animation_new()", content)

    if n == 0:
        print(f"[claude_lottie] WARNING: no tvg_animation_new() call found in {path} — "
              "the file may already use a different API or have been modified.")
        return

    # Prepend the marker so we can detect future runs.
    new_content = f"{_PATCH_MARKER}\n{new_content}"

    with open(path, "w", encoding="utf-8") as f:
        f.write(new_content)

    print(f"[claude_lottie] Patched {path} ({n} replacement(s)) → tvg_lottie_animation_new()")


# Run the patch now (during SConscript eval, before compilation).
patch_lv_lottie()
