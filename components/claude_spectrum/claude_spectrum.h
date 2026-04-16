#pragma once

#ifdef USE_ESP32

#include "esphome/core/component.h"
#include "esphome/core/automation.h"
#include "esphome/core/log.h"
#include "esphome/components/microphone/microphone.h"

#include <lvgl.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace esphome {
namespace claude_spectrum {

/// 256-point FFT @ 16 kHz → 62.5 Hz per bin, 128 usable bins (0..8 kHz).
/// This is enough resolution for 32–64 radial bars with a logarithmic
/// frequency mapping, and small enough to run on the mic task without
/// delaying the voice-assistant pipeline.
constexpr size_t SPECTRUM_FFT_SIZE = 256;

/// Circular radial spectrum visualizer that draws into an existing
/// lv_canvas widget. The widget must be declared in the LVGL YAML (so
/// the user can position/size it wherever they like on the screen); we
/// only take an lv_obj_t* pointing at it.
///
/// The microphone stream is tapped via microphone::Microphone::add_data_callback,
/// shared non-destructively with other consumers (e.g. the voice assistant),
/// so this component does NOT block the wake-word pipeline. A configurable
/// buffer_skip lets you further reduce CPU load when needed.
class ClaudeSpectrum : public Component {
 public:
  // ---- wiring ----
  void set_microphone(microphone::Microphone *m) { this->mic_ = m; }
  void set_target(lv_obj_t *obj) { this->canvas_obj_ = obj; }

  // ---- geometry ----
  void set_size(int w, int h) {
    this->width_ = w;
    this->height_ = h;
  }
  void set_num_bars(int n) { this->num_bars_ = n; }
  void set_radius_inner(float r) { this->radius_inner_ = r; }
  void set_radius_outer(float r) { this->radius_outer_ = r; }
  void set_bar_width(int w) { this->bar_width_ = w; }

  // ---- colors (0xRRGGBB, opacity comes from bg_opa / line drawing at full) ----
  void set_color_start(uint32_t c) { this->color_start_ = c; }
  void set_color_end(uint32_t c) { this->color_end_ = c; }
  void set_bg_color(uint32_t c) { this->bg_color_ = c; }
  void set_bg_opa(uint8_t o) { this->bg_opa_ = o; }

  // ---- behavior ----
  void set_update_interval(uint32_t ms) { this->update_interval_ms_ = ms; }
  void set_smoothing(float s) { this->smoothing_ = s; }
  void set_gain(float g) { this->gain_ = g; }
  void set_auto_start(bool b) { this->auto_start_ = b; }
  void set_buffer_skip(int n) { this->buffer_skip_ = n; }
  void set_rotate_speed(float s) { this->rotate_speed_ = s; }
  void set_mirror(bool b) { this->mirror_ = b; }

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_CONNECTION; }

  void start();
  void stop();

 protected:
  /// Audio thread: fills the FFT window from the mic buffer, runs a
  /// windowed FFT once a full SPECTRUM_FFT_SIZE window is available,
  /// and publishes per-bar magnitudes into bars_raw_ under bars_mutex_.
  void on_audio_data_(const std::vector<uint8_t> &data);

  /// Run Hann window + FFT on the current fft_real_/fft_imag_ buffer and
  /// reduce the magnitude spectrum into num_bars_ logarithmic bands.
  void process_window_();

  /// Main-thread: smooth bars_raw_ into smoothed_bars_ (fast attack, slow
  /// release) and redraw the canvas with radial line segments.
  void redraw_();

  /// In-place iterative Cooley-Tukey radix-2 FFT on fft_real_ / fft_imag_.
  void fft_inplace_();

  // ---- wiring ----
  microphone::Microphone *mic_{nullptr};
  lv_obj_t *canvas_obj_{nullptr};

  // ---- geometry ----
  int width_{400};
  int height_{400};
  int num_bars_{48};
  float radius_inner_{120.0f};
  float radius_outer_{180.0f};
  int bar_width_{4};

  // ---- colors ----
  uint32_t color_start_{0x4B00FFu};  // deep purple
  uint32_t color_end_{0x00FFE5u};    // cyan
  uint32_t bg_color_{0x000000u};
  uint8_t bg_opa_{0};  // fully transparent by default — nice for overlay

  // ---- behavior ----
  uint32_t update_interval_ms_{40};  // ~25 Hz redraw
  float smoothing_{0.75f};            // release alpha (attack = 1 - smoothing/2)
  float gain_{1.8f};
  bool auto_start_{true};
  int buffer_skip_{2};  // 1 = every mic buffer, 2 = every other, 3 = every 3rd...
  float rotate_speed_{0.0f};  // radians/redraw; 0 = static
  bool mirror_{false};        // duplicate mirrored across Y axis (symmetric look)

  // ---- FFT buffers (audio thread only) ----
  std::vector<float> fft_real_;
  std::vector<float> fft_imag_;
  std::vector<float> hann_window_;
  size_t fft_fill_{0};
  int buffer_skip_counter_{0};

  // ---- shared bar state ----
  /// Raw magnitudes [0..1] per bar, produced by the audio thread. Guarded
  /// by bars_mutex_ for the copy-on-read pattern used in redraw_().
  std::vector<float> bars_raw_;
  std::mutex bars_mutex_;
  std::atomic<bool> has_new_bars_{false};

  /// Smoothed magnitudes, owned and updated exclusively by the main loop.
  std::vector<float> smoothed_bars_;

  // ---- misc runtime ----
  uint32_t last_redraw_ms_{0};
  float rotation_angle_{0.0f};
  std::atomic<bool> running_{false};
  bool callback_registered_{false};
};

// --------------------------------------------------------------------------
// Actions
// --------------------------------------------------------------------------

template <typename... Ts>
class StartAction : public Action<Ts...> {
 public:
  explicit StartAction(ClaudeSpectrum *parent) : parent_(parent) {}
  void play(const Ts &...x) override { this->parent_->start(); }

 protected:
  ClaudeSpectrum *parent_;
};

template <typename... Ts>
class StopAction : public Action<Ts...> {
 public:
  explicit StopAction(ClaudeSpectrum *parent) : parent_(parent) {}
  void play(const Ts &...x) override { this->parent_->stop(); }

 protected:
  ClaudeSpectrum *parent_;
};

}  // namespace claude_spectrum
}  // namespace esphome

#endif  // USE_ESP32
