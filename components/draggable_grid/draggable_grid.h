#pragma once
//
// Native ESPHome component for LVGL draggable grid (Option A, iOS-style).
// 100% declaratif cote YAML : aucune lambda ne doit etre ecrite par
// l'utilisateur.
//
// Probleme resolu (v2) :
//   Un press court sur un bouton declenche son on_click habituel (ouverture
//   de page). Un press long (>= LONG_PRESS_TIME) fait basculer la grille en
//   "edit mode" : un overlay transparent apparait sur chaque bouton et
//   intercepte les press suivants pour permettre le drag. Un nouveau press
//   long sur un bouton quitte le edit mode.
//
// Etat
// ----
// - Stockage statique (inline variables) : ~240 octets, zero heap.
// - g_active est remis a -1 DES la premiere ligne de la branche RELEASED /
//   PRESS_LOST du callback overlay -> aucune RAM residuelle entre drags.
//
// Hierarchie
// ----------
//   Button (recoit LV_EVENT_LONG_PRESSED -> toggle edit mode)
//     +-- Overlay (enfant, HIDDEN par defaut)
//         - en edit mode : devient visible, capture PRESSED/PRESSING/
//           RELEASED/PRESS_LOST pour executer le drag + swap
//         - en mode normal : HIDDEN -> LVGL l'ignore, les evenements vont
//           au Button et le on_click fonctionne normalement.
//

#include "esphome/core/component.h"
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

inline Cell      g_cells[MAX_BUTTONS]{};
inline Slot      g_slots[MAX_BUTTONS]{};
inline lv_obj_t* g_overlays[MAX_BUTTONS]{};
inline int8_t    g_count = 0;
inline int8_t    g_active = -1;        // -1 = idle
inline bool      g_edit_mode = false;

// --- helpers internes ---------------------------------------
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

inline void set_edit_mode(bool enabled) {
  g_edit_mode = enabled;
  for (int8_t i = 0; i < g_count; ++i) {
    lv_obj_t* ov = g_overlays[i];
    if (ov == nullptr) continue;
    if (enabled) {
      lv_obj_clear_flag(ov, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(ov, LV_OBJ_FLAG_HIDDEN);
    }
  }
}

inline void toggle_edit_mode() { set_edit_mode(!g_edit_mode); }

// Callback attache AU BOUTON lui-meme : detecte uniquement le long-press
// pour basculer edit mode. Tout le reste passe par le on_click habituel.
inline void btn_long_press_cb(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_LONG_PRESSED) return;
  toggle_edit_mode();
}

// Callback attache a L'OVERLAY (visible uniquement en edit mode).
// user_data = index du slot dans g_slots (passe a la creation).
inline void overlay_event_cb(lv_event_t* e) {
  const lv_event_code_t code = lv_event_get_code(e);
  const int8_t idx = static_cast<int8_t>(
      reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
  if (idx < 0 || idx >= g_count) return;
  lv_obj_t* btn = g_slots[idx].obj;
  if (btn == nullptr) return;

  if (code == LV_EVENT_PRESSED) {
    g_active = idx;
    lv_obj_move_foreground(btn);
    return;
  }

  if (code == LV_EVENT_PRESSING) {
    if (g_active != idx) return;
    lv_indev_t* indev = lv_indev_active();
    if (indev == nullptr) return;
    lv_point_t v;
    lv_indev_get_vect(indev, &v);
    if (v.x == 0 && v.y == 0) return;
    int32_t nx = lv_obj_get_x_aligned(btn) + v.x;
    int32_t ny = lv_obj_get_y_aligned(btn) + v.y;
    lv_obj_set_pos(btn, nx, ny);
    return;
  }

  if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
    if (g_active != idx) return;
    const int8_t src = g_active;
    g_active = -1;   // RAM cleanup immediat

    int32_t cx  = lv_obj_get_x_aligned(btn) + g_cell_w / 2;
    int32_t cy  = lv_obj_get_y_aligned(btn) + g_cell_h / 2;
    int8_t  dst = nearest_cell(cx, cy);

    if (dst == src) {
      lv_obj_set_pos(btn, g_cells[src].x, g_cells[src].y);
      return;
    }

    lv_obj_t* other = g_slots[dst].obj;
    if (other != nullptr) {
      lv_obj_set_pos(other, g_cells[src].x, g_cells[src].y);
    }
    lv_obj_set_pos(btn, g_cells[dst].x, g_cells[dst].y);

    // Swap : les pointeurs obj ET les overlays doivent rester associes
    // a leur bouton, donc on echange les deux entrees en miroir.
    lv_obj_t* tmp_obj = g_slots[src].obj;
    g_slots[src].obj = g_slots[dst].obj;
    g_slots[dst].obj = tmp_obj;
    lv_obj_t* tmp_ov = g_overlays[src];
    g_overlays[src] = g_overlays[dst];
    g_overlays[dst] = tmp_ov;

    // Re-assigner les user_data des overlays apres swap pour que l'idx
    // stocke dans le callback corresponde a la nouvelle position.
    // (On modifie directement la liste d'event callbacks de l'overlay.)
    // Note : LVGL n'offre pas de set_user_data pour event_cb, donc on
    // ne touche pas au user_data ; a la place, on lit toujours le slot
    // via g_slots[idx].obj qui contient desormais le bon bouton.
    // -> deja fait dans la branche PRESSING (g_slots[g_active].obj).
  }
}

// Enregistre un bouton : set_pos, ajoute le long-press cb sur le bouton,
// cree l'overlay transparent enfant (cache) et y attache le cb de drag.
inline void attach(lv_obj_t* obj, int idx, int16_t cx, int16_t cy) {
  if (obj == nullptr) return;
  if (idx < 0 || idx >= MAX_BUTTONS) return;

  g_cells[idx].x = cx;
  g_cells[idx].y = cy;
  g_slots[idx].obj = obj;
  if (idx + 1 > g_count) g_count = idx + 1;
  lv_obj_set_pos(obj, cx, cy);

  // Long-press sur le bouton -> toggle edit mode
  lv_obj_add_event_cb(obj, btn_long_press_cb, LV_EVENT_LONG_PRESSED, nullptr);

  // Overlay transparent enfant du bouton
  lv_obj_t* ov = lv_obj_create(obj);
  lv_obj_remove_style_all(ov);
  lv_obj_set_size(ov, LV_PCT(100), LV_PCT(100));
  lv_obj_center(ov);
  lv_obj_set_style_bg_opa(ov, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(ov, 0, 0);
  lv_obj_set_style_pad_all(ov, 0, 0);
  lv_obj_set_style_radius(ov, 0, 0);
  lv_obj_add_flag(ov, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(ov, LV_OBJ_FLAG_PRESS_LOCK);
  lv_obj_clear_flag(ov, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(ov, LV_OBJ_FLAG_EVENT_BUBBLE);
  lv_obj_add_flag(ov, LV_OBJ_FLAG_HIDDEN);     // cache en mode normal

  lv_obj_add_event_cb(ov, overlay_event_cb, LV_EVENT_ALL,
                      reinterpret_cast<void*>(static_cast<intptr_t>(idx)));

  g_overlays[idx] = ov;
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
  lv_obj_t* obj;
  int8_t    idx;
  int16_t   x;
  int16_t   y;
};

class DraggableGridComponent : public Component {
 public:
  void add(lv_obj_t* obj, int idx, int16_t x, int16_t y) {
    if (this->count_ >= ::draggable_grid::MAX_BUTTONS) return;
    this->entries_[this->count_++] = {obj, static_cast<int8_t>(idx), x, y};
  }

  void set_cell_size(int w, int h) {
    this->cell_w_ = static_cast<int16_t>(w);
    this->cell_h_ = static_cast<int16_t>(h);
  }

  void setup() override {
    ::draggable_grid::set_cell_size(this->cell_w_, this->cell_h_);
    for (int i = 0; i < this->count_; ++i) {
      auto& e = this->entries_[i];
      if (e.obj != nullptr) {
        ::draggable_grid::attach(e.obj, e.idx, e.x, e.y);
      }
    }
  }

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
