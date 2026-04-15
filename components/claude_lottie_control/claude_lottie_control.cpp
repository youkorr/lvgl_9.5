#ifdef USE_ESP32

#include "claude_lottie_control.h"

#if LV_USE_LOTTIE

namespace esphome {
namespace claude_lottie_control {

static const char *const TAG = "claude_lottie";

void ClaudeLottieControl::setup() {
  if (this->target_obj_ == nullptr) {
    ESP_LOGE(TAG, "No target lv_lottie widget assigned");
    return;
  }
  ESP_LOGI(TAG, "Bound to lv_lottie widget @%p", this->target_obj_);
}

void ClaudeLottieControl::dump_config() {
  ESP_LOGCONFIG(TAG, "Claude Lottie Control:");
  ESP_LOGCONFIG(TAG, "  Target: %p", this->target_obj_);
  if (this->target_obj_ != nullptr) {
    auto *lottie = reinterpret_cast<lv_lottie_t *>(this->target_obj_);
    ESP_LOGCONFIG(TAG, "  tvg_anim: %p", lottie->tvg_anim);
  }
}

Tvg_Animation *ClaudeLottieControl::get_animation() {
  if (this->target_obj_ == nullptr) return nullptr;
  // lv_lottie_t layout: exposes tvg_canvas, tvg_anim (Tvg_Animation *), anim (LVGL).
  auto *lottie = reinterpret_cast<lv_lottie_t *>(this->target_obj_);
  return reinterpret_cast<Tvg_Animation *>(lottie->tvg_anim);
}

}  // namespace claude_lottie_control
}  // namespace esphome

// ============================================================================
// Shim: missing tvg_lottie_animation_{assign,gen_slot} C wrappers.
//
// LVGL 9.5's bundled thorvg_capi.cpp doesn't export these two wrappers, but
// the underlying C++ methods tvg::LottieAnimation::{assign, gen(slot)} ARE
// linkable in the compiled library (proven by tvg_lottie_animation_new()
// successfully calling LottieAnimation::gen() from the same TU).
//
// We forward-declare tvg::LottieAnimation with just the two public methods we
// need and call them via reinterpret_cast on the Tvg_Animation* handle. The
// Itanium C++ ABI resolves the mangled symbols at link time without requiring
// the full class layout.
//
// Defined in this TU (not a separate file) to guarantee it's part of the
// build — a standalone claude_lottie_shim.cpp sibling was not being picked up
// by ESPHome's external_components source-copy step.
// ============================================================================

namespace tvg {

enum class Result : uint8_t {
  Success = 0,
  InvalidArguments,
  InsufficientCondition,
  FailedAllocation,
  MemoryCorruption,
  NonSupport,
  Unknown,
};

class LottieAnimation {
 public:
  uint32_t gen(const char *slot) noexcept;
  Result assign(const char *layer, uint32_t ix, const char *var, float val);
};

}  // namespace tvg

extern "C" uint32_t tvg_lottie_animation_gen_slot(Tvg_Animation *animation, const char *slot) {
  if (animation == nullptr || slot == nullptr) return 0;
  return reinterpret_cast<tvg::LottieAnimation *>(animation)->gen(slot);
}

extern "C" Tvg_Result tvg_lottie_animation_assign(Tvg_Animation *animation, const char *layer,
                                                   uint32_t ix, const char *var, float val) {
  if (animation == nullptr || layer == nullptr || var == nullptr) {
    return TVG_RESULT_INVALID_ARGUMENT;
  }
  auto r = reinterpret_cast<tvg::LottieAnimation *>(animation)->assign(layer, ix, var, val);
  return static_cast<Tvg_Result>(r);
}

#endif  // LV_USE_LOTTIE
#endif  // USE_ESP32
