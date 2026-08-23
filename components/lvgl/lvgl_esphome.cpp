#include "esphome/core/defines.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "lvgl_esphome.h"

#include <utility>  // std::swap

#include "core/lv_obj_class_private.h"

// Portable bits so the component also builds on non-ESP32 targets (host/SDL).
#ifdef USE_ESP32
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"  // esp_ptr_internal()
#include "esp_cache.h"         // esp_cache_msync()
// Zero-copy present into the mipi_dsi DPI framebuffers, only when that component
// is part of the build (its header is then copied next to ours).
#if defined(USE_ESP32_VARIANT_ESP32P4) && __has_include("esphome/components/mipi_dsi/mipi_dsi_fast_present.h")
#include "esphome/components/mipi_dsi/mipi_dsi_fast_present.h"
#define LVGL_HAS_FAST_PRESENT
#include <cstring>  // memcpy
#endif
#else
#include <chrono>
#endif
#if defined(__GLIBC__) || defined(__ANDROID__)
#include <malloc.h>  // malloc_usable_size()
#endif

namespace esphome {
namespace lvgl {
// Monotonic microsecond timestamp used for the perf/FPS accounting. Uses the
// ESP timer on-target and std::chrono elsewhere.
static inline uint64_t lvgl_now_us() {
#ifdef USE_ESP32
  return static_cast<uint64_t>(esp_timer_get_time());
#else
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count());
#endif
}
}  // namespace lvgl
}  // namespace esphome

#ifdef USE_LVGL_PPA
#include "driver/ppa.h"
#include "lv_draw_ppa.h"  // LV_PPA_IMG_MODE_* + the draw-thread diagnostic latch
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
extern "C" {
void lv_draw_ppa_init(void);
void lvgl_port_ppa_v9_init(lv_display_t *display);
}
// One pending display flush handed from flush_cb_ to the flush task.
namespace esphome {
namespace lvgl {
struct FlushJob {
  lv_area_t area;
  uint8_t *buf;
};
}  // namespace lvgl
}  // namespace esphome
#endif

#ifdef USE_LVGL_FPS_BENCHMARK
extern "C" {
void lvgl_fps_attach_v2(lv_display_t *display);
}
#endif

#include <cstring>
#include <numeric>

namespace esphome::lvgl {
static const char *const TAG = "lvgl";

// Published CPU% (real work, flush wait excluded) for the LVGL sysmon
// overlay. Updated each second by loop(). Read by __wrap_lv_timer_get_idle
// below to override lv_sysmon's broken FreeRTOS-mode CPU calculation.
static volatile uint32_t s_cpu_pct = 0;
}  // namespace esphome::lvgl

// Linker wrap (PlatformIO LDFLAGs -Wl,--wrap=lv_timer_get_idle and
// -Wl,--wrap=lv_os_get_idle_percent).
// LVGL sysmon's perf widget reads CPU%% via lv_os_get_idle_percent()
// when LV_USE_OS=LV_OS_FREERTOS (and via lv_timer_get_idle() under
// LV_OS_NONE). Wrap both so the overlay reads our s_cpu_pct regardless
// of the OS mode. Returns 100 - cpu, the "idle %" sysmon expects.
extern "C" uint32_t __wrap_lv_timer_get_idle(void) {
  uint32_t cpu = esphome::lvgl::s_cpu_pct;
  if (cpu > 100) cpu = 100;
  return 100 - cpu;
}

extern "C" uint32_t __wrap_lv_os_get_idle_percent(void) {
  uint32_t cpu = esphome::lvgl::s_cpu_pct;
  if (cpu > 100) cpu = 100;
  return 100 - cpu;
}

namespace esphome::lvgl {

#ifdef USE_LVGL_PPA
/// Dedicated PPA SRM client for display framebuffer rotation (separate from LVGL draw unit).
static ppa_client_handle_t s_display_srm_client = nullptr;

/**
 * Attempt to rotate a display framebuffer using the PPA SRM hardware.
 * Returns true if PPA rotation succeeded, false if software fallback is needed.
 *
 * Angle mapping (PPA uses CCW, ESPHome uses CW):
 *   90° CW  → PPA_SRM_ROTATION_ANGLE_270 (270° CCW)
 *   180°    → PPA_SRM_ROTATION_ANGLE_180
 *   270° CW → PPA_SRM_ROTATION_ANGLE_90  (90° CCW)
 */
static bool ppa_rotate_display_buf(const void *src, void *dst, int32_t w, int32_t h,
                                   display::DisplayRotation rot, size_t src_capacity, size_t dst_capacity) {
  if (s_display_srm_client == nullptr || w < 2 || h < 2)
    return false;
  // A null buffer would silently pass the alignment check below (0 & 127 == 0)
  // and then crash inside esp_cache_msync / the PPA. Reject explicitly.
  if (src == nullptr || dst == nullptr)
    return false;

  // ESP32-P4 PPA requires both buffer address and buffer_size to be aligned
  // to the data cache line size — 64 B by default, 128 B if
  // CONFIG_CACHE_L2_CACHE_LINE_128B=y. Use the larger value so the check
  // passes under both sdkconfigs.
  constexpr uintptr_t CACHE_LINE = 128;
  if ((reinterpret_cast<uintptr_t>(src) & (CACHE_LINE - 1)) != 0)
    return false;
  if ((reinterpret_cast<uintptr_t>(dst) & (CACHE_LINE - 1)) != 0)
    return false;

  ppa_srm_rotation_angle_t ppa_angle;
  int32_t out_w, out_h;
  switch (rot) {
    case display::DISPLAY_ROTATION_90_DEGREES:
      ppa_angle = PPA_SRM_ROTATION_ANGLE_270;
      out_w = h;
      out_h = w;
      break;
    case display::DISPLAY_ROTATION_180_DEGREES:
      ppa_angle = PPA_SRM_ROTATION_ANGLE_180;
      out_w = w;
      out_h = h;
      break;
    case display::DISPLAY_ROTATION_270_DEGREES:
      ppa_angle = PPA_SRM_ROTATION_ANGLE_90;
      out_w = h;
      out_h = w;
      break;
    default:
      return false;
  }

#if LV_COLOR_DEPTH == 32
  constexpr ppa_srm_color_mode_t PPA_CM = PPA_SRM_COLOR_MODE_RGB888;
  constexpr size_t BPP = 3;
#else
  constexpr ppa_srm_color_mode_t PPA_CM = PPA_SRM_COLOR_MODE_RGB565;
  constexpr size_t BPP = 2;
#endif

  size_t out_bytes = (size_t) out_w * out_h * BPP;
  size_t aligned_out_bytes = (out_bytes + CACHE_LINE - 1) & ~(CACHE_LINE - 1);
  size_t in_bytes = ((size_t) w * h * BPP + CACHE_LINE - 1) & ~(CACHE_LINE - 1);

  // Safety: the source read range and the rotated output must fit inside their
  // buffers. If the geometry is inconsistent (e.g. `display:` rotation stacked
  // on top of `lvgl: rotation:`, which swaps dimensions twice), the PPA would
  // otherwise write past rotate_buf_ and corrupt adjacent memory -> garbage
  // pointer -> crash in esp_cache_msync. Fall back to software instead.
  if (in_bytes > src_capacity || aligned_out_bytes > dst_capacity) {
    static bool warned = false;
    if (!warned) {
      warned = true;
      ESP_LOGE(TAG,
               "PPA display rotation skipped: geometry does not fit the buffers "
               "(in=%zuB/cap %zuB, out=%zuB/cap %zuB, %dx%d). This usually means "
               "rotation is applied twice (both `display:` and `lvgl: rotation:`). "
               "Set rotation in only ONE of them.",
               in_bytes, src_capacity, aligned_out_bytes, dst_capacity, (int) w, (int) h);
    }
    return false;
  }

  ppa_srm_oper_config_t cfg = {};
  cfg.in.buffer = (void *) src;
  cfg.in.pic_w = w;
  cfg.in.pic_h = h;
  cfg.in.block_w = w;
  cfg.in.block_h = h;
  cfg.in.srm_cm = PPA_CM;
  cfg.out.buffer = dst;
  cfg.out.buffer_size = aligned_out_bytes;
  cfg.out.pic_w = out_w;
  cfg.out.pic_h = out_h;
  cfg.out.srm_cm = PPA_CM;
  cfg.rotation_angle = ppa_angle;
  cfg.scale_x = 1.0f;  // must be 1.0f, not 0.0f (default after zero-init)
  cfg.scale_y = 1.0f;
  cfg.alpha_update_mode = PPA_ALPHA_NO_CHANGE;
  cfg.mode = PPA_TRANS_MODE_BLOCKING;

  // Cache coherency around the PPA DMA transfer. The PPA driver does NOT
  // maintain cache for user buffers, so without this the rotated frame shows
  // color scintillation + trembling: the PPA writes the output to PSRAM by
  // DMA, but draw_pixels_at() reads it back through the CPU cache and sees
  // stale/partial lines.
  //   - Input: write back the CPU-rendered source so the PPA reads fresh data.
  //   - Output: invalidate after the transfer so the panel push reads fresh data.
  // ONLY for PSRAM (external) buffers: internal SRAM is DMA-coherent and is NOT
  // behind the PSRAM cache, so esp_cache_msync() on it fails with
  // "invalid addr or null pointer". With the internal-SRAM rotation pipeline
  // src/dst live in internal SRAM, so the sync must be skipped there.
  // src/dst are already 128-byte (cache-line) aligned (checked above); the sizes
  // were rounded up to the cache line and validated to fit their buffers above.
  if (esp_ptr_external_ram(src))
    esp_cache_msync((void *) src, in_bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);

  esp_err_t ret = ppa_do_scale_rotate_mirror(s_display_srm_client, &cfg);
  if (ret != ESP_OK) {
    static bool warned = false;
    if (!warned) {
      ESP_LOGW(TAG, "PPA display rotation unavailable (err=%d), using SW fallback", ret);
      warned = true;
    }
    return false;
  }
  // Invalidate the freshly written output so the subsequent panel push does not
  // read stale cache lines (root cause of the color flicker + tremor).
  if (esp_ptr_external_ram(dst))
    esp_cache_msync(dst, aligned_out_bytes, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
  return true;
}
#endif  // USE_LVGL_PPA

static const size_t MIN_BUFFER_FRAC = 8;

static const char *const EVENT_NAMES[] = {
    "NONE",
    "PRESSED",
    "PRESSING",
    "PRESS_LOST",
    "SHORT_CLICKED",
    "LONG_PRESSED",
    "LONG_PRESSED_REPEAT",
    "CLICKED",
    "RELEASED",
    "SCROLL_BEGIN",
    "SCROLL_END",
    "SCROLL",
    "GESTURE",
    "KEY",
    "FOCUSED",
    "DEFOCUSED",
    "LEAVE",
    "HIT_TEST",
    "COVER_CHECK",
    "REFR_EXT_DRAW_SIZE",
    "DRAW_MAIN_BEGIN",
    "DRAW_MAIN",
    "DRAW_MAIN_END",
    "DRAW_POST_BEGIN",
    "DRAW_POST",
    "DRAW_POST_END",
    "DRAW_PART_BEGIN",
    "DRAW_PART_END",
    "VALUE_CHANGED",
    "INSERT",
    "REFRESH",
    "READY",
    "CANCEL",
    "DELETE",
    "CHILD_CHANGED",
    "CHILD_CREATED",
    "CHILD_DELETED",
    "SCREEN_UNLOAD_START",
    "SCREEN_LOAD_START",
    "SCREEN_LOADED",
    "SCREEN_UNLOADED",
    "SIZE_CHANGED",
    "STYLE_CHANGED",
    "LAYOUT_CHANGED",
    "GET_SELF_SIZE",
};

static const unsigned LOG_LEVEL_MAP[] = {
    ESPHOME_LOG_LEVEL_DEBUG, ESPHOME_LOG_LEVEL_INFO,  ESPHOME_LOG_LEVEL_WARN,
    ESPHOME_LOG_LEVEL_ERROR, ESPHOME_LOG_LEVEL_ERROR, ESPHOME_LOG_LEVEL_NONE,

};

std::string lv_event_code_name_for(lv_event_t *event) {
  auto event_code = lv_event_get_code(event);
  if (event_code < sizeof(EVENT_NAMES) / sizeof(EVENT_NAMES[0])) {
    return EVENT_NAMES[event_code];
  }
  return str_sprintf("%2d", event_code);
}

static void rounder_cb(lv_event_t *event) {
  auto *comp = static_cast<LvglComponent *>(lv_event_get_user_data(event));
  auto *area = static_cast<lv_area_t *>(lv_event_get_param(event));
  // cater for display driver chips with special requirements for bounds of partial
  // draw areas. Extend the draw area to satisfy:
  // * Coordinates must be a multiple of draw_rounding
  auto draw_rounding = comp->draw_rounding;
  // round down the start coordinates
  area->x1 = area->x1 / draw_rounding * draw_rounding;
  area->y1 = area->y1 / draw_rounding * draw_rounding;
  // round up the end coordinates
  area->x2 = (area->x2 + draw_rounding) / draw_rounding * draw_rounding - 1;
  area->y2 = (area->y2 + draw_rounding) / draw_rounding * draw_rounding - 1;
}

void LvglComponent::render_end_cb(lv_event_t *event) {
  auto *comp = static_cast<LvglComponent *>(lv_event_get_user_data(event));
  comp->draw_end_();
}

void LvglComponent::render_start_cb(lv_event_t *event) {
  ESP_LOGVV(TAG, "Draw start");
  auto *comp = static_cast<LvglComponent *>(lv_event_get_user_data(event));
  comp->draw_start_();
}

lv_event_code_t lv_api_event;     // NOLINT
lv_event_code_t lv_update_event;  // NOLINT
void LvglComponent::dump_config() {
  ESP_LOGCONFIG(TAG,
                "LVGL:\n"
                "  Display width/height: %d x %d\n"
                "  Buffer size: %zu%%\n"
                "  Rotation: %d\n"
                "  Draw rounding: %d",
                this->width_, this->height_, 100 / this->buffer_frac_, this->rotation, (int) this->draw_rounding);
#ifdef USE_LVGL_PPA
  ESP_LOGCONFIG(TAG, "  PPA SRM (display rotation): %s",
                s_display_srm_client != nullptr ? "registered (HW)" : "failed (SW fallback)");
  ESP_LOGCONFIG(TAG, "  PPA SW-blend handler (v9):  registered (RGB565 fills/blends → HW)");
  ESP_LOGCONFIG(TAG, "  PPA draw unit:              registered (canvas/image → HW)");
#else
  ESP_LOGCONFIG(TAG, "  PPA acceleration: disabled (use_ppa: false)");
#endif
}

void LvglComponent::set_paused(bool paused, bool show_snow) {
  this->paused_ = paused;
  this->show_snow_ = show_snow;
  if (!paused && lv_screen_active() != nullptr) {
    lv_display_trigger_activity(this->disp_);  // resets the inactivity time
    lv_obj_invalidate(lv_screen_active());
  }
  if (paused && this->pause_callback_ != nullptr)
    this->pause_callback_->trigger();
  if (!paused && this->resume_callback_ != nullptr)
    this->resume_callback_->trigger();
}

void LvglComponent::esphome_lvgl_init() {
  lv_init();
#ifdef USE_LVGL_PPA
  // Two PPA paths active at once for max coverage:
  //
  //   1) lv_draw_ppa unit (full draw unit, lv_draw_ppa_init)
  //      → accelerates IMAGE draw tasks (canvas widget, lv_image)
  //      → critical for camera streaming through lv_canvas
  //
  //   2) lvgl_ppa_accel_v9 (SW-blend handler, lvgl_port_ppa_v9_init)
  //      → accelerates RGB565 fills/blends in the SW pipeline
  //      → catches what the draw unit rejects (radius != 0, opa < max,
  //        gradients, etc.)
  //
  // Espressif's esp_lvgl_adapter only uses (2), but that leaves canvas/
  // image draws going through the slow SW image renderer. With a 640x480
  // RGB565 camera canvas, this added ~50 ms of LVGL overhead per frame.
  // Enabling (1) brings image drawing back onto PPA hardware.
  lv_draw_ppa_init();

  // Register a dedicated PPA SRM client for display framebuffer rotation.
  // This is independent of the LVGL draw pipeline and stays enabled.
  if (s_display_srm_client == nullptr) {
    ppa_client_config_t srm_cfg = {};
    srm_cfg.oper_type = PPA_OPERATION_SRM;
    srm_cfg.max_pending_trans_num = 1;
    srm_cfg.data_burst_length = PPA_DATA_BURST_LENGTH_64;
    esp_err_t srm_ret = ppa_register_client(&srm_cfg, &s_display_srm_client);
    if (srm_ret == ESP_OK) {
      ESP_LOGI(TAG, "PPA display rotation SRM client registered (HW rotation active)");
    } else {
      // Not silent: this is the difference between HW rotation (0% CPU) and the
      // SW fallback loops (high CPU). On the Tab5 / ESP32-P4 rev v1.0 this is
      // typically chip-revision gating in IDF, not a hardware defect.
      ESP_LOGE(TAG,
               "PPA SRM client registration FAILED (err=%d/%s) -> display rotation will run on the "
               "CPU. This is an ESP-IDF/sdkconfig chip-revision gating issue (CONFIG_ESP32P4_REV_MIN), "
               "not this code. Known-good: ESP-IDF 5.5.2 / platform 55.03.35.",
               srm_ret, esp_err_to_name(srm_ret));
      s_display_srm_client = nullptr;
    }
  }
#endif
  lv_tick_set_cb([] { return millis(); });
  lv_update_event = static_cast<lv_event_code_t>(lv_event_register_id());
  lv_api_event = static_cast<lv_event_code_t>(lv_event_register_id());
}

void LvglComponent::add_event_cb(lv_obj_t *obj, event_callback_t callback, lv_event_code_t event) {
  lv_obj_add_event_cb(obj, callback, event, nullptr);
}

void LvglComponent::add_event_cb(lv_obj_t *obj, event_callback_t callback, lv_event_code_t event1,
                                 lv_event_code_t event2) {
  add_event_cb(obj, callback, event1);
  add_event_cb(obj, callback, event2);
}

void LvglComponent::add_event_cb(lv_obj_t *obj, event_callback_t callback, lv_event_code_t event1,
                                 lv_event_code_t event2, lv_event_code_t event3) {
  add_event_cb(obj, callback, event1);
  add_event_cb(obj, callback, event2);
  add_event_cb(obj, callback, event3);
}

void LvglComponent::add_page(LvPageType *page) {
  this->pages_.push_back(page);
  page->set_parent(this);
  lv_display_set_default(this->disp_);
  page->setup(this->pages_.size() - 1);
}

void LvglComponent::show_page(size_t index, lv_scr_load_anim_t anim, uint32_t time) {
  if (index >= this->pages_.size())
    return;
  this->current_page_ = index;
  lv_scr_load_anim(this->pages_[this->current_page_]->obj, anim, time, 0, false);
}

void LvglComponent::show_next_page(lv_scr_load_anim_t anim, uint32_t time) {
  if (this->pages_.empty() || (this->current_page_ == this->pages_.size() - 1 && !this->page_wrap_))
    return;
  auto start = this->current_page_;
  do {
    this->current_page_ = (this->current_page_ + 1) % this->pages_.size();
    if (this->current_page_ == start)
      return;  // all pages are skipped, avoid infinite loop
  } while (this->pages_[this->current_page_]->skip);
  this->show_page(this->current_page_, anim, time);
}

void LvglComponent::show_prev_page(lv_scr_load_anim_t anim, uint32_t time) {
  if (this->pages_.empty() || (this->current_page_ == 0 && !this->page_wrap_))
    return;
  auto start = this->current_page_;
  do {
    this->current_page_ = (this->current_page_ + this->pages_.size() - 1) % this->pages_.size();
    if (this->current_page_ == start)
      return;  // all pages are skipped, avoid infinite loop
  } while (this->pages_[this->current_page_]->skip);
  this->show_page(this->current_page_, anim, time);
}

size_t LvglComponent::get_current_page() const { return this->current_page_; }
bool LvPageType::is_showing() const { return this->parent_->get_current_page() == this->index; }

void LvglComponent::draw_buffer_(const lv_area_t *area, lv_color_data *ptr) {
  auto width = lv_area_get_width(area);
  auto height = lv_area_get_height(area);
  auto height_rounded = (height + this->draw_rounding - 1) / this->draw_rounding * this->draw_rounding;
  auto x1 = area->x1;
  auto y1 = area->y1;
  auto *dst = reinterpret_cast<lv_color_data *>(this->rotate_buf_);

#ifdef LVGL_HAS_FAST_PRESENT
  // Zero-copy present: mipi_dsi with >=2 DPI framebuffers + LVGL full-refresh.
  // full-refresh makes every flush cover the whole screen, so we can PPA-rotate
  // (or copy for rotation 0) the frame straight into a real scan-out buffer and
  // swap it in — no draw_pixels_at copy into the scanned FB, no blocking wait.
  if (this->full_refresh_ && mipi_dsi::g_fast_present.num_fbs >= 2 && mipi_dsi::g_fast_present.present != nullptr &&
      width == static_cast<int32_t>(this->width_) && height == static_cast<int32_t>(this->height_)) {
#if LV_COLOR_DEPTH == 32
    constexpr size_t FP_BPP = 4;
#else
    constexpr size_t FP_BPP = LV_COLOR_DEPTH / 8;
#endif
    void *fb = mipi_dsi::g_fast_present.fb[this->fast_fb_index_];
    size_t fb_bytes = static_cast<size_t>(mipi_dsi::g_fast_present.w) * mipi_dsi::g_fast_present.h * FP_BPP;
    bool done = false;
#ifdef USE_LVGL_PPA
    if (s_display_srm_client != nullptr && this->rotation != display::DISPLAY_ROTATION_0_DEGREES) {
      done = ppa_rotate_display_buf(ptr, fb, width, height, this->rotation, this->buf_bytes_, fb_bytes);
    }
#endif
    if (!done && this->rotation == display::DISPLAY_ROTATION_0_DEGREES) {
      std::memcpy(fb, ptr, static_cast<size_t>(width) * height * FP_BPP);
      done = true;
    }
    if (done) {
      mipi_dsi::g_fast_present.present(mipi_dsi::g_fast_present.instance, fb);
      this->fast_fb_index_ = (this->fast_fb_index_ + 1) % mipi_dsi::g_fast_present.num_fbs;
      return;
    }
    // Rotation requested but PPA unavailable -> fall through to the normal path.
  }
#endif  // LVGL_HAS_FAST_PRESENT

#ifdef USE_LVGL_PPA
  // Try PPA hardware rotation first (zero CPU cost, ~10x faster than SW loops).
  // Falls back to software automatically if PPA rejects the operation.
  if (s_display_srm_client != nullptr && this->rotation != display::DISPLAY_ROTATION_0_DEGREES) {
    if (ppa_rotate_display_buf(ptr, this->rotate_buf_, width, height, this->rotation, this->buf_bytes_,
                               this->buf_bytes_)) {
      // dst already points to rotate_buf_ (initialized above)
      // Coordinate update: identical geometry to the software path
      switch (this->rotation) {
        case display::DISPLAY_ROTATION_90_DEGREES:
          y1 = x1;
          x1 = this->height_ - area->y1 - height;
          height = width;
          width = height_rounded;
          break;
        case display::DISPLAY_ROTATION_180_DEGREES:
          x1 = this->width_ - x1 - width;
          y1 = this->height_ - y1 - height;
          break;
        case display::DISPLAY_ROTATION_270_DEGREES:
          x1 = y1;
          y1 = this->width_ - area->x1 - width;
          height = width;
          width = height_rounded;
          break;
        default:
          break;
      }
      for (auto *display : this->displays_) {
        display->draw_pixels_at(x1, y1, width, height, (const uint8_t *) dst, display::COLOR_ORDER_RGB, LV_BITNESS,
                                this->big_endian_);
      }
      return;
    }
    // PPA refused this op → fall through to software rotation below.
  }
  // Loud, one-time diagnostic: if PPA is compiled in and we are actually
  // rotating but the rotation is running on the CPU (SW loops below), the
  // hardware accelerator is NOT being used. This is the "no silent CPU
  // fallback" requirement: on a board where ppa_register_client() failed
  // (e.g. chip-revision gating in sdkconfig/IDF), the user must SEE it in the
  // boot log instead of silently paying the CPU cost.
  if (this->rotation != display::DISPLAY_ROTATION_0_DEGREES) {
    static bool sw_rotation_reported = false;
    if (!sw_rotation_reported) {
      sw_rotation_reported = true;
      if (s_display_srm_client == nullptr) {
        ESP_LOGE(TAG,
                 "Display rotation is running on the CPU: PPA SRM client is NOT registered. "
                 "The HW accelerator is unused and CPU load will be high. This is almost always "
                 "ESP-IDF chip-revision gating (CONFIG_ESP32P4_REV_MIN) refusing PPA on this "
                 "silicon/IDF combo — fix it in the YAML sdkconfig, not here. A known-good combo "
                 "is ESP-IDF 5.5.2 / platform 55.03.35 (the Waveshare config).");
      } else {
        ESP_LOGE(TAG,
                 "Display rotation fell back to CPU: PPA SRM client is registered but rejected the "
                 "operation (check buffer cache-line alignment and dimensions).");
      }
    }
  }
#endif  // USE_LVGL_PPA

  switch (this->rotation) {
    case display::DISPLAY_ROTATION_90_DEGREES:
#if LV_COLOR_DEPTH == 32
      {
        // RGB888: 3 bytes per pixel
        auto *dst8 = reinterpret_cast<uint8_t *>(this->rotate_buf_);
        auto *ptr8 = reinterpret_cast<const uint8_t *>(ptr);
        for (lv_coord_t x = height; x-- != 0;) {
          for (lv_coord_t y = 0; y != width; y++) {
            size_t out = (size_t(y) * height_rounded + x) * 3;
            dst8[out + 0] = *ptr8++;
            dst8[out + 1] = *ptr8++;
            dst8[out + 2] = *ptr8++;
          }
        }
      }
#else
      for (lv_coord_t x = height; x-- != 0;) {
        for (lv_coord_t y = 0; y != width; y++) {
          dst[y * height_rounded + x] = *ptr++;
        }
      }
#endif
      y1 = x1;
      x1 = this->height_ - area->y1 - height;
      height = width;
      width = height_rounded;
      break;

    case display::DISPLAY_ROTATION_180_DEGREES:
#if LV_COLOR_DEPTH == 32
      {
        // RGB888: 3 bytes per pixel
        auto *dst8 = reinterpret_cast<uint8_t *>(this->rotate_buf_);
        auto *ptr8 = reinterpret_cast<const uint8_t *>(ptr);
        for (lv_coord_t y = height; y-- != 0;) {
          for (lv_coord_t x = width; x-- != 0;) {
            size_t out = (size_t(y) * width + x) * 3;
            dst8[out + 0] = *ptr8++;
            dst8[out + 1] = *ptr8++;
            dst8[out + 2] = *ptr8++;
          }
        }
      }
#else
      for (lv_coord_t y = height; y-- != 0;) {
        for (lv_coord_t x = width; x-- != 0;) {
          dst[y * width + x] = *ptr++;
        }
      }
#endif
      x1 = this->width_ - x1 - width;
      y1 = this->height_ - y1 - height;
      break;

    case display::DISPLAY_ROTATION_270_DEGREES:
#if LV_COLOR_DEPTH == 32
      {
        // RGB888: 3 bytes per pixel
        auto *dst8 = reinterpret_cast<uint8_t *>(this->rotate_buf_);
        auto *ptr8 = reinterpret_cast<const uint8_t *>(ptr);
        for (lv_coord_t x = 0; x != height; x++) {
          for (lv_coord_t y = width; y-- != 0;) {
            size_t out = (size_t(y) * height_rounded + x) * 3;
            dst8[out + 0] = *ptr8++;
            dst8[out + 1] = *ptr8++;
            dst8[out + 2] = *ptr8++;
          }
        }
      }
#else
      for (lv_coord_t x = 0; x != height; x++) {
        for (lv_coord_t y = width; y-- != 0;) {
          dst[y * height_rounded + x] = *ptr++;
        }
      }
#endif
      x1 = y1;
      y1 = this->width_ - area->x1 - width;
      height = width;
      width = height_rounded;
      break;

    default:
      dst = ptr;
      break;
  }
  for (auto *display : this->displays_) {
    display->draw_pixels_at(x1, y1, width, height, (const uint8_t *) dst, display::COLOR_ORDER_RGB, LV_BITNESS,
                            this->big_endian_);
  }
}

void LvglComponent::flush_cb_(lv_display_t *disp_drv, const lv_area_t *area, uint8_t *color_p) {
  if (this->is_paused()) {
    lv_display_flush_ready(disp_drv);
    return;
  }
#ifdef USE_LVGL_PPA
  // Pipelined path: hand the rotation + panel push to the flush task and return
  // immediately. With double buffering, LVGL renders the next frame into the
  // other buffer while the task rotates+pushes this one. flush_ready is called
  // from the task when the push completes.
  if (this->async_flush_) {
    FlushJob job;
    job.area = *area;
    job.buf = color_p;
    // Queue has room (LVGL issues at most one flush before flush_ready); block
    // briefly just in case so we never drop a frame.
    xQueueSend(static_cast<QueueHandle_t>(this->flush_queue_), &job, portMAX_DELAY);
    return;
  }
#endif
  uint64_t t0 = lvgl_now_us();
  this->draw_buffer_(area, reinterpret_cast<lv_color_data *>(color_p));
  uint64_t dt = lvgl_now_us() - t0;
  // Track flush wait time so loop() can subtract it when computing
  // CPU%% — the synchronous DMA push isn't real CPU work.
  this->perf_flush_us_ += dt;
  ESP_LOGV(TAG, "flush_cb, area=%d/%d, %d/%d took %llu us", area->x1, area->y1, lv_area_get_width(area),
           lv_area_get_height(area), (unsigned long long)dt);
  lv_display_flush_ready(disp_drv);
}

#ifdef USE_LVGL_PPA
void LvglComponent::flush_task_entry_(void *arg) { static_cast<LvglComponent *>(arg)->flush_task_loop_(); }

void LvglComponent::flush_task_loop_() {
  auto queue = static_cast<QueueHandle_t>(this->flush_queue_);
  auto done = static_cast<SemaphoreHandle_t>(this->flush_done_sem_);
  FlushJob job;
  for (;;) {
    if (xQueueReceive(queue, &job, portMAX_DELAY) == pdTRUE) {
      // Heavy work only: rotate (PPA) + panel push. This task NEVER calls any
      // LVGL function — that would race with lv_timer_handler on the main loop
      // (the previous version crashed with a Core 1 panic). Instead we signal
      // completion and let loop() call lv_display_flush_ready on the main thread.
      this->draw_buffer_(&job.area, reinterpret_cast<lv_color_data *>(job.buf));
      xSemaphoreGive(done);
    }
  }
}

// Called by LVGL (on the main thread) when it must wait for an outstanding
// flush to finish before reusing a draw buffer. We block on the flush task's
// completion semaphore and then mark the flush ready — all on the main thread,
// so lv_display_flush_ready is never called concurrently with lv_timer_handler.
void LvglComponent::flush_wait_cb_(lv_display_t *disp) {
  auto *self = static_cast<LvglComponent *>(lv_display_get_user_data(disp));
  if (self == nullptr || self->flush_done_sem_ == nullptr)
    return;
  if (xSemaphoreTake(static_cast<SemaphoreHandle_t>(self->flush_done_sem_), portMAX_DELAY) == pdTRUE)
    lv_display_flush_ready(disp);
}
#endif

IdleTrigger::IdleTrigger(LvglComponent *parent, TemplatableValue<uint32_t> timeout) : timeout_(std::move(timeout)) {
  parent->add_on_idle_callback([this](uint32_t idle_time) {
    if (!this->is_idle_ && idle_time > this->timeout_.value()) {
      this->is_idle_ = true;
      this->trigger();
    } else if (this->is_idle_ && idle_time < this->timeout_.value()) {
      this->is_idle_ = false;
    }
  });
}

#ifdef USE_LVGL_TOUCHSCREEN
LVTouchListener::LVTouchListener(uint16_t long_press_time, uint16_t long_press_repeat_time, LvglComponent *parent) {
  this->set_parent(parent);
  this->drv_ = lv_indev_create();
  lv_indev_set_type(this->drv_, LV_INDEV_TYPE_POINTER);
  lv_indev_set_disp(this->drv_, parent->get_disp());
  lv_indev_set_long_press_time(this->drv_, long_press_time);
  // long press repeat time TBD
  lv_indev_set_user_data(this->drv_, this);
  lv_indev_set_read_cb(this->drv_, [](lv_indev_t *d, lv_indev_data_t *data) {
    auto *l = static_cast<LVTouchListener *>(lv_indev_get_user_data(d));
    if (l->touch_pressed_) {
      data->point.x = l->touch_point_.x;
      data->point.y = l->touch_point_.y;
      // Rotate the touch into LVGL's logical space when using `lvgl: rotation:`
      // (no-op for display: rotation or rotation 0).
      l->get_parent()->rotate_touch_point(data->point.x, data->point.y);
      data->state = LV_INDEV_STATE_PRESSED;
    } else {
      data->state = LV_INDEV_STATE_RELEASED;
    }
  });
}

void LVTouchListener::update(const touchscreen::TouchPoints_t &tpoints) {
  this->touch_pressed_ = !this->parent_->is_paused() && !tpoints.empty();
  if (this->touch_pressed_)
    this->touch_point_ = tpoints[0];
}
#endif  // USE_LVGL_TOUCHSCREEN

#ifdef USE_LVGL_METER

int16_t lv_get_needle_angle_for_value(lv_obj_t *obj, int value) {
  auto *scale = lv_obj_get_parent(obj);
  auto min_value = lv_scale_get_range_min_value(scale);
  return ((value - min_value) * lv_scale_get_angle_range(scale) / (lv_scale_get_range_max_value(scale) - min_value) +
          lv_scale_get_rotation((scale))) %
         360;
}

void IndicatorLine::set_obj(lv_obj_t *lv_obj) {
  LvCompound::set_obj(lv_obj);
  lv_line_set_points(lv_obj, this->points_, 2);
  lv_obj_add_event_cb(
      lv_obj_get_parent(obj),
      [](lv_event_t *e) {
        auto *indicator = static_cast<IndicatorLine *>(lv_event_get_user_data(e));
        indicator->update_length_();
        ESP_LOGD(TAG, "Updated length, value = %d", indicator->angle_);
      },
      LV_EVENT_SIZE_CHANGED, this);
}

void IndicatorLine::set_value(int value) {
  auto angle = lv_get_needle_angle_for_value(this->obj, value);
  if (angle != this->angle_) {
    this->angle_ = angle;
    this->update_length_();
  }
}

void IndicatorLine::update_length_() {
  uint32_t actual_needle_length;
  auto radius = lv_obj_get_width(lv_obj_get_parent(this->obj)) / 2;
  auto length = lv_obj_get_style_length(this->obj, LV_PART_MAIN);
  auto radial_offset = lv_obj_get_style_radial_offset(this->obj, LV_PART_MAIN);
  if (LV_COORD_IS_PCT(radial_offset)) {
    radial_offset = radius * LV_COORD_GET_PCT(radial_offset) / 100;
  }
  if (LV_COORD_IS_PCT(length)) {
    actual_needle_length = radius * LV_COORD_GET_PCT(length) / 100;
  } else if (length < 0) {
    actual_needle_length = radius + length;
  } else {
    actual_needle_length = length;
  }
  auto x = lv_trigo_cos(this->angle_) / 32768.0f;
  auto y = lv_trigo_sin(this->angle_) / 32768.0f;
  this->points_[0].x = radius + radial_offset * x;
  this->points_[0].y = radius + radial_offset * y;
  this->points_[1].x = x * actual_needle_length + radius;
  this->points_[1].y = y * actual_needle_length + radius;
  lv_obj_refresh_self_size(this->obj);
  lv_obj_invalidate(this->obj);
}
#endif

#ifdef USE_LVGL_KEY_LISTENER
LVEncoderListener::LVEncoderListener(lv_indev_type_t type, uint16_t long_press_time, uint16_t long_press_repeat_time) {
  this->drv_ = lv_indev_create();
  lv_indev_set_type(this->drv_, type);
  lv_indev_set_long_press_time(this->drv_, long_press_time);
  lv_indev_set_long_press_repeat_time(this->drv_, long_press_repeat_time);
  lv_indev_set_user_data(this->drv_, this);
  lv_indev_set_read_cb(this->drv_, [](lv_indev_t *d, lv_indev_data_t *data) {
    auto *l = static_cast<LVEncoderListener *>(lv_indev_get_user_data(d));
    data->state = l->pressed_ ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    data->key = l->key_;
    // LVGL 9.5: Apply rotary sensitivity multiplier
    auto raw_diff = (int16_t) (l->count_ - l->last_count_);
    data->enc_diff = (int16_t) (raw_diff * l->sensitivity_);
    l->last_count_ = l->count_;
    data->continue_reading = false;
  });
}
#endif  // USE_LVGL_KEY_LISTENER

#if defined(USE_LVGL_DROPDOWN) || defined(LV_USE_ROLLER)
std::string LvSelectable::get_selected_text() {
  auto selected = this->get_selected_index();
  if (selected >= this->options_.size())
    return "";
  return this->options_[selected];
}

static std::string join_string(std::vector<std::string> options) {
  return std::accumulate(
      options.begin(), options.end(), std::string(),
      [](const std::string &a, const std::string &b) -> std::string { return a + (!a.empty() ? "\n" : "") + b; });
}

void LvSelectable::set_selected_text(const std::string &text, lv_anim_enable_t anim) {
  auto index = std::find(this->options_.begin(), this->options_.end(), text);
  if (index != this->options_.end()) {
    this->set_selected_index(index - this->options_.begin(), anim);
    lv_obj_send_event(this->obj, lv_api_event, nullptr);
  }
}

void LvSelectable::set_options(std::vector<std::string> options) {
  auto index = this->get_selected_index();
  if (index >= options.size())
    index = options.size() - 1;
  this->options_ = std::move(options);
  this->set_option_string(join_string(this->options_).c_str());
  lv_obj_send_event(this->obj, LV_EVENT_REFRESH, nullptr);
  this->set_selected_index(index, LV_ANIM_OFF);
}
#endif  // USE_LVGL_DROPDOWN || LV_USE_ROLLER

#ifdef USE_LVGL_BUTTONMATRIX
void LvButtonMatrixType::set_obj(lv_obj_t *lv_obj) {
  LvCompound::set_obj(lv_obj);
  lv_obj_add_event_cb(
      lv_obj,
      [](lv_event_t *event) {
        auto *self = static_cast<LvButtonMatrixType *>(lv_event_get_user_data(event));
        if (self->key_callback_.size() == 0)
          return;
        auto key_idx = lv_buttonmatrix_get_selected_button(self->obj);
        if (key_idx == LV_BUTTONMATRIX_BUTTON_NONE)
          return;
        if (self->key_map_.count(key_idx) != 0) {
          self->send_key_(self->key_map_[key_idx]);
          return;
        }
        const auto *str = lv_buttonmatrix_get_button_text(self->obj, key_idx);
        auto len = strlen(str);
        while (len--)
          self->send_key_(*str++);
      },
      LV_EVENT_PRESSED, this);
}
#endif  // USE_LVGL_BUTTONMATRIX

#ifdef USE_LVGL_KEYBOARD
static const char *const KB_SPECIAL_KEYS[] = {
    "abc", "ABC", "1#",
    // maybe add other special keys here
};

void LvKeyboardType::set_obj(lv_obj_t *lv_obj) {
  LvCompound::set_obj(lv_obj);
  lv_obj_add_event_cb(
      lv_obj,
      [](lv_event_t *event) {
        auto *self = static_cast<LvKeyboardType *>(lv_event_get_user_data(event));
        if (self->key_callback_.size() == 0)
          return;

        auto key_idx = lv_buttonmatrix_get_selected_button(self->obj);
        if (key_idx == LV_BUTTONMATRIX_BUTTON_NONE)
          return;
        const char *txt = lv_buttonmatrix_get_button_text(self->obj, key_idx);
        if (txt == nullptr)
          return;
        for (const auto *kb_special_key : KB_SPECIAL_KEYS) {
          if (strcmp(txt, kb_special_key) == 0)
            return;
        }
        while (*txt != 0)
          self->send_key_(*txt++);
      },
      LV_EVENT_PRESSED, this);
}
#endif  // USE_LVGL_KEYBOARD

void LvglComponent::draw_end_() {
  this->perf_frames_++;  // one rendered frame (LV_EVENT_REFR_READY)
  if (this->draw_end_callback_ != nullptr)
    this->draw_end_callback_->trigger();
  if (this->update_when_display_idle_) {
    for (auto *disp : this->displays_)
      disp->update();
  }
}

bool LvglComponent::is_paused() const {
  if (this->paused_)
    return true;
  if (this->update_when_display_idle_) {
    for (auto *disp : this->displays_) {
      if (!disp->is_idle())
        return true;
    }
  }
  return false;
}

void LvglComponent::write_random_() {
  int iterations = 6 - lv_display_get_inactive_time(this->disp_) / 60000;
  if (iterations <= 0)
    iterations = 1;
  while (iterations-- != 0) {
    auto col = random_uint32() % this->width_;
    col = col / this->draw_rounding * this->draw_rounding;
    auto row = random_uint32() % this->height_;
    row = row / this->draw_rounding * this->draw_rounding;
    auto raw_size = (random_uint32() % 32) / this->draw_rounding * this->draw_rounding;
    if (raw_size == 0)
      continue;
    auto size = raw_size - 1;
    lv_area_t area;
    area.x1 = col;
    area.y1 = row;
    area.x2 = col + size;
    area.y2 = row + size;
    if (area.x2 >= this->width_)
      area.x2 = this->width_ - 1;
    if (area.y2 >= this->height_)
      area.y2 = this->height_ - 1;

    size_t line_len = lv_area_get_width(&area) * lv_area_get_height(&area) / 2;
    for (size_t i = 0; i != line_len; i++) {
      ((uint32_t *) (this->draw_buf_))[i] = random_uint32();
    }
    this->draw_buffer_(&area, (lv_color_data *) this->draw_buf_);
  }
}

/**
 * @class LvglComponent
 * @brief Component for rendering LVGL.
 *
 * This component renders LVGL widgets on a display. Some initialisation must be done in the constructor
 * since LVGL needs to be initialised before any widgets can be created.
 *
 * @param displays a list of displays to render onto. All displays must have the same
 *                 resolution.
 * @param buffer_frac the fraction of the display resolution to use for the LVGL
 *                    draw buffer. A higher value will make animations smoother but
 *                    also increase memory usage.
 * @param full_refresh if true, the display will be fully refreshed on every frame.
 *                     If false, only changed areas will be updated.
 * @param draw_rounding the rounding to use when drawing. A value of 1 will draw
 *                      without any rounding, a value of 2 will round to the nearest
 *                      multiple of 2, and so on.
 * @param resume_on_input if true, this component will resume rendering when the user
 *                         presses a key or clicks on the screen.
 */
LvglComponent::LvglComponent(std::vector<display::Display *> displays, float buffer_frac, bool full_refresh,
                             int draw_rounding, bool resume_on_input, bool update_when_display_idle)
    : draw_rounding(draw_rounding),
      displays_(std::move(displays)),
      buffer_frac_(buffer_frac),
      full_refresh_(full_refresh),
      resume_on_input_(resume_on_input),
      update_when_display_idle_(update_when_display_idle) {
  this->disp_ = lv_display_create(240, 240);
}

void LvglComponent::setup() {
  auto *display = this->displays_[0];
  auto rounding = this->draw_rounding;
  // cater for displays with dimensions that don't divide by the required rounding
  this->width_ = display->get_width();
  this->height_ = display->get_height();
  // When rotation is set via `lvgl: rotation:` (not the display: component), the
  // display itself is NOT rotated, so get_width()/get_height() return the
  // *physical* panel dimensions. For a 90/270 rotation LVGL must render at the
  // swapped logical size, so that after we rotate the rendered frame it matches
  // the physical panel. (For `display: rotation:`, get_width()/get_height()
  // already reflect the swap, so we must not swap again.)
  if (this->rotation_configured_ && (this->rotation == display::DISPLAY_ROTATION_90_DEGREES ||
                                     this->rotation == display::DISPLAY_ROTATION_270_DEGREES)) {
    std::swap(this->width_, this->height_);
  }
  auto width = (this->width_ + rounding - 1) / rounding * rounding;
  auto height = (this->height_ + rounding - 1) / rounding * rounding;
  auto frac = this->buffer_frac_;
  if (frac == 0)
    frac = 1;
  // LV_COLOR_FORMAT_RGB888 uses 3 bytes/pixel even when LV_COLOR_DEPTH=32
#if LV_COLOR_DEPTH == 32
  constexpr size_t BYTES_PER_PIXEL = 3;  // RGB888
#else
  constexpr size_t BYTES_PER_PIXEL = LV_COLOR_DEPTH / 8;
#endif
  auto buf_bytes = width * height / frac * BYTES_PER_PIXEL;
  // Align buffer size to the data cache line (128 B if
  // CONFIG_CACHE_L2_CACHE_LINE_128B=y, else 64 B is enough). 128 satisfies
  // both — esp_cache_msync() + PPA require both address AND size to be
  // cache-line aligned. Without this, PPA operations fail on PSRAM buffers
  // ('out.buffer addr or out.buffer_size not aligned to cache line size').
  constexpr size_t BUF_SIZE_ALIGN = 128;
  buf_bytes = (buf_bytes + BUF_SIZE_ALIGN - 1) & ~(BUF_SIZE_ALIGN - 1);

  // --- Internal-SRAM rotation pipeline (opt-in) ---------------------------
  // On PSRAM-bandwidth-limited ESP32-P4 silicon (e.g. rev v1.0) the bottleneck
  // is not the PPA or the CPU but the PSRAM bus. A rotated frame whose draw and
  // rotate buffers live in PSRAM costs ~5 PSRAM round-trips (render write, PPA
  // read+write, draw_pixels_at read+write), which starves the DSI scan-out and
  // the camera. If we instead shrink those buffers so all three (draw_buf_,
  // draw_buf2_, rotate_buf_) fit in internal SRAM — a separate, ~10x faster bus
  // — the whole render+rotate pipeline stays on-chip and only the final panel
  // push touches PSRAM (1 pass instead of 5). The trade is more, smaller
  // partial flushes, which is a net win precisely when PSRAM is the bottleneck.
  // Counter-intuitive vs. the usual "bigger buffer = faster" rule, which only
  // holds when PSRAM bandwidth is abundant.
#if defined(USE_LVGL_PPA) && defined(USE_ESP32)
  {
    display::DisplayRotation eff_rot =
        this->rotation_configured_ ? this->rotation : display->get_rotation();
    if (this->rotation_internal_sram_ && this->full_refresh_) {
      ESP_LOGW(TAG,
               "rotation_buffers_internal is ignored with full_refresh: true (full refresh needs a "
               "full-screen buffer). Use partial refresh to keep the rotation pipeline in SRAM.");
    }
    if (this->rotation_internal_sram_ && !this->full_refresh_ &&
        eff_rot != display::DISPLAY_ROTATION_0_DEGREES) {
      size_t free_int = heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
      // Only draw_buf_ + rotate_buf_ are allocated from this budget: draw_buf2_
      // is disabled (ENABLE_ASYNC_ROTATION=false). Splitting by 2 (not 3) makes
      // each buffer ~50% larger -> ~1/3 fewer partial flushes, which speeds up
      // full-screen redraws (page navigation) without extra memory. Keep ~30%
      // internal-SRAM headroom for the rest of the app (DMA descriptors, stacks).
      size_t per_buf = (free_int * 7 / 10) / 2;
      per_buf &= ~static_cast<size_t>(BUF_SIZE_ALIGN - 1);
      // Need at least a few rounded rows for the rotation math to be useful.
      size_t min_buf = static_cast<size_t>(width) * this->draw_rounding * BYTES_PER_PIXEL;
      min_buf = (min_buf + BUF_SIZE_ALIGN - 1) & ~(BUF_SIZE_ALIGN - 1);
      if (per_buf >= min_buf && per_buf < buf_bytes) {
        size_t full_bytes = static_cast<size_t>(width) * height * BYTES_PER_PIXEL;
        buf_bytes = per_buf;
        frac = (full_bytes + buf_bytes - 1) / buf_bytes;
        ESP_LOGI(TAG,
                 "Rotation internal-SRAM pipeline ON: buffers shrunk to %zu B (~1/%zu screen), "
                 "free internal SRAM=%zu B. Render+rotate stay on-chip; only the panel push hits PSRAM.",
                 buf_bytes, static_cast<size_t>(frac), free_int);
      } else {
        ESP_LOGW(TAG,
                 "Rotation internal-SRAM pipeline requested but not applied: internal-SRAM budget "
                 "%zu B/buf can't beat the current %zu B buffer (free internal=%zu B). Falling back "
                 "to normal allocation (buffers may land in PSRAM).",
                 per_buf, buf_bytes, free_int);
      }
    }
  }
#endif

  void *buffer = nullptr;

  // Helper lambda to allocate an aligned DMA-capable buffer.
  // psram_first: when true, allocate in PSRAM before internal DMA SRAM.
  //   The PPA display-rotation path DMAs these buffers and runs esp_cache_msync
  //   on them every frame. Internal DMA SRAM is the SAME pool the i2s audio and
  //   the wifi stack use for their DMA; placing the PPA rotation buffers there
  //   interleaves PPA DMA + cache maintenance with the audio/wifi DMA pool and
  //   corrupts the heap under concurrent load (random TLSF asserts / access
  //   faults). Putting them in PSRAM isolates the two pools. PPA works fine on
  //   PSRAM (the image path and large displays already rely on it).
  auto alloc_draw_buf = [](size_t sz, bool psram_first) -> void * {
#if defined(USE_LVGL_PPA) && defined(USE_ESP32)
    // Round size up to 128-byte cache line so PPA buffer_size checks pass
    // on both 64 B and 128 B cache-line sdkconfigs.
    size_t aligned_sz = (sz + 127) & ~size_t{127};
    if (!psram_first) {
      void *p = heap_caps_aligned_alloc(128, aligned_sz, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
      if (p != nullptr)
        return p;
    }
    // 128-byte aligned for the 128 B cache line.
    void *p = heap_caps_aligned_alloc(128, aligned_sz, MALLOC_CAP_SPIRAM);
    if (p != nullptr)
      return p;
    if (psram_first) {
      // PSRAM exhausted → fall back to internal DMA SRAM rather than fail.
      p = heap_caps_aligned_alloc(128, aligned_sz, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
      if (p != nullptr)
        return p;
    }
#else
    (void) psram_first;
#endif
    return lv_malloc_core(sz);
  };

  // Isolate the PPA rotation buffers in PSRAM whenever a rotation is active
  // (either `lvgl: rotation:` or inherited from `display:`) -- UNLESS the
  // internal-SRAM rotation pipeline was requested, in which case we WANT the
  // (already shrunk) buffers in internal SRAM, so allocate internal-first.
  auto eff_rotation2 = this->rotation_configured_ ? this->rotation : display->get_rotation();
  bool ppa_psram = (eff_rotation2 != display::DISPLAY_ROTATION_0_DEGREES) && !this->rotation_internal_sram_;

  buffer = alloc_draw_buf(buf_bytes, ppa_psram);
  // if specific buffer size not set and can't get 100%, try for a smaller one
  if (buffer == nullptr && this->buffer_frac_ == 0) {
    frac = MIN_BUFFER_FRAC;
    buf_bytes /= MIN_BUFFER_FRAC;
    buffer = alloc_draw_buf(buf_bytes, ppa_psram);
  }
  this->buffer_frac_ = frac;
  if (buffer == nullptr) {
    this->status_set_error(LOG_STR("Memory allocation failure"));
    this->mark_failed();
    return;
  }
  this->draw_buf_ = static_cast<uint8_t *>(buffer);
  lv_display_set_resolution(this->disp_, this->width_, this->height_);
#if LV_COLOR_DEPTH == 32
  // RGB888: 3 bytes per pixel, fully supported by PPA as destination
  lv_display_set_color_format(this->disp_, LV_COLOR_FORMAT_RGB888);
#else
  lv_display_set_color_format(this->disp_, LV_COLOR_FORMAT_RGB565);
#endif
  // CRITICAL: Set user_data BEFORE flush_cb, as flush_cb uses user_data
  lv_display_set_user_data(this->disp_, this);
  lv_display_set_flush_cb(this->disp_, static_flush_cb);
  lv_display_add_event_cb(this->disp_, rounder_cb, LV_EVENT_INVALIDATE_AREA, this);
  // Store buf_bytes - lv_display_set_buffers() is called at the END of setup()
  // to avoid triggering rendering before all callbacks and pages are configured.
  this->buf_bytes_ = buf_bytes;
  // If the user set `rotation:` on the lvgl: component, that value already lives
  // in this->rotation (via set_lvgl_rotation). Otherwise inherit it from the
  // display: component.
  if (!this->rotation_configured_)
    this->rotation = display->get_rotation();
  if (this->rotation != display::DISPLAY_ROTATION_0_DEGREES) {
    this->rotate_buf_ = static_cast<lv_color_t *>(alloc_draw_buf(buf_bytes, ppa_psram));
    if (this->rotate_buf_ == nullptr) {
      this->status_set_error(LOG_STR("Memory allocation failure"));
      this->mark_failed();
      return;
    }
#ifdef USE_LVGL_PPA
    if (s_display_srm_client != nullptr) {
      ESP_LOGI(TAG, "Display rotation will use PPA SRM hardware acceleration");
    }
    // Pipeline the rotation+push: second draw buffer (double buffering) + a
    // dedicated flush task. LVGL renders frame N+1 into the other buffer while
    // the task rotates+pushes frame N. The task does NO LVGL calls; the main
    // loop / flush_wait_cb_ call lv_display_flush_ready, so there is no thread
    // race. Falls back to the synchronous single-buffer path on any failure.
    //
    // TEMPORARILY DISABLED: the async flush task (pinned to core 1) crashed with
    // a garbage source pointer inside esp_cache_msync (Core 1 Load access fault).
    // Force the synchronous single-buffer path to isolate the async handoff.
    // If synchronous rotation is stable, the bug is in the double-buffer /
    // flush-task pipeline, not in the PPA rotate itself.
    constexpr bool ENABLE_ASYNC_ROTATION = false;
    this->draw_buf2_ =
        ENABLE_ASYNC_ROTATION ? static_cast<uint8_t *>(alloc_draw_buf(buf_bytes, ppa_psram)) : nullptr;
    if (this->draw_buf2_ != nullptr) {
      this->flush_queue_ = xQueueCreate(2, sizeof(FlushJob));
      this->flush_done_sem_ = xSemaphoreCreateCounting(8, 0);
      if (this->flush_queue_ != nullptr && this->flush_done_sem_ != nullptr) {
        BaseType_t ok = xTaskCreatePinnedToCore(&LvglComponent::flush_task_entry_, "lvgl_flush", 8192, this,
                                                 5, reinterpret_cast<TaskHandle_t *>(&this->flush_task_), 1);
        if (ok == pdPASS) {
          this->async_flush_ = true;
          lv_display_set_flush_wait_cb(this->disp_, &LvglComponent::flush_wait_cb_);
          ESP_LOGI(TAG, "Display rotation: pipelined flush enabled (double buffer + flush task)");
        }
      }
      if (!this->async_flush_) {
        if (this->flush_queue_ != nullptr) {
          vQueueDelete(static_cast<QueueHandle_t>(this->flush_queue_));
          this->flush_queue_ = nullptr;
        }
        if (this->flush_done_sem_ != nullptr) {
          vSemaphoreDelete(static_cast<SemaphoreHandle_t>(this->flush_done_sem_));
          this->flush_done_sem_ = nullptr;
        }
        heap_caps_free(this->draw_buf2_);
        this->draw_buf2_ = nullptr;
        ESP_LOGW(TAG, "Pipelined flush unavailable -> synchronous rotation (lower FPS)");
      }
    }
    // Report where the rotation pipeline buffers actually landed. This is the
    // proof of whether the internal-SRAM optimization took effect: if any of
    // these say PSRAM, that buffer's traffic still competes with the DSI/camera.
    auto mem_of = [](const void *p) -> const char * {
      return p == nullptr ? "none" : (esp_ptr_internal(p) ? "internal SRAM" : "PSRAM");
    };
    ESP_LOGI(TAG, "Rotation buffers: draw_buf=%s, draw_buf2=%s, rotate_buf=%s (%zu B each)",
             mem_of(this->draw_buf_), mem_of(this->draw_buf2_), mem_of(this->rotate_buf_), buf_bytes);
#endif
  }
  if (this->draw_start_callback_ != nullptr) {
    lv_display_add_event_cb(this->disp_, render_start_cb, LV_EVENT_RENDER_START, this);
  }
  bool want_refr_ready = this->draw_end_callback_ != nullptr || this->update_when_display_idle_;
#ifdef LV_USE_PERF_MONITOR
  // draw_end_() also counts rendered frames for the logged FPS figure, so the
  // handler has to run even when nothing else asked for it.
  want_refr_ready = true;
#endif
  if (want_refr_ready) {
    lv_display_add_event_cb(this->disp_, render_end_cb, LV_EVENT_REFR_READY, this);
  }
#if LV_USE_LOG
  lv_log_register_print_cb([](lv_log_level_t level, const char *buf) {
    auto next = strchr(buf, ')');
    if (next != nullptr)
      buf = next + 1;
    while (isspace(*buf))
      buf++;
    if (level >= sizeof(LOG_LEVEL_MAP) / sizeof(LOG_LEVEL_MAP[0]))
      level = sizeof(LOG_LEVEL_MAP) / sizeof(LOG_LEVEL_MAP[0]) - 1;
    esp_log_printf_(LOG_LEVEL_MAP[level], TAG, 0, "%.*s", (int) strlen(buf) - 1, buf);
  });
#endif
  // Rotation will be handled by our drawing function, so reset the display rotation.
  for (auto *disp : this->displays_)
    disp->set_rotation(display::DISPLAY_ROTATION_0_DEGREES);
  this->show_page(0, LV_SCR_LOAD_ANIM_NONE, 0);
  lv_display_trigger_activity(this->disp_);

  // CRITICAL: Configure buffers at the VERY END of setup()
  // This avoids deadlock while ensuring buffers are ready before any callbacks execute
  // When pipelined flush is active, pass the second buffer so LVGL double-buffers
  // (render next frame while the flush task rotates+pushes the current one).
  void *buf2 = nullptr;
#ifdef USE_LVGL_PPA
  buf2 = this->draw_buf2_;
#endif
  lv_display_set_buffers(this->disp_, this->draw_buf_, buf2, this->buf_bytes_,
                         this->full_refresh_ ? LV_DISPLAY_RENDER_MODE_FULL : LV_DISPLAY_RENDER_MODE_PARTIAL);
  this->buffers_configured_ = true;

#ifdef USE_LVGL_PPA
  // Espressif esp-iot-solution PPA SW blend handler — accelerates all
  // RGB565 SW blend paths (text, gradients post-rasterize, partial blends).
  // Complements the higher-level PPA draw unit in lv_draw_ppa.c.
  lvgl_port_ppa_v9_init(this->disp_);
#endif

#ifdef USE_LVGL_FPS_BENCHMARK
  // Espressif esp_lvgl_adapter FPS sampler — prints a P10/25/50/75/90
  // report after ~200 samples (or sustained low-FPS detection).
  ESP_LOGI(TAG, "FPS benchmark: calling attach() for disp=%p", this->disp_);
  lvgl_fps_attach_v2(this->disp_);
  ESP_LOGI(TAG, "FPS benchmark: attach() returned");
#else
  ESP_LOGI(TAG, "FPS benchmark: not compiled in (USE_LVGL_FPS_BENCHMARK undefined)");
#endif
}

void LvglComponent::update() {
  // update indicators
  if (this->is_paused()) {
    return;
  }
  this->idle_callbacks_.call(lv_display_get_inactive_time(this->disp_));
}

void LvglComponent::loop() {
  if (!this->buffers_configured_)
    return;  // setup() not complete or failed, skip rendering

  if (!this->loop_started_) {
    this->loop_started_ = true;
    ESP_LOGD(TAG, "LVGL loop started - system is now fully ready");
  }

#ifdef USE_LVGL_PPA
  // Report which branch the PPA image blend took, from the main task. The draw
  // thread only latches the value: calling the logger from there routes through
  // the LVGL log callback into the ESPHome logger and faults the first render.
  // Logged on change, so an opacity sweep shows each branch exactly once.
  {
    // A frame can draw several images through different branches (e.g. the same
    // picture at a few opacities), so report every mode newly seen rather than
    // whichever one happened to be drawn last.
    uint8_t fresh = static_cast<uint8_t>(lv_ppa_img_seen_modes & ~this->ppa_img_reported_mode_);
    if (fresh != 0) {
      this->ppa_img_reported_mode_ |= fresh;
      static const char *const MODE_NAMES[] = {
          "none", "blit (opaque)", "alpha NO_CHANGE", "alpha SCALE", "alpha FIX_VALUE",
      };
      for (uint8_t m = 1; m < sizeof(MODE_NAMES) / sizeof(MODE_NAMES[0]); m++) {
        if (fresh & (1u << m))
          ESP_LOGI(TAG, "PPA image path: %s (src_cf=%u dest_cf=%u)", MODE_NAMES[m],
                   (unsigned) lv_ppa_img_last_src_cf, (unsigned) lv_ppa_img_last_dest_cf);
      }
    }
  }

  // Complete any async (pipelined) flushes the flush task finished. We call
  // lv_display_flush_ready here, on the main thread, so it never races with
  // lv_timer_handler — the flush task only does the rotate+push and signals.
  if (this->flush_done_sem_ != nullptr) {
    while (xSemaphoreTake(static_cast<SemaphoreHandle_t>(this->flush_done_sem_), 0) == pdTRUE)
      lv_display_flush_ready(this->disp_);
  }
#endif

  if (this->is_paused()) {
    if (this->paused_ && this->show_snow_)
      this->write_random_();
  } else {
    // Time the LVGL handler. flush_cb_ separately accumulates the DSI
    // DMA wait into perf_flush_us_; subtract it so the reported CPU%%
    // counts only real render work (matches lvgl_camera_display's
    // approach: cpu_time / frame_interval).
    uint64_t t0 = lvgl_now_us();
    lv_timer_handler();
    uint64_t t1 = lvgl_now_us();
    this->perf_busy_us_ += (t1 - t0);
    uint64_t now_us = t1;
    if (this->perf_window_start_us_ == 0)
      this->perf_window_start_us_ = now_us;
    uint64_t elapsed_us = now_us - this->perf_window_start_us_;
    if (elapsed_us >= 1000000) {
      uint64_t cpu_us = (this->perf_busy_us_ > this->perf_flush_us_)
                            ? (this->perf_busy_us_ - this->perf_flush_us_)
                            : 0;
      uint32_t cpu_pct = (uint32_t)((cpu_us * 100ULL) / elapsed_us);
      if (cpu_pct > 100) cpu_pct = 100;
      s_cpu_pct = cpu_pct;  // publish to __wrap_lv_timer_get_idle / sysmon overlay
#ifdef LV_USE_PERF_MONITOR
      // Same numbers as the on-screen overlay, but in the log so a test
      // run can be copied out instead of read off the panel. Only built
      // when perf_monitor: is enabled, so normal builds stay quiet.
      ESP_LOGI(TAG, "perf: page %u  FPS %u  CPU %u%%  alpha=%s srm=%s", (unsigned) this->current_page_,
               (unsigned) ((this->perf_frames_ * 1000000ULL) / elapsed_us), (unsigned) cpu_pct,
               lv_ppa_alpha_min_area == 0 ? "PPA" : "SW",
               lv_ppa_srm_min_area == 0 ? "PPA" : "SW");
      // Free heap alongside it: transformed images make LVGL allocate
      // intermediate layers, and a failed allocation is a likely way for a
      // heavy scale/rotate screen to fall over.
      // LVGL allocates through lv_malloc_core(), which lands in these heaps,
      // so the ESP figures cover the layer allocations too.
      ESP_LOGI(TAG, "  heap: int %u KB  psram %u KB",
               (unsigned) (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
               (unsigned) (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
      // Where that CPU went inside the PPA path: cache maintenance over the
      // draw buffer versus the blocking wait in the PPA calls themselves.
      if (lv_ppa_op_count > 0) {
        ESP_LOGI(TAG, "  ppa: %u ops  cache %u ms  op %u ms  (%u us/op)",
                 (unsigned) lv_ppa_op_count, (unsigned) (lv_ppa_us_cache / 1000),
                 (unsigned) (lv_ppa_us_op / 1000),
                 (unsigned) (lv_ppa_us_op / lv_ppa_op_count));
      }
      lv_ppa_us_cache = 0;
      lv_ppa_us_op = 0;
      lv_ppa_op_count = 0;
#endif
      this->perf_frames_ = 0;
      // Verbose-only log: enable via 'logs: lvgl: VERBOSE' in YAML if you
      // need the breakdown. Default DEBUG/INFO levels stay silent.
      ESP_LOGV(TAG, "perf: CPU %u%% (render %llu us, flush %llu us / wall %llu us)",
               (unsigned)cpu_pct,
               (unsigned long long)cpu_us,
               (unsigned long long)this->perf_flush_us_,
               (unsigned long long)elapsed_us);
      this->perf_busy_us_ = 0;
      this->perf_flush_us_ = 0;
      this->perf_window_start_us_ = now_us;
    }
  }
}

#ifdef USE_LVGL_ANIMIMG
void lv_animimg_stop(lv_obj_t *obj) {
  int32_t duration = lv_animimg_get_duration(obj);
  lv_animimg_set_duration(obj, 0);
  lv_animimg_start(obj);
  lv_animimg_set_duration(obj, duration);
}
#endif
void LvglComponent::static_flush_cb(lv_display_t *disp_drv, const lv_area_t *area, uint8_t *color_p) {
  reinterpret_cast<LvglComponent *>(lv_display_get_user_data(disp_drv))->flush_cb_(disp_drv, area, color_p);
}

#if LV_USE_SCALE
void lv_scale_draw_event_cb(lv_event_t *e, int32_t range_start, int32_t range_end, lv_color_t color_start,
                            lv_color_t color_end, bool local) {
  auto *scale = static_cast<lv_obj_t *>(lv_event_get_target(e));
  lv_draw_task_t *task = lv_event_get_draw_task(e);

  if (lv_draw_task_get_type(task) == LV_DRAW_TASK_TYPE_LINE) {
    auto *line_dsc = static_cast<lv_draw_line_dsc_t *>(lv_draw_task_get_draw_dsc(task));
    auto tick_idx = line_dsc->base.id1;

    // Convert tick index to scale value
    auto total_ticks = lv_scale_get_total_tick_count(scale);
    auto scale_min = lv_scale_get_range_min_value(scale);
    auto scale_max = lv_scale_get_range_max_value(scale);
    int32_t tick_value;
    if (total_ticks > 1) {
      tick_value = scale_min + (int32_t) tick_idx * (scale_max - scale_min) / (total_ticks - 1);
    } else {
      tick_value = scale_min;
    }

    if (tick_value >= range_start && tick_value <= range_end) {
      int32_t range;
      int32_t pos;
      if (local) {
        range = range_end - range_start;
        pos = tick_value - range_start;
      } else {
        range = scale_max - scale_min;
        pos = tick_value - scale_min;
      }
      if (range == 0)
        range = 1;
      auto ratio = (pos * 255) / range;
      line_dsc->color = lv_color_mix(color_end, color_start, ratio);
    }
  }
}

void lv_scale_tick_offset_event_cb(lv_event_t *e, uint16_t offset, uint16_t stride) {
  auto *scale = static_cast<lv_obj_t *>(lv_event_get_target(e));
  lv_draw_task_t *task = lv_event_get_draw_task(e);
  auto type = lv_draw_task_get_type(task);

  if (type == LV_DRAW_TASK_TYPE_LINE) {
    auto *line_dsc = static_cast<lv_draw_line_dsc_t *>(lv_draw_task_get_draw_dsc(task));
    auto tick_idx = line_dsc->base.id1;

    bool is_major = (tick_idx >= offset) && ((tick_idx - offset) % stride == 0);

    if (!is_major) {
      line_dsc->color = lv_obj_get_style_line_color(scale, LV_PART_ITEMS);
      line_dsc->width = lv_obj_get_style_line_width(scale, LV_PART_ITEMS);

      int32_t minor_len = lv_obj_get_style_length(scale, LV_PART_ITEMS);
      int32_t major_len = lv_obj_get_style_length(scale, LV_PART_INDICATOR);
      if (major_len > 0 && minor_len > 0 && minor_len != major_len) {
        auto dx = line_dsc->p1.x - line_dsc->p2.x;
        auto dy = line_dsc->p1.y - line_dsc->p2.y;
        line_dsc->p1.x = line_dsc->p2.x + dx * minor_len / major_len;
        line_dsc->p1.y = line_dsc->p2.y + dy * minor_len / major_len;
      }
    }
  } else if (type == LV_DRAW_TASK_TYPE_LABEL) {
    auto *label_dsc = static_cast<lv_draw_label_dsc_t *>(lv_draw_task_get_draw_dsc(task));
    auto tick_idx = label_dsc->base.id1;

    bool is_major = (tick_idx >= offset) && ((tick_idx - offset) % stride == 0);

    if (!is_major) {
      label_dsc->opa = LV_OPA_TRANSP;
    }
  }
}
#endif  // LV_USE_SCALE

static void lv_container_constructor(const lv_obj_class_t *class_p, lv_obj_t *obj) {
  LV_TRACE_OBJ_CREATE("begin");
  LV_UNUSED(class_p);
}

// Container class. Name is based on LVGL naming convention but upper case to keep ESPHome clang-tidy happy
const lv_obj_class_t LV_CONTAINER_CLASS = {
    .base_class = &lv_obj_class,
    .constructor_cb = lv_container_constructor,
    .name = "lv_container",
};

lv_obj_t *lv_container_create(lv_obj_t *parent) {
  lv_obj_t *obj = lv_obj_class_create_obj(&LV_CONTAINER_CLASS, parent);
  lv_obj_class_init_obj(obj);
  return obj;
}

}  // namespace esphome::lvgl

lv_result_t lv_mem_test_core() { return LV_RESULT_OK; }

void lv_mem_init() {}

void lv_mem_deinit() {}

#if defined(USE_HOST) || defined(USE_RP2040) || defined(USE_ESP8266)
// Memory alignment for draw buffers on non-ESP32 platforms.
// We use 64-byte alignment for optimal performance even though LV_DRAW_BUF_ALIGN
// is set to 4 (to avoid warnings from LVGL's internal stack/static buffers).
// Standard malloc() only guarantees 8-16 byte alignment, so we implement
// our own aligned allocation.
static constexpr size_t LVGL_ALIGNMENT = 64;

// Store original pointer before aligned address for proper freeing
void *lv_malloc_core(size_t size) {
  if (size == 0)
    return nullptr;

  // Allocate extra space for alignment and to store original pointer
  size_t total_size = size + LVGL_ALIGNMENT + sizeof(void *);
  void *raw = malloc(total_size);  // NOLINT
  if (raw == nullptr) {
    ESP_LOGE(esphome::lvgl::TAG, "Failed to allocate %zu bytes", size);
    return nullptr;
  }

  // Calculate aligned pointer (leaving space for original pointer storage)
  uintptr_t raw_addr = reinterpret_cast<uintptr_t>(raw);
  uintptr_t aligned_addr = (raw_addr + sizeof(void *) + LVGL_ALIGNMENT - 1) & ~(LVGL_ALIGNMENT - 1);
  void *aligned = reinterpret_cast<void *>(aligned_addr);

  // Store original pointer just before aligned address
  reinterpret_cast<void **>(aligned)[-1] = raw;

  return aligned;
}

void lv_free_core(void *ptr) {
  if (ptr == nullptr)
    return;
  // Retrieve and free the original pointer
  void *raw = reinterpret_cast<void **>(ptr)[-1];
  free(raw);  // NOLINT
}

void *lv_realloc_core(void *ptr, size_t size) {
  if (ptr == nullptr)
    return lv_malloc_core(size);
  if (size == 0) {
    lv_free_core(ptr);
    return nullptr;
  }

  // Allocate new aligned buffer and copy data
  void *new_ptr = lv_malloc_core(size);
  if (new_ptr == nullptr)
    return nullptr;

  // We don't know the old size exactly, so copy min(new_size, old_usable_size).
  // On most platforms, malloc_usable_size() returns the actual allocated size.
  // Fall back to new size if unavailable (safe: reads at most what was allocated).
#if defined(__GLIBC__) || defined(__ANDROID__)
  size_t old_size = malloc_usable_size(reinterpret_cast<void **>(ptr)[-1]);
  // Subtract alignment overhead to get usable size from aligned pointer
  size_t overhead = reinterpret_cast<uintptr_t>(ptr) - reinterpret_cast<uintptr_t>(reinterpret_cast<void **>(ptr)[-1]);
  old_size = (old_size > overhead) ? old_size - overhead : 0;
#else
  size_t old_size = size;  // conservative fallback: may read less than available
#endif
  memcpy(new_ptr, ptr, (size < old_size) ? size : old_size);
  lv_free_core(ptr);

  return new_ptr;
}

void lv_mem_monitor_core(lv_mem_monitor_t *mon_p) { memset(mon_p, 0, sizeof(lv_mem_monitor_t)); }

#endif
#ifdef USE_ESP32
static unsigned cap_bits = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;  // NOLINT

void lv_mem_monitor_core(lv_mem_monitor_t *mon_p) {
  multi_heap_info_t heap_info;
  heap_caps_get_info(&heap_info, cap_bits);
  mon_p->total_size = heap_info.total_allocated_bytes + heap_info.total_free_bytes;
  mon_p->free_size = heap_info.total_free_bytes;
  mon_p->max_used = heap_info.total_allocated_bytes;
  mon_p->free_biggest_size = heap_info.largest_free_block;
  mon_p->used_cnt = heap_info.allocated_blocks;
  mon_p->free_cnt = heap_info.free_blocks;
  mon_p->used_pct = heap_info.allocated_blocks * 100 / (heap_info.allocated_blocks + heap_info.free_blocks);
  mon_p->frag_pct = 0;
}

void *lv_malloc_core(size_t size) {
  void *ptr;
  // Use 64-byte alignment for optimal ESP32 PSRAM/cache performance.
  // Note: LV_DRAW_BUF_ALIGN is set to 4 to avoid LVGL warnings from
  // internal stack/static buffers, but heap allocations use 64-byte alignment.
  constexpr size_t LVGL_ALIGNMENT = 64;

  // BUGFIX: Don't modify global cap_bits - use local variable
  unsigned caps = cap_bits;

  // Try PSRAM first
  ptr = heap_caps_aligned_alloc(LVGL_ALIGNMENT, size, caps);
  if (ptr == nullptr) {
    // Fallback to internal RAM if PSRAM allocation fails
    caps = MALLOC_CAP_8BIT;
    ptr = heap_caps_aligned_alloc(LVGL_ALIGNMENT, size, caps);
  }

  if (ptr == nullptr) {
    ESP_LOGE(esphome::lvgl::TAG, "Failed to allocate %zu bytes (64-byte aligned)", size);
    return nullptr;
  }

  // Log only very large buffers (>1MB) for debugging
  if (size > 1000000) {
    ESP_LOGI(esphome::lvgl::TAG, "Large buffer allocated: %zu bytes at %p", size, ptr);
  }

  return ptr;
}

void lv_free_core(void *ptr) {
  ESP_LOGV(esphome::lvgl::TAG, "free %p", ptr);
  if (ptr == nullptr)
    return;
  heap_caps_free(ptr);
}

void *lv_realloc_core(void *ptr, size_t size) {
  ESP_LOGV(esphome::lvgl::TAG, "realloc %p: %zu", ptr, size);

  if (ptr == nullptr)
    return lv_malloc_core(size);
  if (size == 0) {
    lv_free_core(ptr);
    return nullptr;
  }

  // CRITICAL: heap_caps_realloc does NOT preserve 64-byte alignment!
  // We must allocate a new aligned buffer and copy the data
  void *new_ptr = lv_malloc_core(size);
  if (new_ptr == nullptr)
    return nullptr;

  // Copy data to new buffer using heap_caps_get_allocated_size for safe bounds
  size_t old_size = heap_caps_get_allocated_size(ptr);
  memcpy(new_ptr, ptr, (size < old_size) ? size : old_size);
  lv_free_core(ptr);

  return new_ptr;
}
#endif

