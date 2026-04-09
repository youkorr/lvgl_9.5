#include "claude_audio_level.h"

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <algorithm>
#include <cmath>

namespace esphome {
namespace claude_audio_level {

static const char *const TAG = "claude_audio_level";

void ClaudeAudioLevel::setup() {
  if (this->mic_ == nullptr) {
    ESP_LOGE(TAG, "No microphone configured");
    this->mark_failed();
    return;
  }

  // Register a passive data callback on the microphone. This lets us
  // observe the audio stream without taking ownership — the voice
  // assistant can still pull samples from the same mic in parallel.
  this->mic_->add_data_callback([this](const std::vector<uint8_t> &data) {
    this->on_audio_data_(data);
  });
  this->callback_registered_ = true;

  // Publish an initial zero so LVGL bindings have a value right away.
  this->publish_state(0.0f);

  if (this->auto_start_) {
    this->start();
  }
}

void ClaudeAudioLevel::loop() {
  const uint32_t now = millis();
  if (now - this->last_publish_ms_ < this->update_interval_ms_) {
    return;
  }

  // Consume the latest level. We also apply an idle decay if nothing
  // new arrived: this makes the orb visibly fall back to rest when the
  // user stops talking instead of freezing on the last peak.
  float level = this->latest_level_.load(std::memory_order_relaxed);
  const bool fresh = this->has_new_data_.exchange(false, std::memory_order_relaxed);

  if (!fresh && this->running_) {
    // Exponential decay toward zero when no new frames arrive.
    level *= 0.85f;
    this->latest_level_.store(level, std::memory_order_relaxed);
  }

  this->publish_state(level);
  this->last_publish_ms_ = now;
}

void ClaudeAudioLevel::dump_config() {
  ESP_LOGCONFIG(TAG, "Claude Audio Level:");
  ESP_LOGCONFIG(TAG, "  Microphone: %s", this->mic_ != nullptr ? "OK" : "MISSING");
  ESP_LOGCONFIG(TAG, "  Update interval: %u ms", this->update_interval_ms_);
  ESP_LOGCONFIG(TAG, "  Smoothing: %.2f", this->smoothing_);
  ESP_LOGCONFIG(TAG, "  Scale: %.2f", this->scale_);
  ESP_LOGCONFIG(TAG, "  Auto start: %s", YESNO(this->auto_start_));
  LOG_SENSOR("  ", "Level", this);
}

void ClaudeAudioLevel::start() {
  if (this->running_) return;
  this->running_ = true;
  ESP_LOGD(TAG, "Audio level monitoring started");
}

void ClaudeAudioLevel::stop() {
  if (!this->running_) return;
  this->running_ = false;
  this->latest_level_.store(0.0f, std::memory_order_relaxed);
  this->has_new_data_.store(true, std::memory_order_relaxed);
  ESP_LOGD(TAG, "Audio level monitoring stopped");
}

void ClaudeAudioLevel::on_audio_data_(const std::vector<uint8_t> &data) {
  if (!this->running_) return;
  if (data.size() < 2) return;

  // Treat incoming buffer as signed 16-bit PCM samples.
  const int16_t *samples = reinterpret_cast<const int16_t *>(data.data());
  const size_t n = data.size() / sizeof(int16_t);
  if (n == 0) return;

  // Compute RMS in the [0, 1] range.
  double sum_sq = 0.0;
  for (size_t i = 0; i < n; ++i) {
    const float s = static_cast<float>(samples[i]) * (1.0f / 32768.0f);
    sum_sq += static_cast<double>(s) * static_cast<double>(s);
  }
  float rms = std::sqrt(static_cast<float>(sum_sq / static_cast<double>(n)));

  // Amplify: typical speech RMS sits around 0.05 – 0.2, which is too
  // subtle for a visual effect. Multiply, then soft-clip to ~1.3 so
  // the orb has headroom for loud peaks.
  float amplified = rms * this->scale_;
  if (amplified > 1.3f) amplified = 1.3f;

  // Exponential moving average smoothing.
  float prev = this->latest_level_.load(std::memory_order_relaxed);
  float smoothed = prev * this->smoothing_ + amplified * (1.0f - this->smoothing_);
  this->latest_level_.store(smoothed, std::memory_order_relaxed);
  this->has_new_data_.store(true, std::memory_order_relaxed);
}

}  // namespace claude_audio_level
}  // namespace esphome
