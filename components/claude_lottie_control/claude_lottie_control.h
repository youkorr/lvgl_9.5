#pragma once

#ifdef USE_ESP32

#include "esphome/core/component.h"
#include "esphome/core/automation.h"
#include "esphome/core/log.h"

#include <lvgl.h>

#if LV_USE_LOTTIE

// Access lv_lottie_t layout (tvg_canvas, tvg_anim, anim fields).
// lv_lottie_private.h is a private LVGL header but is already used by
// components/lvgl/lottie_loader.h so we know it is available.
#include <src/widgets/lottie/lv_lottie_private.h>

#include <string>

// thorvg C API bindings — LVGL ships thorvg in-tree at src/libs/thorvg.
// This is the same path components/lvgl/svg_loader.h uses for the SVG widget.
// Provides tvg_lottie_animation_{gen,apply,del}_slot, tvg_lottie_animation_assign,
// tvg_lottie_animation_tween, tvg_lottie_animation_set_{marker,quality}.
#include <src/libs/thorvg/thorvg_capi.h>

namespace esphome {
namespace claude_lottie_control {

/// Wraps a single lv_lottie widget and exposes thorvg LottieAnimation
/// extension API through ESPHome automation actions.
///
/// The target widget is passed in at code-generation time via set_target(),
/// using the lv_obj_t* that ESPHome's LVGL component exposes for any widget
/// declared with an id. No name lookup or tree walking is required.
///
/// IMPORTANT: this component depends on a build-time patch (applied via
/// claude_lottie_build.py) that forces lv_lottie.c to use
/// tvg_lottie_animation_new() instead of tvg_animation_new(). Without that
/// patch, the tvg_anim field under the lv_lottie widget is a plain
/// tvg::Animation instance and calling the LottieAnimation extension
/// functions on it would be undefined behavior.
class ClaudeLottieControl : public Component {
 public:
  void set_target(lv_obj_t *obj) { this->target_obj_ = obj; }

  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_CONNECTION; }

  /// Return the underlying Tvg_Animation pointer of the target lv_lottie widget.
  /// This is the same handle that the C API expects for the
  /// tvg_lottie_animation_* functions. Can be called at any time — the field
  /// is refreshed by lv_lottie_set_src_*() after parsing.
  Tvg_Animation get_animation();

 protected:
  lv_obj_t *target_obj_{nullptr};
};

// --------------------------------------------------------------------------
// Actions — one per LottieAnimation method
// --------------------------------------------------------------------------

template <typename... Ts>
class AssignAction : public Action<Ts...> {
 public:
  explicit AssignAction(ClaudeLottieControl *parent) : parent_(parent) {}

  TEMPLATABLE_VALUE(std::string, layer)
  TEMPLATABLE_VALUE(uint32_t, ix)
  TEMPLATABLE_VALUE(std::string, var_name)
  TEMPLATABLE_VALUE(float, val)

  void play(Ts... x) override {
    auto anim = this->parent_->get_animation();
    if (anim == nullptr) return;
    const std::string layer = this->layer_.value(x...);
    const uint32_t ix = this->ix_.value(x...);
    const std::string var_name = this->var_name_.value(x...);
    const float val = this->val_.value(x...);
    tvg_lottie_animation_assign(anim, layer.c_str(), ix, var_name.c_str(), val);
  }

 protected:
  ClaudeLottieControl *parent_;
};

template <typename... Ts>
class GenSlotAction : public Action<Ts...> {
 public:
  explicit GenSlotAction(ClaudeLottieControl *parent) : parent_(parent) {}

  TEMPLATABLE_VALUE(std::string, slot_json)

  void play(Ts... x) override {
    auto anim = this->parent_->get_animation();
    if (anim == nullptr) return;
    const std::string slot = this->slot_json_.value(x...);
    uint32_t id = tvg_lottie_animation_gen_slot(anim, slot.c_str());
    ESP_LOGD("claude_lottie", "gen_slot -> id=%u", (unsigned) id);
  }

 protected:
  ClaudeLottieControl *parent_;
};

template <typename... Ts>
class ApplySlotAction : public Action<Ts...> {
 public:
  explicit ApplySlotAction(ClaudeLottieControl *parent) : parent_(parent) {}

  TEMPLATABLE_VALUE(uint32_t, slot_id)

  void play(Ts... x) override {
    auto anim = this->parent_->get_animation();
    if (anim == nullptr) return;
    tvg_lottie_animation_apply_slot(anim, this->slot_id_.value(x...));
  }

 protected:
  ClaudeLottieControl *parent_;
};

template <typename... Ts>
class DelSlotAction : public Action<Ts...> {
 public:
  explicit DelSlotAction(ClaudeLottieControl *parent) : parent_(parent) {}

  TEMPLATABLE_VALUE(uint32_t, slot_id)

  void play(Ts... x) override {
    auto anim = this->parent_->get_animation();
    if (anim == nullptr) return;
    tvg_lottie_animation_del_slot(anim, this->slot_id_.value(x...));
  }

 protected:
  ClaudeLottieControl *parent_;
};

template <typename... Ts>
class TweenAction : public Action<Ts...> {
 public:
  explicit TweenAction(ClaudeLottieControl *parent) : parent_(parent) {}

  TEMPLATABLE_VALUE(float, from)
  TEMPLATABLE_VALUE(float, to)
  TEMPLATABLE_VALUE(float, progress)

  void play(Ts... x) override {
    auto anim = this->parent_->get_animation();
    if (anim == nullptr) return;
    tvg_lottie_animation_tween(anim,
                               this->from_.value(x...),
                               this->to_.value(x...),
                               this->progress_.value(x...));
  }

 protected:
  ClaudeLottieControl *parent_;
};

template <typename... Ts>
class SetMarkerAction : public Action<Ts...> {
 public:
  explicit SetMarkerAction(ClaudeLottieControl *parent) : parent_(parent) {}

  TEMPLATABLE_VALUE(std::string, marker)

  void play(Ts... x) override {
    auto anim = this->parent_->get_animation();
    if (anim == nullptr) return;
    const std::string m = this->marker_.value(x...);
    tvg_lottie_animation_set_marker(anim, m.empty() ? nullptr : m.c_str());
  }

 protected:
  ClaudeLottieControl *parent_;
};

template <typename... Ts>
class SetQualityAction : public Action<Ts...> {
 public:
  explicit SetQualityAction(ClaudeLottieControl *parent) : parent_(parent) {}

  TEMPLATABLE_VALUE(uint8_t, quality)

  void play(Ts... x) override {
    auto anim = this->parent_->get_animation();
    if (anim == nullptr) return;
    tvg_lottie_animation_set_quality(anim, this->quality_.value(x...));
  }

 protected:
  ClaudeLottieControl *parent_;
};

}  // namespace claude_lottie_control
}  // namespace esphome

#endif  // LV_USE_LOTTIE
#endif  // USE_ESP32
