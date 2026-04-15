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

#endif  // LV_USE_LOTTIE
#endif  // USE_ESP32
