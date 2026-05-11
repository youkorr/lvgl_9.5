#!/usr/bin/env bash
# Build LVGL v9 + our bridge into lvgl.js / lvgl.wasm.
#
# Expects emsdk to be installed and `emcc` on PATH (the GitHub workflow does
# this for us via mymindstorm/setup-emsdk). Outputs land in dist/.
#
# We don't vendor LVGL in this repo — we clone the upstream tag directly so
# the build is reproducible from a single version pin.

set -euo pipefail

LVGL_TAG="${LVGL_TAG:-v9.2.2}"

HERE="$(cd "$(dirname "$0")" && pwd)"
BUILD="${HERE}/build"
DIST="${HERE}/dist"
LVGL_DIR="${BUILD}/lvgl"

rm -rf "${BUILD}" "${DIST}"
mkdir -p "${BUILD}" "${DIST}"

echo "==> Cloning LVGL ${LVGL_TAG}"
git clone --depth 1 --branch "${LVGL_TAG}" \
  https://github.com/lvgl/lvgl.git "${LVGL_DIR}"

echo "==> Generating lv_conf.h"
# Start from the upstream template and enable it (the template is gated by
# `#if 0` at the top). Our overrides come after via -D flags so we don't have
# to grep+sed every option.
cp "${LVGL_DIR}/lv_conf_template.h" "${BUILD}/lv_conf.h"
sed -i 's/^#if 0 \/\*Set it to "1" to enable content\*\//#if 1/' "${BUILD}/lv_conf.h"

# Collect every LVGL .c file. v9 is structured as src/{core,draw,widgets,...}.
echo "==> Collecting LVGL sources"
mapfile -t LVGL_SRCS < <(find "${LVGL_DIR}/src" -name '*.c')
echo "    ${#LVGL_SRCS[@]} source files"

CFLAGS=(
  -Os
  -DLV_CONF_INCLUDE_SIMPLE
  -DLV_COLOR_DEPTH=32
  -DLV_USE_OS=0
  -DLV_TICK_CUSTOM=0
  -DLV_USE_LOG=0
  -DLV_FONT_MONTSERRAT_14=1
  -DLV_FONT_MONTSERRAT_16=1
  -DLV_FONT_MONTSERRAT_24=1
  -DLV_FONT_DEFAULT="&lv_font_montserrat_16"
  -DLV_USE_PERF_MONITOR=0
  -DLV_USE_MEM_MONITOR=0
  -I"${BUILD}"
  -I"${LVGL_DIR}"
  -I"${LVGL_DIR}/src"
)

EMFLAGS=(
  -s WASM=1
  -s ALLOW_MEMORY_GROWTH=1
  -s INITIAL_MEMORY=16777216
  -s MODULARIZE=1
  -s EXPORT_NAME=createLvglModule
  -s ENVIRONMENT=web
  -s EXPORTED_FUNCTIONS=_main,_lvgl_start,_lvgl_set_text,_lvgl_set_bg,_malloc,_free
  -s EXPORTED_RUNTIME_METHODS=ccall,cwrap,HEAPU8,HEAPU32,UTF8ToString,stringToUTF8
)

echo "==> Compiling with emcc"
emcc "${CFLAGS[@]}" "${EMFLAGS[@]}" \
  "${HERE}/main.c" "${LVGL_SRCS[@]}" \
  -o "${DIST}/lvgl.js"

echo "==> Done"
ls -lh "${DIST}/"
