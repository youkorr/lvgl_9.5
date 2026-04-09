#pragma once

#include "esphome/core/component.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/microphone/microphone.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace esphome {
namespace claude_audio_level {

/// Real-time audio level sensor that taps into an existing microphone
/// and publishes a smoothed RMS value suitable for driving UI animations.
///
/// The component registers a data callback on the supplied microphone.
/// Samples are expected to be signed 16-bit PCM. For each incoming
/// buffer, the RMS is computed, multiplied by `scale_`, and fed through
/// an exponential moving average (alpha = smoothing_). The smoothed
/// value is published as a sensor reading no more frequently than
/// `update_interval_` milliseconds.
///
/// The callback path is lock-free: the audio thread only writes to
/// atomics, and the ESPHome main loop reads them to publish.
class ClaudeAudioLevel : public sensor::Sensor, public Component {
 public:
  void set_microphone(microphone::Microphone *mic) { this->mic_ = mic; }
  void set_update_interval(uint32_t ms) { this->update_interval_ms_ = ms; }
  void set_smoothing(float s) { this->smoothing_ = s; }
  void set_auto_start(bool b) { this->auto_start_ = b; }
  void set_scale(float s) { this->scale_ = s; }

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

  /// Start listening (idempotent).
  void start();
  /// Stop listening (will publish 0 once).
  void stop();

 protected:
  /// Called from the microphone task with a raw PCM byte buffer.
  void on_audio_data_(const std::vector<uint8_t> &data);

  microphone::Microphone *mic_{nullptr};
  uint32_t update_interval_ms_{33};
  float smoothing_{0.6f};
  bool auto_start_{true};
  float scale_{3.0f};

  /// Latest smoothed value pushed by the audio callback. Atomic so the
  /// main loop can safely read it.
  std::atomic<float> latest_level_{0.0f};
  /// Flag to guarantee at least one publish per update interval when
  /// no new audio arrives (e.g. mic stopped mid-stream).
  std::atomic<bool> has_new_data_{false};

  uint32_t last_publish_ms_{0};
  bool callback_registered_{false};
  bool running_{false};
};

}  // namespace claude_audio_level
}  // namespace esphome
