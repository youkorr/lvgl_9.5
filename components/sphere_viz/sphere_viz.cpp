#include "sphere_viz.h"

#include <algorithm>

namespace esphome {
namespace sphere_viz {

static const char *const TAG = "sphere_viz";

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------

void SphereViz::setup() {
  if (this->parent_ == nullptr) {
    this->parent_ = lv_screen_active();
  }
  this->allocate_canvas_();
  if (this->mode_ == MODE_PARTICLES) {
    this->build_particles_();
  } else if (this->mode_ == MODE_DYSON) {
    this->build_dyson_shards_();
  } else {
    this->build_wireframe_();
  }
  this->last_frame_us_ = (uint32_t) (esp_timer_get_time() & 0xFFFFFFFF);
  ESP_LOGI(TAG, "SphereViz ready: %dx%d mode=%d verts=%d edges=%d",
           this->w_, this->h_, (int) this->mode_,
           (int) this->verts_.size(), (int) this->edges_.size());
}

void SphereViz::allocate_canvas_() {
  const int w = this->w_, h = this->h_;
  const size_t bytes = (size_t) w * h * 4;  // ARGB8888

  this->pixels_ = (uint8_t *) heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (this->pixels_ == nullptr) {
    this->pixels_ = (uint8_t *) heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
  }
  if (this->pixels_ == nullptr) {
    ESP_LOGE(TAG, "Out of memory allocating %u bytes for canvas", (unsigned) bytes);
    this->mark_failed();
    return;
  }
  this->stride_ = w * 4;
  std::memset(this->pixels_, 0, bytes);

  this->draw_buf_ = (lv_draw_buf_t *) heap_caps_malloc(sizeof(lv_draw_buf_t),
                                                       MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (this->draw_buf_ == nullptr) {
    ESP_LOGE(TAG, "Out of memory allocating draw_buf");
    this->mark_failed();
    return;
  }
  lv_draw_buf_init(this->draw_buf_, w, h, LV_COLOR_FORMAT_ARGB8888, 0, this->pixels_, bytes);
  lv_draw_buf_set_flag(this->draw_buf_, LV_IMAGE_FLAGS_MODIFIABLE);

  this->canvas_ = lv_canvas_create(this->parent_);
  lv_canvas_set_draw_buf(this->canvas_, this->draw_buf_);
  lv_obj_set_pos(this->canvas_, this->x_, this->y_);
  lv_obj_set_size(this->canvas_, w, h);
  lv_obj_clear_flag(this->canvas_, LV_OBJ_FLAG_CLICKABLE);
  // No widget chrome — only the ARGB pixels we draw should be visible.
  lv_obj_set_style_bg_opa(this->canvas_, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(this->canvas_, 0, LV_PART_MAIN);
  lv_obj_set_style_outline_width(this->canvas_, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(this->canvas_, 0, LV_PART_MAIN);
}

// ---------------------------------------------------------------------------
// Build geometry
// ---------------------------------------------------------------------------

void SphereViz::build_wireframe_() {
  // Meridians: lines of constant longitude; Parallels: lines of constant latitude.
  // Use 32 samples per line for smooth curves after 3D projection.
  const int SAMPLES = 32;
  this->verts_.clear();
  this->edges_.clear();
  this->verts_.reserve((this->meridians_ + this->parallels_) * SAMPLES);

  // Meridians
  for (int m = 0; m < this->meridians_; m++) {
    float lon = (float) m / (float) this->meridians_ * 2.0f * (float) M_PI;
    int start = (int) this->verts_.size();
    for (int s = 0; s < SAMPLES; s++) {
      float lat = -(float) M_PI / 2.0f + (float) s / (float) (SAMPLES - 1) * (float) M_PI;
      Vec3 v;
      v.x = std::cos(lat) * std::cos(lon);
      v.y = std::sin(lat);
      v.z = std::cos(lat) * std::sin(lon);
      this->verts_.push_back(v);
      if (s > 0) {
        this->edges_.push_back({(uint16_t) (start + s - 1), (uint16_t) (start + s)});
      }
    }
  }

  // Parallels (skip poles)
  for (int p = 1; p < this->parallels_; p++) {
    float lat = -(float) M_PI / 2.0f + (float) p / (float) this->parallels_ * (float) M_PI;
    float cy = std::sin(lat);
    float r  = std::cos(lat);
    int start = (int) this->verts_.size();
    for (int s = 0; s < SAMPLES; s++) {
      float lon = (float) s / (float) SAMPLES * 2.0f * (float) M_PI;
      Vec3 v;
      v.x = r * std::cos(lon);
      v.y = cy;
      v.z = r * std::sin(lon);
      this->verts_.push_back(v);
      int next = (s + 1) % SAMPLES;
      this->edges_.push_back({(uint16_t) (start + s), (uint16_t) (start + next)});
    }
  }
}

void SphereViz::build_particles_() {
  // Fibonacci sphere distribution — nicely uniform.
  this->verts_.clear();
  this->edges_.clear();
  this->dirs_.clear();
  this->phases_.clear();
  this->verts_.reserve(this->particle_count_);
  this->dirs_.reserve(this->particle_count_);
  this->phases_.reserve(this->particle_count_);

  const float ga = (float) M_PI * (3.0f - std::sqrt(5.0f));  // golden angle

  // Deterministic PRNG (xorshift32) so boot-to-boot particle scatter is stable.
  uint32_t rng = 0xC0FFEE42u;
  auto nextf = [&rng]() -> float {
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    // Map to [-1, 1]
    return ((int32_t) rng) * (1.0f / 2147483648.0f);
  };

  for (int i = 0; i < this->particle_count_; i++) {
    float y = 1.0f - (i / (float) (this->particle_count_ - 1)) * 2.0f;
    float r = std::sqrt(1.0f - y * y);
    float theta = ga * i;
    Vec3 v;
    v.x = std::cos(theta) * r;
    v.y = y;
    v.z = std::sin(theta) * r;
    this->verts_.push_back(v);

    // Random 3D direction for scatter (rejection-sampled unit vector).
    Vec3 d;
    for (int tries = 0; tries < 8; tries++) {
      d.x = nextf(); d.y = nextf(); d.z = nextf();
      float len2 = d.x * d.x + d.y * d.y + d.z * d.z;
      if (len2 > 0.05f && len2 <= 1.0f) {
        float inv = 1.0f / std::sqrt(len2);
        d.x *= inv; d.y *= inv; d.z *= inv;
        break;
      }
    }
    this->dirs_.push_back(d);
    // Random phase in [0, 2π)
    this->phases_.push_back((nextf() * 0.5f + 0.5f) * 2.0f * (float) M_PI);
  }
}

void SphereViz::build_dyson_shards_() {
  this->shards_.clear();
  this->shards_.reserve(this->shard_count_);

  const float ga = (float) M_PI * (3.0f - std::sqrt(5.0f));
  // Deterministic PRNG — stable shard pattern boot-to-boot.
  uint32_t rng = 0xBADF00Du;
  auto nextf = [&rng]() -> float {
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    return ((int32_t) rng) * (1.0f / 2147483648.0f);  // [-1, 1]
  };
  auto u01 = [&]() -> float { return (nextf() + 1.0f) * 0.5f; };

  for (int i = 0; i < this->shard_count_; i++) {
    Shard s;
    // Fibonacci sphere for even coverage.
    float y = 1.0f - (i / (float) (this->shard_count_ - 1)) * 2.0f;
    float rr = std::sqrt(1.0f - y * y);
    float theta = ga * i;
    s.center.x = std::cos(theta) * rr;
    s.center.y = y;
    s.center.z = std::sin(theta) * rr;

    // Orthonormal tangent basis (u, v) at `center`.
    Vec3 up{0.0f, 1.0f, 0.0f};
    if (std::fabs(s.center.y) > 0.98f) up = {1.0f, 0.0f, 0.0f};
    // u = normalize(cross(up, n))
    Vec3 n = s.center;
    Vec3 u;
    u.x = up.y * n.z - up.z * n.y;
    u.y = up.z * n.x - up.x * n.z;
    u.z = up.x * n.y - up.y * n.x;
    float ul = std::sqrt(u.x*u.x + u.y*u.y + u.z*u.z);
    u.x /= ul; u.y /= ul; u.z /= ul;
    // v = cross(n, u)
    Vec3 v;
    v.x = n.y * u.z - n.z * u.y;
    v.y = n.z * u.x - n.x * u.z;
    v.z = n.x * u.y - n.y * u.x;

    // Random in-plane rotation → unique triangle orientation.
    float phi = u01() * 2.0f * (float) M_PI;
    float cph = std::cos(phi), sph = std::sin(phi);
    Vec3 ur{u.x * cph + v.x * sph, u.y * cph + v.y * sph, u.z * cph + v.z * sph};
    Vec3 vr{-u.x * sph + v.x * cph, -u.y * sph + v.y * cph, -u.z * sph + v.z * cph};
    s.u = ur;
    s.v = vr;

    // Equilateral-ish triangle in local (u,v), with slight vertex jitter
    // so no two shards look identical.
    const float k = 0.95f;
    auto jitter = [&](float base) { return base + nextf() * 0.12f; };
    s.t[0] = jitter(0.0f);          s.t[1] = jitter(-1.0f * k);
    s.t[2] = jitter(0.866f * k);    s.t[3] = jitter(0.5f * k);
    s.t[4] = jitter(-0.866f * k);   s.t[5] = jitter(0.5f * k);

    // Mix of sizes → gives the cluttered swarm look from the reference.
    s.size = 0.08f + 0.05f * u01();
    this->shards_.push_back(s);
  }
}

// ---------------------------------------------------------------------------
// Pixel helpers
// ---------------------------------------------------------------------------

inline void SphereViz::put_px_(int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  if ((unsigned) x >= (unsigned) this->w_) return;
  if ((unsigned) y >= (unsigned) this->h_) return;
  uint8_t *p = this->pixels_ + y * this->stride_ + x * 4;
  // LVGL 9 ARGB8888 on ESP32-P4 displays this memory layout as R, G, B, A
  // when the display is configured RGB. Writing RGB order here keeps the
  // YAML color value (0xRRGGBB) visually correct.
  p[0] = r;
  p[1] = g;
  p[2] = b;
  p[3] = a;
}

inline void SphereViz::blend_px_(int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  if ((unsigned) x >= (unsigned) this->w_) return;
  if ((unsigned) y >= (unsigned) this->h_) return;
  uint8_t *p = this->pixels_ + y * this->stride_ + x * 4;
  // src-over blend with accumulated alpha (RGB order, see put_px_)
  uint32_t inv = 255 - a;
  p[0] = (uint8_t) ((r * a + p[0] * inv) / 255);
  p[1] = (uint8_t) ((g * a + p[1] * inv) / 255);
  p[2] = (uint8_t) ((b * a + p[2] * inv) / 255);
  uint32_t na = a + (p[3] * inv) / 255;
  p[3] = (uint8_t) (na > 255 ? 255 : na);
}

void SphereViz::clear_buffer_(uint32_t bg_argb) {
  const uint8_t a = (bg_argb >> 24) & 0xFF;
  const uint8_t r = (bg_argb >> 16) & 0xFF;
  const uint8_t g = (bg_argb >> 8) & 0xFF;
  const uint8_t b = bg_argb & 0xFF;
  uint8_t *p = this->pixels_;
  const int total = this->w_ * this->h_;
  for (int i = 0; i < total; i++) {
    p[0] = r;
    p[1] = g;
    p[2] = b;
    p[3] = a;
    p += 4;
  }
}

void SphereViz::draw_line_aa_(int x0, int y0, int x1, int y1,
                              uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  // Xiaolin Wu-style light AA on a Bresenham skeleton.
  int dx = std::abs(x1 - x0), dy = std::abs(y1 - y0);
  int sx = x0 < x1 ? 1 : -1;
  int sy = y0 < y1 ? 1 : -1;
  int err = dx - dy;
  int max_steps = dx + dy + 2;
  while (max_steps-- > 0) {
    this->blend_px_(x0, y0, r, g, b, a);
    // Soften neighbor perpendicular to dominant direction
    if (dx >= dy) this->blend_px_(x0, y0 + sy, r, g, b, a / 3);
    else          this->blend_px_(x0 + sx, y0, r, g, b, a / 3);
    if (x0 == x1 && y0 == y1) break;
    int e2 = 2 * err;
    if (e2 > -dy) { err -= dy; x0 += sx; }
    if (e2 <  dx) { err += dx; y0 += sy; }
  }
}

void SphereViz::draw_disc_(int cx, int cy, int rad,
                           uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  if (rad <= 0) {
    this->blend_px_(cx, cy, r, g, b, a);
    return;
  }
  int r2 = rad * rad;
  for (int dy = -rad; dy <= rad; dy++) {
    int yy = cy + dy;
    if ((unsigned) yy >= (unsigned) this->h_) continue;
    int span = (int) std::sqrt((float) (r2 - dy * dy));
    for (int dx = -span; dx <= span; dx++) {
      this->blend_px_(cx + dx, yy, r, g, b, a);
    }
  }
}

void SphereViz::draw_glow_point_(int cx, int cy, float rad,
                                 uint8_t r, uint8_t g, uint8_t b) {
  // Smooth radial point: softer Gaussian-like falloff (1-d/r)^2 so tiny
  // particles don't look like pixelated squares.
  if (rad < 1.2f) rad = 1.2f;
  int ir = (int) std::ceil(rad + 0.5f);
  const float inv_r = 1.0f / rad;
  for (int dy = -ir; dy <= ir; dy++) {
    int yy = cy + dy;
    if ((unsigned) yy >= (unsigned) this->h_) continue;
    for (int dx = -ir; dx <= ir; dx++) {
      int xx = cx + dx;
      if ((unsigned) xx >= (unsigned) this->w_) continue;
      float d = std::sqrt((float) (dx * dx + dy * dy)) * inv_r;
      if (d >= 1.0f) continue;
      float k = 1.0f - d;
      k = k * k;  // square for softer edge
      uint8_t aa = (uint8_t) (k * 255.0f);
      if (aa == 0) continue;
      this->blend_px_(xx, yy, r, g, b, aa);
    }
  }
}

void SphereViz::fill_triangle_(int x0, int y0, int x1, int y1, int x2, int y2,
                               uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  // Bounding box clipped to canvas.
  int xmin = std::min(std::min(x0, x1), x2);
  int ymin = std::min(std::min(y0, y1), y2);
  int xmax = std::max(std::max(x0, x1), x2);
  int ymax = std::max(std::max(y0, y1), y2);
  if (xmin < 0) xmin = 0;
  if (ymin < 0) ymin = 0;
  if (xmax >= this->w_) xmax = this->w_ - 1;
  if (ymax >= this->h_) ymax = this->h_ - 1;
  if (xmax < xmin || ymax < ymin) return;

  // Edge function.
  auto edge = [](int ax, int ay, int bx, int by, int cx, int cy) -> int {
    return (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
  };
  int area = edge(x0, y0, x1, y1, x2, y2);
  if (area == 0) return;
  // Sign-agnostic fill (works for CW and CCW).
  int sign = area > 0 ? 1 : -1;
  for (int y = ymin; y <= ymax; y++) {
    for (int x = xmin; x <= xmax; x++) {
      int w0 = edge(x1, y1, x2, y2, x, y) * sign;
      int w1 = edge(x2, y2, x0, y0, x, y) * sign;
      int w2 = edge(x0, y0, x1, y1, x, y) * sign;
      if (w0 >= 0 && w1 >= 0 && w2 >= 0) {
        this->blend_px_(x, y, r, g, b, a);
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Render frame
// ---------------------------------------------------------------------------

void SphereViz::render_frame_() {
  if (this->pixels_ == nullptr) return;

  // Synthetic pulse (used during TTS / thinking when no mic RMS is fed).
  // Mixes with real target_level so both can coexist.
  float synth = 0.0f;
  if (this->auto_amp_ > 0.001f && this->auto_hz_ > 0.001f) {
    const float dt = 1.0f / (float) this->fps_;
    this->auto_phase_ += 2.0f * (float) M_PI * this->auto_hz_ * dt;
    if (this->auto_phase_ > 2.0f * (float) M_PI) this->auto_phase_ -= 2.0f * (float) M_PI;
    // Half-rectified sine so it looks like breathing
    float s = (std::sin(this->auto_phase_) + 1.0f) * 0.5f;
    synth = this->auto_amp_ * s;
  }
  float mixed_target = this->target_level_ + synth;
  if (mixed_target > 1.0f) mixed_target = 1.0f;

  // EMA smoothing of the mixed level
  const float alpha = 0.35f;
  this->ema_level_ = this->ema_level_ + alpha * (mixed_target - this->ema_level_);
  const float lvl = this->ema_level_;

  // Smooth color transition: color_ → color_target_
  if (this->color_ != this->color_target_) {
    auto lerp_ch = [](uint8_t a, uint8_t b) -> uint8_t {
      int d = (int) b - (int) a;
      if (d > 0)  return (uint8_t) (a + ((d > 8) ? 8 : d));
      if (d < 0)  return (uint8_t) (a + ((d < -8) ? -8 : d));
      return a;
    };
    uint8_t cr = lerp_ch((this->color_ >> 16) & 0xFF, (this->color_target_ >> 16) & 0xFF);
    uint8_t cg = lerp_ch((this->color_ >> 8)  & 0xFF, (this->color_target_ >> 8)  & 0xFF);
    uint8_t cb = lerp_ch( this->color_        & 0xFF,  this->color_target_        & 0xFF);
    this->color_ = ((uint32_t) cr << 16) | ((uint32_t) cg << 8) | cb;
  }

  // Global time accumulator (used for per-particle oscillation)
  this->t_ += 1.0f / (float) this->fps_;
  if (this->t_ > 10000.0f) this->t_ -= 10000.0f;

  // Rotation progress
  this->yaw_   += 0.010f + lvl * 0.03f;
  this->pitch_ += 0.004f;
  if (this->yaw_   > 2.0f * (float) M_PI) this->yaw_   -= 2.0f * (float) M_PI;
  if (this->pitch_ > 2.0f * (float) M_PI) this->pitch_ -= 2.0f * (float) M_PI;

  const float cy = std::cos(this->yaw_),   sy = std::sin(this->yaw_);
  const float cp = std::cos(this->pitch_), sp = std::sin(this->pitch_);

  // Pulse: radius scales with audio
  const int base_r = (this->radius_override_ > 0)
      ? this->radius_override_
      : (int) (std::min(this->w_, this->h_) * 0.42f);
  const float pulse = 1.0f + 0.18f * lvl;
  const float R = base_r * pulse;
  const int ox = this->w_ / 2;
  const int oy = this->h_ / 2;

  // Transparent canvas — let LVGL page bg_color show through.
  this->clear_buffer_(0x00000000);

  // Extract color channels from user-configured color
  const uint8_t CR = (this->color_ >> 16) & 0xFF;
  const uint8_t CG = (this->color_ >> 8) & 0xFF;
  const uint8_t CB = this->color_ & 0xFF;

  // Brighten tint with audio
  auto brighten = [&](uint8_t c, float k) {
    int v = (int) (c * (0.5f + 0.5f * k));
    if (v > 255) v = 255;
    return (uint8_t) v;
  };

  // Project vertex → screen
  auto project = [&](const Vec3 &v, int &sx_out, int &sy_out, float &z_out) {
    // yaw around Y, then pitch around X
    float x1 =  v.x * cy + v.z * sy;
    float z1 = -v.x * sy + v.z * cy;
    float y1 = v.y;
    float y2 = y1 * cp - z1 * sp;
    float z2 = y1 * sp + z1 * cp;
    // Perspective: simulate slight depth
    float persp = 2.3f / (2.3f + z2);
    sx_out = (int) (ox + x1 * R * persp);
    sy_out = (int) (oy + y2 * R * persp);
    z_out  = z2;
  };

  // Subtle inner glow behind wireframe (bloom) — cheap filled circle.
  // DYSON has its own star core + halo, so skip this generic tint.
  if (this->mode_ != MODE_DYSON) {
    uint8_t gr = brighten(CR, lvl);
    uint8_t gg = brighten(CG, lvl);
    uint8_t gb = brighten(CB, lvl);
    int glow_r = (int) (R * (0.55f + 0.10f * lvl));
    this->draw_glow_point_(ox, oy, (float) glow_r, gr / 4, gg / 4, gb / 4);
  }

  if (this->mode_ == MODE_WIREFRAME) {
    // Draw each edge with depth-based alpha (back = faint, front = bright).
    for (const auto &e : this->edges_) {
      int ax, ay, bx, by;
      float az, bz;
      project(this->verts_[e.a], ax, ay, az);
      project(this->verts_[e.b], bx, by, bz);
      float zz = (az + bz) * 0.5f;
      float front = 0.5f - zz * 0.5f;
      if (front < 0.0f) front = 0.0f;
      if (front > 1.0f) front = 1.0f;
      uint8_t alpha = (uint8_t) (60 + front * (195.0f + lvl * 60.0f));
      if (alpha > 255) alpha = 255;
      uint8_t r = brighten(CR, front);
      uint8_t g = brighten(CG, front);
      uint8_t b = brighten(CB, front);
      this->draw_line_aa_(ax, ay, bx, by, r, g, b, alpha);
    }
  } else if (this->mode_ == MODE_DYSON) {
    // ------------------------------------------------------------------
    // DYSON — blinding white star core encapsulated by a cloud of
    // triangular panels ("Dyson swarm"). Panels are dark on the
    // star-facing side; their silhouette is rimmed in warm red/orange
    // from the backlight, matching the reference artwork.
    // ------------------------------------------------------------------

    // Star core: three layered glows (wide halo → warm mid → hot core).
    // Radii pulse with audio level.
    const int halo_r = (int) (R * (0.80f + 0.15f * lvl));
    const int mid_r  = (int) (R * (0.55f + 0.10f * lvl));
    const int core_r = (int) (R * (0.38f + 0.08f * lvl));
    this->draw_glow_point_(ox, oy, (float) halo_r,  80, 150, 210);
    this->draw_glow_point_(ox, oy, (float) mid_r,  180, 220, 245);
    this->draw_glow_point_(ox, oy, (float) core_r, 255, 252, 248);
    // Tiny sharp hot spot for a distinct "blown out" center.
    this->draw_glow_point_(ox, oy, (float) (core_r * 0.45f), 255, 255, 255);

    // Panel rendering.
    // 1) Project each shard's 3 vertices.
    // 2) Compute the triangle's world-space normal (= shard center after
    //    rotation). Panels facing the viewer (front-z) render bright
    //    and are drawn LAST so they end up on top.
    // 3) Draw far-side panels first with a warm-rim overlay so the
    //    backlight illusion reads correctly.
    const float spin = this->yaw_;
    const float pitch_now = this->pitch_;
    const float ccy = std::cos(spin), ssy = std::sin(spin);
    const float ccp = std::cos(pitch_now), ssp = std::sin(pitch_now);
    auto rot_vec = [&](const Vec3 &a) -> Vec3 {
      float x1 =  a.x * ccy + a.z * ssy;
      float z1 = -a.x * ssy + a.z * ccy;
      float y1 = a.y;
      Vec3 o;
      o.x = x1;
      o.y = y1 * ccp - z1 * ssp;
      o.z = y1 * ssp + z1 * ccp;
      return o;
    };

    // Two passes: back-facing first (dim), then front-facing (full).
    for (int pass = 0; pass < 2; pass++) {
      for (const auto &s : this->shards_) {
        // Rotated normal → determines front/back.
        Vec3 rn = rot_vec(s.center);
        bool back = rn.z > 0.0f;
        if ((pass == 0) != back) continue;  // pass 0 = back, pass 1 = front

        // Build 3 world-space vertices: center + t_i.x * u + t_i.y * v,
        // scaled by panel size, then sitting on a sphere slightly
        // larger than R so panels float outside the star surface.
        const float shell = 1.02f;
        Vec3 p[3];
        for (int k = 0; k < 3; k++) {
          float tx = s.t[k * 2]     * s.size;
          float ty = s.t[k * 2 + 1] * s.size;
          p[k].x = (s.center.x + s.u.x * tx + s.v.x * ty) * shell;
          p[k].y = (s.center.y + s.u.y * tx + s.v.y * ty) * shell;
          p[k].z = (s.center.z + s.u.z * tx + s.v.z * ty) * shell;
        }

        int sx[3], sy[3];
        float sz_avg = 0.0f;
        for (int k = 0; k < 3; k++) {
          float zk;
          project(p[k], sx[k], sy[k], zk);
          sz_avg += zk;
        }
        sz_avg /= 3.0f;
        float front = 0.5f - sz_avg * 0.5f;
        if (front < 0.0f) front = 0.0f;
        if (front > 1.0f) front = 1.0f;

        if (back) {
          // Silhouetted against the bright star: darker fill + red rim
          // where the light escapes around the edges.
          uint8_t fr = (uint8_t) (30  + 40 * (1.0f - front));
          uint8_t fg = (uint8_t) (50  + 50 * (1.0f - front));
          uint8_t fb = (uint8_t) (80  + 60 * (1.0f - front));
          this->fill_triangle_(sx[0], sy[0], sx[1], sy[1], sx[2], sy[2],
                               fr, fg, fb, 210);
          // Warm red rim (backlight bleed).
          this->draw_line_aa_(sx[0], sy[0], sx[1], sy[1], 255, 90,  60, 220);
          this->draw_line_aa_(sx[1], sy[1], sx[2], sy[2], 255, 110, 70, 220);
          this->draw_line_aa_(sx[2], sy[2], sx[0], sy[0], 255, 90,  60, 220);
        } else {
          // Front-facing: still dark blue-grey, but with cooler highlight
          // edges (cyan-white) — panels catch star light at the rim.
          uint8_t fr = (uint8_t) (20 + 35 * front);
          uint8_t fg = (uint8_t) (35 + 50 * front);
          uint8_t fb = (uint8_t) (70 + 90 * front);
          this->fill_triangle_(sx[0], sy[0], sx[1], sy[1], sx[2], sy[2],
                               fr, fg, fb, 235);
          uint8_t rim_a = (uint8_t) (140 + 80 * front);
          this->draw_line_aa_(sx[0], sy[0], sx[1], sy[1], 220, 235, 255, rim_a);
          this->draw_line_aa_(sx[1], sy[1], sx[2], sy[2], 220, 235, 255, rim_a);
          this->draw_line_aa_(sx[2], sy[2], sx[0], sy[0], 220, 235, 255, rim_a);
        }
      }
    }
  } else {
    // Particles: each particle drifts from its sphere seat along a per-particle
    // random direction. Amplitude scales with audio level — so during speech
    // the cloud "explodes" across the whole canvas, then collapses back at idle.
    //
    // Two-component amplitude (in units of sphere radius R):
    //   idle_wobble   — small constant jitter so even silence isn't frozen
    //   surge         — level-driven swing. 2.0 means particles can fly out
    //                   to twice the sphere radius, well past the canvas edge.
    const float idle_wobble = 0.04f;
    const float surge       = 2.0f * lvl;
    const float fast_t      = this->t_ * 3.2f;   // per-particle oscillation rate
    for (size_t i = 0; i < this->verts_.size(); i++) {
      const Vec3 &base = this->verts_[i];
      const Vec3 &dir  = this->dirs_[i];
      const float ph   = this->phases_[i];
      // Half-shifted sin so particles never all cross zero at once
      const float osc = std::sin(fast_t + ph);
      const float d   = idle_wobble * osc + surge * (0.5f + 0.5f * osc);
      Vec3 pos;
      pos.x = base.x + dir.x * d;
      pos.y = base.y + dir.y * d;
      pos.z = base.z + dir.z * d;

      int sx, sy;
      float sz;
      project(pos, sx, sy, sz);
      float front = 0.5f - sz * 0.5f;
      if (front < 0.0f) front = 0.0f;
      if (front > 1.0f) front = 1.0f;

      // Displaced particles render slightly larger & brighter ("hot")
      float boost = 1.0f + std::fabs(d) * 1.5f;
      float rad = (1.0f + front * (1.8f + lvl * 1.5f)) * boost;
      int ai = (int) (40 + front * (180.0f + lvl * 80.0f));
      if (ai > 255) ai = 255;
      uint8_t a = (uint8_t) ai;
      uint8_t r = brighten(CR, front);
      uint8_t g = brighten(CG, front);
      uint8_t b = brighten(CB, front);
      // Premultiply alpha into color for blend look
      uint8_t pr = (uint8_t) ((r * a) / 255);
      uint8_t pg = (uint8_t) ((g * a) / 255);
      uint8_t pb = (uint8_t) ((b * a) / 255);
      this->draw_glow_point_(sx, sy, rad, pr, pg, pb);
    }
  }

  // Tell LVGL the canvas buffer changed
  if (this->canvas_ != nullptr) {
    lv_obj_invalidate(this->canvas_);
  }
}

// ---------------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------------

void SphereViz::loop() {
  if (this->pixels_ == nullptr || this->canvas_ == nullptr) return;
  const uint32_t period_us = 1000000u / (uint32_t) this->fps_;
  const uint32_t now = (uint32_t) (esp_timer_get_time() & 0xFFFFFFFF);
  if ((now - this->last_frame_us_) < period_us) return;
  this->last_frame_us_ = now;
  this->render_frame_();
}

}  // namespace sphere_viz
}  // namespace esphome
