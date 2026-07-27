#pragma once
// Tiny shared ABI so the LVGL component can present straight into the DPI
// panel's framebuffers (zero-copy swap) without a hard include/RTTI dependency
// on the mipi_dsi class. mipi_dsi::setup() fills this in when framebuffers>=2;
// the LVGL flush path reads it. num_fbs==0 means "not available" (fall back to
// the classic draw_pixels_at copy flush).
#include <cstddef>

namespace esphome {
namespace mipi_dsi {

struct FastPresent {
  void *fb[3];            // real DPI scan-out framebuffers
  int num_fbs;            // 0 = fast present unavailable
  int w;                  // panel width  (framebuffer, unrotated)
  int h;                  // panel height (framebuffer, unrotated)
  void *instance;         // MipiDsi* (opaque)
  void (*present)(void *instance, void *fb);  // swap scan-out to fb (no copy)
};

// C++17 inline variable: a single shared instance across translation units,
// with no link dependency on the mipi_dsi .cpp. If mipi_dsi is not part of the
// build, it simply stays zero-initialised (num_fbs==0 -> fast path disabled),
// so LVGL can include this header unconditionally.
inline FastPresent g_fast_present{};

}  // namespace mipi_dsi
}  // namespace esphome
