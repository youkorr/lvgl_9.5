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
// Stub: tvg_lottie_animation_{assign,gen_slot}
//
// LVGL 9.5's bundled thorvg is compiled WITHOUT
// THORVG_LOTTIE_EXPRESSIONS_SUPPORT. Both the C wrappers
// (tvg_lottie_animation_{assign,gen_slot}) AND the underlying C++ methods
// (tvg::LottieAnimation::{assign, gen(slot)}) are absent from the library
// — confirmed by a linker error on the C++ mangled symbol.
//
// Turn the YAML actions into no-ops so the build can link. The Lottie
// animation still plays its loop normally — it just can't be modulated at
// runtime via ExtendScript expressions.
//
// To enable real reactivity, either:
//   (a) rebuild LVGL's bundled thorvg with
//       -DTHORVG_LOTTIE_EXPRESSIONS_SUPPORT and pull in JerryScript, or
//   (b) drive reactivity from YAML via LVGL transforms
//       (lv_obj_set_style_transform_scale on the lottie widget, modulated
//        by the FFT sensor values).
// ============================================================================

static bool tvg_stub_warned = false;
static void tvg_stub_warn_once() {
  if (!tvg_stub_warned) {
    ESP_LOGW("claude_lottie",
             "tvg_lottie_animation_assign/gen_slot are STUBS — LVGL's bundled "
             "thorvg has no expressions support. Orb animation plays but will "
             "NOT react to FFT. See claude_lottie_control.cpp header comment.");
    tvg_stub_warned = true;
  }
}

extern "C" uint32_t tvg_lottie_animation_gen_slot(Tvg_Animation * /*animation*/,
                                                   const char * /*slot*/) {
  tvg_stub_warn_once();
  return 0;  // slot id 0 = "no slot"
}

extern "C" Tvg_Result tvg_lottie_animation_assign(Tvg_Animation * /*animation*/,
                                                   const char * /*layer*/, uint32_t /*ix*/,
                                                   const char * /*var*/, float /*val*/) {
  tvg_stub_warn_once();
  return TVG_RESULT_NOT_SUPPORTED;
}

#endif  // LV_USE_LOTTIE
#endif  // USE_ESP32
