#pragma once
//
// Native ESPHome component for LVGL draggable grid.
// 100% declaratif cote YAML : aucune lambda ne doit etre ecrite par
// l'utilisateur. Toute la logique (etat, callbacks LVGL, swap, cleanup
// memoire) vit dans ce header.
//
// Etat : stockage statique uniquement (inline variables), zero heap.
// RAM cleanup : g_active est remis a -1 des la premiere ligne de
// event_cb() dans le cas RELEASED/PRESS_LOST, avant toute autre logique.
//
// Le C++ ci-dessous expose deux mondes :
//   - namespace draggable_grid : l'engine pur (zero dependance ESPHome)
//   - namespace esphome::draggable_grid_cmpt : le Component qui relie
//     l'engine aux widgets crees par le composant LVGL d'ESPHome, avec
//     setup_priority::LATE pour s'executer APRES la creation des widgets.
//

#include "esphome/core/component.h"
#include "esphome/components/lvgl/lvgl_esphome.h"
#include "lvgl.h"
#include <cstdint>
#include <climits>

// ============================================================
// Engine (pur LVGL)
// ============================================================
namespace draggable_grid {

constexpr int MAX_BUTTONS = 16;

struct Cell { int16_t x; int16_t y; };
struct Slot { lv_obj_t* obj; };

// Geometrie configurable (mise a jour par le Component ESPHome au setup)
inline int16_t g_cell_w = 150;
inline int16_t g_cell_h = 100;

inline Cell    g_cells[MAX_BUTTONS]{};
inline Slot    g_slots[MAX_BUTTONS]{};
inline int8_t  g_count = 0;
inline int8_t  g_active = -1;   // -1 = idle

// --- helpers internes ----------------------------------------
inline int8_t find_slot(lv_obj_t* obj) {
  for (int8_t i = 0; i < g_count; ++i) {
    if (g_slots[i].obj == obj) return i;
  }
  return -1;
}

inline int8_t nearest_cell(int32_t cx, int32_t cy) {
  int32_t best = INT32_MAX;
  int8_t  best_i = 0;
  for (int8_t i = 0; i < g_count; ++i) {
    int32_t dx = cx - (g_cells[i].x + g_cell_w / 2);
    int32_t dy = cy - (g_cells[i].y + g_cell_h / 2);
    int32_t d  = dx * dx + dy * dy;
    if (d < best) { best = d; best_i = i; }
  }
  return best_i;
}

// Unique callback LVGL, dispatch sur le code d'evenement.
inline void event_cb(lv_event_t* e) {
  const lv_event_code_t code = lv_event_get_code(e);
  lv_obj_t* obj = lv_event_get_target_obj(e);

  if (code == LV_EVENT_PRESSED) {
    int8_t s = find_slot(obj);
    if (s < 0) return;
    g_active = s;
    lv_obj_move_foreground(obj);
    return;
  }

  if (code == LV_EVENT_PRESSING) {
    if (g_active < 0 || g_slots[g_active].obj != obj) return;
    lv_indev_t* indev = lv_indev_active();
    if (indev == nullptr) return;
    lv_point_t v;
    lv_indev_get_vect(indev, &v);
    if (v.x == 0 && v.y == 0) return;
    int32_t nx = lv_obj_get_x_aligned(obj) + v.x;
    int32_t ny = lv_obj_get_y_aligned(obj) + v.y;
    lv_obj_set_pos(obj, nx, ny);
    return;
  }

  if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
    if (g_active < 0) return;
    const int8_t src = g_active;
    g_active = -1;   // <-- RAM cleanup immediat, avant toute autre logique
    if (g_slots[src].obj != obj) return;

    int32_t cx  = lv_obj_get_x_aligned(obj) + g_cell_w / 2;
    int32_t cy  = lv_obj_get_y_aligned(obj) + g_cell_h / 2;
    int8_t  dst = nearest_cell(cx, cy);

    if (dst == src) {
      lv_obj_set_pos(obj, g_cells[src].x, g_cells[src].y);
      return;
    }

    lv_obj_t* other = g_slots[dst].obj;
    if (other != nullptr) {
      lv_obj_set_pos(other, g_cells[src].x, g_cells[src].y);
    }
    lv_obj_set_pos(obj, g_cells[dst].x, g_cells[dst].y);
    g_slots[src].obj = other;
    g_slots[dst].obj = obj;
    return;
  }
}

// Enregistrement d'un bouton + attache du callback LVGL unique.
inline void attach(lv_obj_t* obj, int idx, int16_t cx, int16_t cy) {
  if (obj == nullptr) return;
  if (idx < 0 || idx >= MAX_BUTTONS) return;
  g_cells[idx].x = cx;
  g_cells[idx].y = cy;
  g_slots[idx].obj = obj;
  if (idx + 1 > g_count) g_count = idx + 1;
  lv_obj_set_pos(obj, cx, cy);
  lv_obj_add_event_cb(obj, event_cb, LV_EVENT_ALL, nullptr);
}

inline void set_cell_size(int w, int h) {
  g_cell_w = static_cast<int16_t>(w);
  g_cell_h = static_cast<int16_t>(h);
}

}  // namespace draggable_grid


// ============================================================
// ESPHome Component wrapper
// ============================================================
namespace esphome {
namespace draggable_grid_cmpt {

struct PendingEntry {
  lvgl::LvCompound* btn;
  int8_t  idx;
  int16_t x;
  int16_t y;
};

class DraggableGridComponent : public Component {
 public:
  void add(lvgl::LvCompound* btn, int idx, int16_t x, int16_t y) {
    if (this->count_ >= ::draggable_grid::MAX_BUTTONS) return;
    this->entries_[this->count_++] = {btn, static_cast<int8_t>(idx), x, y};
  }

  void set_cell_size(int w, int h) {
    this->cell_w_ = static_cast<int16_t>(w);
    this->cell_h_ = static_cast<int16_t>(h);
  }

  void setup() override {
    ::draggable_grid::set_cell_size(this->cell_w_, this->cell_h_);
    for (int i = 0; i < this->count_; ++i) {
      auto& e = this->entries_[i];
      if (e.btn && e.btn->obj) {
        ::draggable_grid::attach(e.btn->obj, e.idx, e.x, e.y);
      }
    }
  }

  // Priorite LATE (-100) -> s'execute APRES LVGL (PROCESSOR = 400)
  // donc les widgets sont deja crees quand on attache nos callbacks.
  float get_setup_priority() const override {
    return setup_priority::LATE;
  }

 private:
  PendingEntry entries_[::draggable_grid::MAX_BUTTONS]{};
  int8_t  count_ = 0;
  int16_t cell_w_ = 150;
  int16_t cell_h_ = 100;
};

}  // namespace draggable_grid_cmpt
}  // namespace esphome
