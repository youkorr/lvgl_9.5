#include "claude_audio_level.h"

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <algorithm>
#include <cmath>

namespace esphome {
namespace claude_audio_level {

static const char *const TAG = "claude_audio_level";

static const char *band_name(AudioBand b) {
  switch (b) {
    case AudioBand::LEVEL:  return "LEVEL";
    case AudioBand::BASS:   return "BASS";
    case AudioBand::MID:    return "MID";
    case AudioBand::TREBLE: return "TREBLE";
  }
  return "?";
}

void ClaudeAudioLevel::setup() {
  if (this->mic_ == nullptr) {
    ESP_LOGE(TAG, "No microphone configured");
    this->mark_failed();
    return;
  }

  // Allocate FFT buffers only when we need them.
  if (this->band_ != AudioBand::LEVEL) {
    this->fft_real_.assign(FFT_SIZE, 0.0f);
    this->fft_imag_.assign(FFT_SIZE, 0.0f);
    this->hann_window_.assign(FFT_SIZE, 0.0f);
    // Pre-compute Hann window: w[n] = 0.5 * (1 - cos(2π n / (N-1)))
    for (size_t i = 0; i < FFT_SIZE; ++i) {
      this->hann_window_[i] = 0.5f * (1.0f - std::cos(2.0f * M_PI * i / (FFT_SIZE - 1)));
    }
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

  // Delta gating: only call publish_state() if the value changed by more
  // than delta_gate_ since the last publish. This suppresses thousands of
  // near-identical log lines when the mic is idle (always 0.0000..) and
  // keeps the HA API traffic proportional to actual activity.
  this->last_publish_ms_ = now;
  if (this->delta_gate_ > 0.0f && this->last_published_value_ >= 0.0f) {
    const float diff = std::fabs(level - this->last_published_value_);
    if (diff < this->delta_gate_) return;
  }
  this->publish_state(level);
  this->last_published_value_ = level;
}

void ClaudeAudioLevel::dump_config() {
  ESP_LOGCONFIG(TAG, "Claude Audio Level:");
  ESP_LOGCONFIG(TAG, "  Microphone: %s", this->mic_ != nullptr ? "OK" : "MISSING");
  ESP_LOGCONFIG(TAG, "  Band: %s", band_name(this->band_));
  ESP_LOGCONFIG(TAG, "  Update interval: %u ms", this->update_interval_ms_);
  ESP_LOGCONFIG(TAG, "  Smoothing: %.2f", this->smoothing_);
  ESP_LOGCONFIG(TAG, "  Scale: %.2f", this->scale_);
  ESP_LOGCONFIG(TAG, "  Buffer skip: 1/%d", this->buffer_skip_);
  ESP_LOGCONFIG(TAG, "  Delta gate: %.3f", this->delta_gate_);
  ESP_LOGCONFIG(TAG, "  Auto start: %s", YESNO(this->auto_start_));
  LOG_SENSOR("  ", "Level", this);
}

void ClaudeAudioLevel::start() {
  if (this->running_) return;
  this->running_ = true;
  ESP_LOGD(TAG, "Audio level monitoring started (band=%s)", band_name(this->band_));
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

  // Decimate incoming buffers so we don't hog CPU on the mic task. The
  // voice-assistant pipeline always gets 100% of the samples — we just
  // skip our own FFT/RMS work on (buffer_skip_ - 1) out of every N
  // buffers. Decisive for keeping wake-word detection reliable.
  if (this->buffer_skip_ > 1) {
    this->buffer_skip_counter_++;
    if (this->buffer_skip_counter_ < this->buffer_skip_) return;
    this->buffer_skip_counter_ = 0;
  }

  // Treat incoming buffer as signed 16-bit PCM samples.
  const int16_t *samples = reinterpret_cast<const int16_t *>(data.data());
  const size_t n = data.size() / sizeof(int16_t);
  if (n == 0) return;

  // Compute raw metric according to the configured band.
  float raw;
  if (this->band_ == AudioBand::LEVEL) {
    raw = this->compute_rms_(samples, n);
  } else {
    raw = this->compute_band_energy_(samples, n);
  }

  // Scale & soft-clip — the band path already emits roughly [0..1] so
  // the same scale/clip shape works for both modes.
  float amplified = raw * this->scale_;
  if (amplified > 1.3f) amplified = 1.3f;

  // Asymmetric envelope: fast attack (so the orb reacts instantly to
  // voice onset) + slow release (so it decays gracefully instead of
  // flickering between frames). This matches the mp4_player spectrum.
  const float prev = this->latest_level_.load(std::memory_order_relaxed);
  float smoothed;
  if (amplified > prev) {
    // Attack — move towards target fast.
    const float attack = 1.0f - this->smoothing_ * 0.5f;  // ~0.7 default
    smoothed = prev + (amplified - prev) * attack;
  } else {
    // Release — use the configured smoothing as the EMA alpha.
    smoothed = prev * this->smoothing_ + amplified * (1.0f - this->smoothing_);
  }
  this->latest_level_.store(smoothed, std::memory_order_relaxed);
  this->has_new_data_.store(true, std::memory_order_relaxed);
}

float ClaudeAudioLevel::compute_rms_(const int16_t *samples, size_t n) {
  double sum_sq = 0.0;
  for (size_t i = 0; i < n; ++i) {
    const float s = static_cast<float>(samples[i]) * (1.0f / 32768.0f);
    sum_sq += static_cast<double>(s) * static_cast<double>(s);
  }
  return std::sqrt(static_cast<float>(sum_sq / static_cast<double>(n)));
}

float ClaudeAudioLevel::compute_band_energy_(const int16_t *samples, size_t n) {
  // Feed samples into the FFT window until it is full, then run the
  // transform and extract the configured band's energy.
  for (size_t i = 0; i < n; ++i) {
    const float s = static_cast<float>(samples[i]) * (1.0f / 32768.0f);
    this->fft_real_[this->fft_fill_] = s;
    this->fft_imag_[this->fft_fill_] = 0.0f;
    this->fft_fill_++;

    if (this->fft_fill_ >= FFT_SIZE) {
      // Apply Hann window to reduce spectral leakage.
      for (size_t k = 0; k < FFT_SIZE; ++k) {
        this->fft_real_[k] *= this->hann_window_[k];
      }

      this->fft_inplace_();

      // Compute magnitude² per bin then split into three logarithmic
      // bands. Only the first half is meaningful (Nyquist).
      const size_t half = FFT_SIZE / 2;

      // Band edges in bins. For a 256-pt FFT @ 16 kHz Nyquist is 8 kHz
      // and each bin is 62.5 Hz wide. Roughly:
      //   BASS:   bin  1 .. 4    (62 – 250 Hz)
      //   MID:    bin  5 .. 32   (250 Hz – 2 kHz)
      //   TREBLE: bin 33 .. 127  (2 kHz – 8 kHz)
      size_t lo, hi;
      float gain;
      switch (this->band_) {
        case AudioBand::BASS:
          lo = 1; hi = 4; gain = 1.0f;
          break;
        case AudioBand::MID:
          lo = 5; hi = 32; gain = 0.8f;
          break;
        case AudioBand::TREBLE:
          lo = 33; hi = half - 1; gain = 1.5f;  // boost treble (usually quieter)
          break;
        default:
          lo = 1; hi = half - 1; gain = 1.0f;
          break;
      }

      double energy = 0.0;
      for (size_t k = lo; k <= hi; ++k) {
        const float re = this->fft_real_[k];
        const float im = this->fft_imag_[k];
        energy += static_cast<double>(re * re + im * im);
      }

      // Normalize by number of bins so short bands don't look tiny.
      const size_t bin_count = (hi - lo + 1);
      const float mean_energy = static_cast<float>(energy / bin_count);

      // Convert to magnitude and apply a simple log-ish curve that
      // matches human loudness perception more closely than raw linear.
      float mag = std::sqrt(mean_energy);
      // Log compression: map ~[0..0.3] linear range into ~[0..1].
      mag = std::log1pf(mag * 10.0f) * 0.25f * gain;
      if (mag < 0.0f) mag = 0.0f;
      if (mag > 1.0f) mag = 1.0f;

      this->last_band_value_ = mag;
      this->fft_fill_ = 0;  // ready for next window
    }
  }
  return this->last_band_value_;
}

void ClaudeAudioLevel::fft_inplace_() {
  // Radix-2 Cooley-Tukey FFT, iterative.
  const size_t N = FFT_SIZE;

  // Bit-reversal permutation.
  size_t j = 0;
  for (size_t i = 1; i < N; ++i) {
    size_t bit = N >> 1;
    for (; j & bit; bit >>= 1) {
      j ^= bit;
    }
    j ^= bit;
    if (i < j) {
      std::swap(this->fft_real_[i], this->fft_real_[j]);
      std::swap(this->fft_imag_[i], this->fft_imag_[j]);
    }
  }

  // Butterflies.
  for (size_t len = 2; len <= N; len <<= 1) {
    const float ang = -2.0f * static_cast<float>(M_PI) / static_cast<float>(len);
    const float wlen_re = std::cos(ang);
    const float wlen_im = std::sin(ang);
    const size_t half = len >> 1;
    for (size_t i = 0; i < N; i += len) {
      float w_re = 1.0f;
      float w_im = 0.0f;
      for (size_t k = 0; k < half; ++k) {
        const float u_re = this->fft_real_[i + k];
        const float u_im = this->fft_imag_[i + k];
        const float t_re = w_re * this->fft_real_[i + k + half] - w_im * this->fft_imag_[i + k + half];
        const float t_im = w_re * this->fft_imag_[i + k + half] + w_im * this->fft_real_[i + k + half];
        this->fft_real_[i + k] = u_re + t_re;
        this->fft_imag_[i + k] = u_im + t_im;
        this->fft_real_[i + k + half] = u_re - t_re;
        this->fft_imag_[i + k + half] = u_im - t_im;
        // Advance twiddle.
        const float nw_re = w_re * wlen_re - w_im * wlen_im;
        const float nw_im = w_re * wlen_im + w_im * wlen_re;
        w_re = nw_re;
        w_im = nw_im;
      }
    }
  }
}

}  // namespace claude_audio_level
}  // namespace esphome
