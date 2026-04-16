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
/// and publishes either a smoothed RMS value (LEVEL mode) or the energy
/// in one of three FFT bands (BASS/MID/TREBLE) suitable for driving UI
/// animations such as Lottie expression variables.
///
/// The component registers a data callback on the supplied microphone.
/// Samples are expected to be signed 16-bit PCM. For each incoming
/// buffer, the configured band metric is computed, fed through an
/// asymmetric smoothing envelope (fast attack, slow release), and
/// published at `update_interval_` ms intervals.
///
/// The callback path is lock-free: the audio thread only writes to
/// atomics, and the ESPHome main loop reads them to publish.
enum class AudioBand : uint8_t {
  LEVEL = 0,   ///< Full-band RMS (no FFT)
  BASS = 1,    ///< Low frequencies (roughly 0 – 250 Hz @ 16 kHz)
  MID = 2,     ///< Mid frequencies (roughly 250 Hz – 2 kHz)
  TREBLE = 3,  ///< High frequencies (roughly 2 kHz – Nyquist)
};

/// FFT window size used for band decomposition. 256 points @ 16 kHz gives
/// a 16 ms window with ~62.5 Hz resolution — enough to separate bass
/// from mid from treble for VU-style visualisations without the cost
/// of a larger transform.
constexpr size_t FFT_SIZE = 256;

class ClaudeAudioLevel : public sensor::Sensor, public Component {
 public:
  void set_microphone(microphone::Microphone *mic) { this->mic_ = mic; }
  void set_update_interval(uint32_t ms) { this->update_interval_ms_ = ms; }
  void set_smoothing(float s) { this->smoothing_ = s; }
  void set_auto_start(bool b) { this->auto_start_ = b; }
  void set_scale(float s) { this->scale_ = s; }
  void set_band(AudioBand b) { this->band_ = b; }
  void set_buffer_skip(int n) { this->buffer_skip_ = n; }
  void set_delta_gate(float d) { this->delta_gate_ = d; }

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

  /// Compute the RMS of the whole buffer and return a normalized [0..1]
  /// value scaled by `scale_` and soft-clipped at 1.3. Used in LEVEL mode.
  float compute_rms_(const int16_t *samples, size_t n);

  /// Feed new samples into the internal FFT window. When a full window
  /// is available, run an FFT and return the normalized energy of the
  /// configured band; otherwise return the most recent previous value.
  float compute_band_energy_(const int16_t *samples, size_t n);

  /// In-place radix-2 Cooley-Tukey FFT on `fft_real_` / `fft_imag_`.
  /// Length is `FFT_SIZE`.
  void fft_inplace_();

  microphone::Microphone *mic_{nullptr};
  // 150 ms = ~6.6 Hz publish rate. Plenty for visual bindings driving LVGL
  // style properties (slider, transform_scale) and low enough that sensor
  // state-change logs don't flood the console nor spam the HA API.
  uint32_t update_interval_ms_{150};
  float smoothing_{0.6f};
  bool auto_start_{true};
  float scale_{3.0f};
  AudioBand band_{AudioBand::LEVEL};
  // Skip FFT on (buffer_skip_ - 1) out of every buffer_skip_ mic buffers.
  // FFT is the dominant CPU cost in the mic task; decimating here gives
  // back cycles to the voice-assistant wake-word pipeline. Default 3 means
  // we only process ~33% of incoming buffers — still plenty for visuals.
  int buffer_skip_{3};
  // Minimum change in smoothed value required to re-publish. Prevents
  // float-jitter from generating one state update per callback while still
  // pushing through real level changes quickly. 0.0 disables gating.
  float delta_gate_{0.01f};

  // FFT working buffers. Only allocated when band_ != LEVEL.
  std::vector<float> fft_real_;
  std::vector<float> fft_imag_;
  std::vector<float> hann_window_;
  size_t fft_fill_{0};
  float last_band_value_{0.0f};

  /// Latest smoothed value pushed by the audio callback. Atomic so the
  /// main loop can safely read it.
  std::atomic<float> latest_level_{0.0f};
  /// Flag to guarantee at least one publish per update interval when
  /// no new audio arrives (e.g. mic stopped mid-stream).
  std::atomic<bool> has_new_data_{false};

  uint32_t last_publish_ms_{0};
  bool callback_registered_{false};
  bool running_{false};
  int buffer_skip_counter_{0};
  float last_published_value_{-1.0f};  // sentinel so first publish always goes through
};

}  // namespace claude_audio_level
}  // namespace esphome
