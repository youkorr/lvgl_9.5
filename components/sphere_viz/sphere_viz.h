#pragma once

#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#ifdef USE_ESP32
#include <esp_heap_caps.h>
#include <esp_timer.h>
#endif

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

extern "C" {
#include "lvgl.h"
}

namespace esphome {
namespace sphere_viz {

enum SphereMode {
  MODE_WIREFRAME = 0,
  MODE_PARTICLES = 1,
  MODE_DYSON = 2,
};

// One pre-computed point on the unit sphere.
struct Vec3 {
  float x, y, z;
};

// Segment connecting two unit-sphere points (wireframe).
struct Edge {
  uint16_t a, b;
};

class SphereViz : public Component {
 public:
  void set_parent(lv_obj_t *parent) { this->parent_ = parent; }
  void set_geometry(int x, int y, int w, int h) {
    this->x_ = x;
    this->y_ = y;
    this->w_ = w;
    this->h_ = h;
  }
  void set_mode(SphereMode m) { this->mode_ = m; }
  void set_fps(int fps) { this->fps_ = fps; }
  void set_color(uint32_t argb) {
    this->color_ = argb;
    this->color_target_ = argb;
  }
  // Smoothly transition to a new color over the next few frames.
  void set_color_target(uint32_t argb) { this->color_target_ = argb; }
  void set_particle_count(int n) { this->particle_count_ = n; }
  void set_meridians(int n) { this->meridians_ = n; }
  void set_parallels(int n) { this->parallels_ = n; }
  void set_radius(int r) { this->radius_override_ = r; }

  // 0.0 = silence, 1.0 = full scale. Can be called from audio callback.
  void set_level(float v) {
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    this->target_level_ = v;
  }

  // Synthetic pulse when no live audio is available (e.g. during TTS).
  // amplitude 0..1, hz 0..20. amplitude=0 disables.
  void set_auto_pulse(float amplitude, float hz) {
    this->auto_amp_ = amplitude < 0.0f ? 0.0f : (amplitude > 1.0f ? 1.0f : amplitude);
    this->auto_hz_  = hz < 0.0f ? 0.0f : (hz > 20.0f ? 20.0f : hz);
  }

  float get_ema() const { return this->ema_level_; }

  void setup() override;
  void loop() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

 protected:
  // Geometry & config
  lv_obj_t *parent_{nullptr};
  int x_{0}, y_{0};
  int w_{400}, h_{400};
  int radius_override_{0};
  SphereMode mode_{MODE_WIREFRAME};
  int fps_{30};
  uint32_t color_{0x00FF88};   // current displayed color (0xRRGGBB)
  uint32_t color_target_{0x00FF88};  // target after smooth transition
  int particle_count_{600};
  int meridians_{12};
  int parallels_{8};

  // Synthetic pulse
  float auto_amp_{0.0f};
  float auto_hz_{0.0f};
  float auto_phase_{0.0f};

  // Runtime
  lv_obj_t *canvas_{nullptr};
  lv_draw_buf_t *draw_buf_{nullptr};
  uint8_t *pixels_{nullptr};
  int stride_{0};
  uint32_t last_frame_us_{0};

  // Continuous time in seconds, used for per-particle oscillation.
  float t_{0.0f};

  // Rotation state
  float yaw_{0.0f};
  float pitch_{0.0f};

  // Audio-reactive smoothing
  volatile float target_level_{0.0f};
  float ema_level_{0.0f};

  // Pre-computed unit sphere
  std::vector<Vec3> verts_;
  std::vector<Edge> edges_;

  // Per-particle scatter parameters (PARTICLES mode only).
  std::vector<Vec3> dirs_;     // random unit direction for "explosion" drift
  std::vector<float> phases_;  // random oscillation phase [0, 2π]

  // Build helpers
  void allocate_canvas_();
  void build_wireframe_();
  void build_particles_();

  // Rendering helpers (ARGB8888)
  inline void put_px_(int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a);
  inline void blend_px_(int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a);
  void clear_buffer_(uint32_t bg_argb);
  void draw_line_aa_(int x0, int y0, int x1, int y1, uint8_t r, uint8_t g, uint8_t b, uint8_t a);
  void draw_disc_(int cx, int cy, int rad, uint8_t r, uint8_t g, uint8_t b, uint8_t a);
  void draw_glow_point_(int cx, int cy, float rad, uint8_t r, uint8_t g, uint8_t b);

  void render_frame_();
};

}  // namespace sphere_viz
}  // namespace esphome
