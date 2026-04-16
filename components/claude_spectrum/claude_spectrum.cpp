#ifdef USE_ESP32

#include "claude_spectrum.h"

#include "esphome/core/hal.h"

#include <algorithm>
#include <cmath>

namespace esphome {
namespace claude_spectrum {

static const char *const TAG = "claude_spectrum";

void ClaudeSpectrum::setup() {
  if (this->mic_ == nullptr) {
    ESP_LOGE(TAG, "No microphone configured");
    this->mark_failed();
    return;
  }
  if (this->canvas_obj_ == nullptr) {
    ESP_LOGE(TAG, "No target lv_canvas widget assigned");
    this->mark_failed();
    return;
  }

  // Allocate FFT working buffers + precompute Hann window.
  this->fft_real_.assign(SPECTRUM_FFT_SIZE, 0.0f);
  this->fft_imag_.assign(SPECTRUM_FFT_SIZE, 0.0f);
  this->hann_window_.assign(SPECTRUM_FFT_SIZE, 0.0f);
  for (size_t i = 0; i < SPECTRUM_FFT_SIZE; ++i) {
    this->hann_window_[i] = 0.5f * (1.0f - std::cos(2.0f * M_PI * i / (SPECTRUM_FFT_SIZE - 1)));
  }
  this->bars_raw_.assign(this->num_bars_, 0.0f);
  this->smoothed_bars_.assign(this->num_bars_, 0.0f);

  // Tap the microphone. ESPHome's microphone component broadcasts each
  // buffer to every registered data callback — the voice assistant keeps
  // receiving samples in parallel so this does not break the wake word.
  this->mic_->add_data_callback([this](const std::vector<uint8_t> &data) {
    this->on_audio_data_(data);
  });
  this->callback_registered_ = true;

  if (this->auto_start_) {
    this->start();
  }

  ESP_LOGI(TAG, "Bound to canvas %p (%dx%d), %d bars, r=[%.0f..%.0f]",
           this->canvas_obj_, this->width_, this->height_, this->num_bars_,
           this->radius_inner_, this->radius_outer_);
}

void ClaudeSpectrum::loop() {
  const uint32_t now = millis();
  if (now - this->last_redraw_ms_ < this->update_interval_ms_) return;
  this->last_redraw_ms_ = now;

  this->redraw_();
}

void ClaudeSpectrum::dump_config() {
  ESP_LOGCONFIG(TAG, "Claude Spectrum:");
  ESP_LOGCONFIG(TAG, "  Microphone: %s", this->mic_ != nullptr ? "OK" : "MISSING");
  ESP_LOGCONFIG(TAG, "  Canvas: %p", this->canvas_obj_);
  ESP_LOGCONFIG(TAG, "  Size: %dx%d", this->width_, this->height_);
  ESP_LOGCONFIG(TAG, "  Bars: %d", this->num_bars_);
  ESP_LOGCONFIG(TAG, "  Radius: inner=%.0f outer=%.0f", this->radius_inner_, this->radius_outer_);
  ESP_LOGCONFIG(TAG, "  Bar width: %d", this->bar_width_);
  ESP_LOGCONFIG(TAG, "  Update interval: %u ms (~%.0f Hz)",
                this->update_interval_ms_, 1000.0f / (float) this->update_interval_ms_);
  ESP_LOGCONFIG(TAG, "  Smoothing: %.2f", this->smoothing_);
  ESP_LOGCONFIG(TAG, "  Gain: %.2f", this->gain_);
  ESP_LOGCONFIG(TAG, "  Buffer skip: 1/%d", this->buffer_skip_);
  ESP_LOGCONFIG(TAG, "  Mirror: %s", YESNO(this->mirror_));
  ESP_LOGCONFIG(TAG, "  Auto start: %s", YESNO(this->auto_start_));
}

void ClaudeSpectrum::start() {
  if (this->running_.load()) return;
  this->running_.store(true);
  ESP_LOGD(TAG, "Spectrum monitoring started");
}

void ClaudeSpectrum::stop() {
  if (!this->running_.load()) return;
  this->running_.store(false);
  // Zero out shared bars so the next redraw clears the visualiser.
  {
    std::lock_guard<std::mutex> lock(this->bars_mutex_);
    std::fill(this->bars_raw_.begin(), this->bars_raw_.end(), 0.0f);
  }
  this->has_new_bars_.store(true);
  ESP_LOGD(TAG, "Spectrum monitoring stopped");
}

// ---------------------------------------------------------------------------
// Audio thread
// ---------------------------------------------------------------------------

void ClaudeSpectrum::on_audio_data_(const std::vector<uint8_t> &data) {
  if (!this->running_.load()) return;
  if (data.size() < 2) return;

  // Decimate incoming buffers — we only need ~30 Hz of FFT updates for a
  // smooth visual; the voice assistant always gets 100% of buffers.
  if (this->buffer_skip_ > 1) {
    this->buffer_skip_counter_++;
    if (this->buffer_skip_counter_ < this->buffer_skip_) return;
    this->buffer_skip_counter_ = 0;
  }

  const int16_t *samples = reinterpret_cast<const int16_t *>(data.data());
  const size_t n = data.size() / sizeof(int16_t);

  // Feed samples into the FFT window until it is full, then run one
  // analysis pass. Excess samples roll over into the next window.
  for (size_t i = 0; i < n; ++i) {
    const float s = static_cast<float>(samples[i]) * (1.0f / 32768.0f);
    this->fft_real_[this->fft_fill_] = s;
    this->fft_imag_[this->fft_fill_] = 0.0f;
    this->fft_fill_++;
    if (this->fft_fill_ >= SPECTRUM_FFT_SIZE) {
      this->process_window_();
      this->fft_fill_ = 0;
    }
  }
}

void ClaudeSpectrum::process_window_() {
  // Apply Hann window to reduce spectral leakage.
  for (size_t k = 0; k < SPECTRUM_FFT_SIZE; ++k) {
    this->fft_real_[k] *= this->hann_window_[k];
  }
  this->fft_inplace_();

  const size_t half = SPECTRUM_FFT_SIZE / 2;

  // Log-scale frequency mapping: bar i covers bins exp(t_i)..exp(t_{i+1})
  // where t is linearly interpolated between log(1) and log(half-1).
  // Bin 0 (DC) is skipped.
  const float log_min = std::log(1.0f);
  const float log_max = std::log(static_cast<float>(half - 1));

  std::vector<float> new_bars(this->num_bars_, 0.0f);
  for (int b = 0; b < this->num_bars_; ++b) {
    const float t0 = log_min + (log_max - log_min) * b / this->num_bars_;
    const float t1 = log_min + (log_max - log_min) * (b + 1) / this->num_bars_;
    size_t lo = std::max<size_t>(1, static_cast<size_t>(std::floor(std::exp(t0))));
    size_t hi = std::min<size_t>(half - 1, static_cast<size_t>(std::ceil(std::exp(t1))));
    if (hi < lo) hi = lo;

    double energy = 0.0;
    int count = 0;
    for (size_t k = lo; k <= hi; ++k) {
      const float re = this->fft_real_[k];
      const float im = this->fft_imag_[k];
      energy += static_cast<double>(re * re + im * im);
      count++;
    }
    if (count == 0) count = 1;
    float mag = std::sqrt(static_cast<float>(energy / count));
    // Log compression → perceptual loudness curve. Also emphasises
    // low-level detail so quiet speech still lights up bars.
    mag = std::log1pf(mag * 12.0f) * 0.25f;
    if (mag < 0.0f) mag = 0.0f;
    if (mag > 1.0f) mag = 1.0f;
    new_bars[b] = mag;
  }

  {
    std::lock_guard<std::mutex> lock(this->bars_mutex_);
    this->bars_raw_ = std::move(new_bars);
  }
  this->has_new_bars_.store(true);
}

void ClaudeSpectrum::fft_inplace_() {
  const size_t N = SPECTRUM_FFT_SIZE;

  // Bit-reversal permutation.
  size_t j = 0;
  for (size_t i = 1; i < N; ++i) {
    size_t bit = N >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
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
    const size_t halfL = len >> 1;
    for (size_t i = 0; i < N; i += len) {
      float w_re = 1.0f;
      float w_im = 0.0f;
      for (size_t k = 0; k < halfL; ++k) {
        const float u_re = this->fft_real_[i + k];
        const float u_im = this->fft_imag_[i + k];
        const float t_re = w_re * this->fft_real_[i + k + halfL] - w_im * this->fft_imag_[i + k + halfL];
        const float t_im = w_re * this->fft_imag_[i + k + halfL] + w_im * this->fft_real_[i + k + halfL];
        this->fft_real_[i + k] = u_re + t_re;
        this->fft_imag_[i + k] = u_im + t_im;
        this->fft_real_[i + k + halfL] = u_re - t_re;
        this->fft_imag_[i + k + halfL] = u_im - t_im;
        const float nw_re = w_re * wlen_re - w_im * wlen_im;
        const float nw_im = w_re * wlen_im + w_im * wlen_re;
        w_re = nw_re;
        w_im = nw_im;
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Main thread: smoothing + canvas redraw
// ---------------------------------------------------------------------------

void ClaudeSpectrum::redraw_() {
  if (this->canvas_obj_ == nullptr) return;

  // Snapshot the raw bars under lock, then release so the audio thread
  // can produce the next frame without blocking.
  std::vector<float> current;
  {
    std::lock_guard<std::mutex> lock(this->bars_mutex_);
    current = this->bars_raw_;
  }

  const int n = this->num_bars_;
  if (static_cast<int>(this->smoothed_bars_.size()) != n) {
    this->smoothed_bars_.assign(n, 0.0f);
  }

  // Asymmetric envelope: fast attack for snappy reaction, slow release
  // for a graceful decay — makes the visualiser feel alive instead of
  // strobe-y when speech stops.
  const float attack = 1.0f - this->smoothing_ * 0.5f;  // e.g. 0.625 default
  for (int i = 0; i < n; ++i) {
    float target = (i < static_cast<int>(current.size())) ? current[i] : 0.0f;
    float prev = this->smoothed_bars_[i];
    if (target > prev) {
      this->smoothed_bars_[i] = prev + (target - prev) * attack;
    } else {
      this->smoothed_bars_[i] = prev * this->smoothing_ + target * (1.0f - this->smoothing_);
    }
  }

  // ---- draw ----
  lv_color_t bg = lv_color_hex(this->bg_color_ & 0xFFFFFF);
  lv_canvas_fill_bg(this->canvas_obj_, bg, this->bg_opa_);

  lv_layer_t layer;
  lv_canvas_init_layer(this->canvas_obj_, &layer);

  lv_draw_line_dsc_t dsc;
  lv_draw_line_dsc_init(&dsc);
  dsc.opa = LV_OPA_COVER;
  dsc.width = this->bar_width_;
  dsc.round_start = true;
  dsc.round_end = true;

  const float cx = this->width_ * 0.5f;
  const float cy = this->height_ * 0.5f;
  const float bar_range = this->radius_outer_ - this->radius_inner_;

  const uint8_t sr = (this->color_start_ >> 16) & 0xFF;
  const uint8_t sg = (this->color_start_ >> 8) & 0xFF;
  const uint8_t sb = this->color_start_ & 0xFF;
  const uint8_t er = (this->color_end_ >> 16) & 0xFF;
  const uint8_t eg = (this->color_end_ >> 8) & 0xFF;
  const uint8_t eb = this->color_end_ & 0xFF;

  // When mirror is on, the visual half is [0..n/2) and we reflect into [n/2..n).
  // Otherwise each bar is its own independent slice of the full circle.
  const int visual_bars = this->mirror_ ? (n / 2) : n;

  for (int i = 0; i < n; ++i) {
    int src_i = i;
    if (this->mirror_ && i >= visual_bars) {
      src_i = n - 1 - i;  // mirror top half into bottom
    }
    const float level = this->smoothed_bars_[src_i] * this->gain_;
    const float clamped = level > 1.0f ? 1.0f : (level < 0.0f ? 0.0f : level);
    const float r_out = this->radius_inner_ + clamped * bar_range;

    // Angle: bar 0 at the top (−π/2), increasing clockwise.
    const float ang = -static_cast<float>(M_PI) * 0.5f
                      + 2.0f * static_cast<float>(M_PI) * i / n
                      + this->rotation_angle_;
    const float cs = std::cos(ang);
    const float sn = std::sin(ang);

    dsc.p1.x = static_cast<int32_t>(cx + this->radius_inner_ * cs);
    dsc.p1.y = static_cast<int32_t>(cy + this->radius_inner_ * sn);
    dsc.p2.x = static_cast<int32_t>(cx + r_out * cs);
    dsc.p2.y = static_cast<int32_t>(cy + r_out * sn);

    // Color lerp (start -> end) as a function of bar height.
    const float k = clamped;
    const uint8_t cr = static_cast<uint8_t>(sr + (er - sr) * k);
    const uint8_t cg = static_cast<uint8_t>(sg + (eg - sg) * k);
    const uint8_t cb = static_cast<uint8_t>(sb + (eb - sb) * k);
    dsc.color = lv_color_make(cr, cg, cb);

    lv_draw_line(&layer, &dsc);
  }

  lv_canvas_finish_layer(this->canvas_obj_, &layer);

  if (this->rotate_speed_ != 0.0f) {
    this->rotation_angle_ += this->rotate_speed_;
    if (this->rotation_angle_ > 2.0f * static_cast<float>(M_PI))
      this->rotation_angle_ -= 2.0f * static_cast<float>(M_PI);
  }
}

}  // namespace claude_spectrum
}  // namespace esphome

#endif  // USE_ESP32
