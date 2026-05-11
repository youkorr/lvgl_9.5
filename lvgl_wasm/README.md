# lvgl_wasm — LVGL v9 → WebAssembly preview canvas

Compiles LVGL v9 + a tiny C bridge (`main.c`) into `lvgl.js` + `lvgl.wasm`.
The frontend (`web/lvgl/preview.html`) loads the bundle, hands it an HTML
canvas, and LVGL renders into it via JS callbacks.

## Build

CI does it: trigger **Build LVGL WASM preview** in the Actions tab. The
workflow installs Emscripten, runs `lvgl_wasm/build.sh`, and commits the
bundle into `web/lvgl/`. After that, open `web/lvgl/preview.html` (or the
**LVGL preview ↗** link in the main page).

## Local build

```sh
# Requires emsdk (https://emscripten.org/docs/getting_started/downloads.html)
source path/to/emsdk_env.sh
bash lvgl_wasm/build.sh
# Output → lvgl_wasm/dist/{lvgl.js, lvgl.wasm}
```

## Status

**Étape 1** — proves the WASM pipeline works. Renders a static
`ncaseonetwo` placeholder at the dimensions passed via `?w=…&h=…`.

Next steps will translate the user's ESPHome `lvgl:` YAML into LVGL widget
calls, mock sensor / time bindings with animated values, and embed the
canvas back into the main UI as an iframe synchronized with the build
state.
