#pragma once

#include <utility>

#include "esphome/components/number/number.h"
#include "esphome/core/component.h"
#include "esphome/core/preferences.h"
#include "lvgl.h"

namespace esphome {
namespace lvgl {

class LVGLNumber : public number::Number, public Component {
 public:
  LVGLNumber(std::function<void(float)> control_lambda, std::function<float()> value_lambda, lv_event_code_t event,
             bool restore)
      : control_lambda_(std::move(control_lambda)),
        value_lambda_(std::move(value_lambda)),
        event_(event),
        restore_(restore) {}

  void setup() override {
    float value;
    lv_lock();
    value = this->value_lambda_();
    lv_unlock();
    if (this->restore_) {
      this->pref_ = global_preferences->make_preference<float>(this->get_preference_hash());
      if (this->pref_.load(&value)) {
        lv_lock();
        this->control_lambda_(value);
        lv_unlock();
      }
    }
    this->publish_state(value);
  }

  void on_value() {
    lv_lock();
    float value = this->value_lambda_();
    lv_unlock();
    this->publish_(value);
  }

 protected:
  void publish_(float value) {
    this->publish_state(value);
    if (this->restore_)
      this->pref_.save(&value);
  }
  void control(float value) override {
    lv_lock();
    this->control_lambda_(value);
    lv_unlock();
    this->publish_(value);
  }
  std::function<void(float)> control_lambda_;
  std::function<float()> value_lambda_;
  lv_event_code_t event_;
  bool restore_;
  ESPPreferenceObject pref_{};
};

}  // namespace lvgl
}  // namespace esphome
