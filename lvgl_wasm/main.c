/* ncaseonetwo · LVGL v9 → WebAssembly bridge
 *
 * Bare-metal LVGL app compiled with Emscripten. Renders into an offscreen
 * pixel buffer, hands every flushed area over to JavaScript via js_blit(),
 * which copies it onto an HTML <canvas>. Time comes from emscripten_get_now().
 *
 * Exported entry points (called from JS via Module.ccall):
 *   lvgl_start(w, h)            — initialise display + show placeholder
 *   lvgl_set_text(const char *) — replace the on-screen label text
 *   lvgl_set_bg(uint32_t rgb)   — repaint screen background
 */

#include <emscripten.h>
#include <emscripten/html5.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"

static lv_display_t *g_disp;
static uint8_t      *g_buf;
static lv_obj_t     *g_label;

EM_JS(void, js_blit,
      (int x1, int y1, int x2, int y2, int ptr, int len), {
  if (Module.lvglBlit) {
    Module.lvglBlit(x1, y1, x2, y2,
                    HEAPU8.subarray(ptr, ptr + len));
  }
});

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px) {
  const int w = area->x2 - area->x1 + 1;
  const int h = area->y2 - area->y1 + 1;
  const int n = w * h;

  /* LVGL v9 with LV_COLOR_DEPTH 32 emits little-endian XRGB8888 (B,G,R,X).
   * Canvas ImageData wants RGBA. Swizzle in place. */
  for (int i = 0; i < n; i++) {
    uint8_t b = px[i * 4 + 0];
    uint8_t g = px[i * 4 + 1];
    uint8_t r = px[i * 4 + 2];
    px[i * 4 + 0] = r;
    px[i * 4 + 1] = g;
    px[i * 4 + 2] = b;
    px[i * 4 + 3] = 0xFF;
  }
  js_blit(area->x1, area->y1, area->x2, area->y2, (int)(uintptr_t)px, n * 4);
  lv_display_flush_ready(disp);
}

static uint32_t tick_cb(void) { return (uint32_t)emscripten_get_now(); }

static void main_loop(void) { lv_timer_handler(); }

EMSCRIPTEN_KEEPALIVE
int lvgl_start(int w, int h) {
  lv_init();
  lv_tick_set_cb(tick_cb);

  const size_t buf_bytes = (size_t)w * h * 4;
  g_buf = (uint8_t *)malloc(buf_bytes);
  if (!g_buf) return -1;

  g_disp = lv_display_create(w, h);
  lv_display_set_buffers(g_disp, g_buf, NULL, buf_bytes,
                         LV_DISPLAY_RENDER_MODE_DIRECT);
  lv_display_set_flush_cb(g_disp, flush_cb);

  lv_obj_t *scr = lv_screen_active();
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x101820), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

  g_label = lv_label_create(scr);
  lv_label_set_text(g_label, "ncaseonetwo\nLVGL 9 / WASM");
  lv_obj_set_style_text_color(g_label, lv_color_hex(0x00f5d4), 0);
  lv_obj_set_style_text_align(g_label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_center(g_label);

  emscripten_set_main_loop(main_loop, 60, 0);
  return 0;
}

EMSCRIPTEN_KEEPALIVE
void lvgl_set_text(const char *t) {
  if (g_label && t) lv_label_set_text(g_label, t);
}

EMSCRIPTEN_KEEPALIVE
void lvgl_set_bg(uint32_t rgb) {
  lv_obj_t *scr = lv_screen_active();
  if (scr) lv_obj_set_style_bg_color(scr, lv_color_hex(rgb), 0);
}

int main(void) { return 0; }
