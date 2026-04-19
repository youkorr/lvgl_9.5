#pragma once
//
// Native ESPHome component for LVGL draggable grid (Option A, iOS-style).
// 100% declaratif cote YAML : aucune lambda ne doit etre ecrite par
// l'utilisateur, et le YAML des boutons reste inchange.
//
// Gestuelle (v4) :
//   - double clic sur un bouton      -> l'event LV_EVENT_PRESSED est
//                                       relaye au bouton -> on_press se
//                                       declenche (ouvre la page).
//   - simple clic sur un bouton      -> ne fait rien (evite les
//                                       ouvertures accidentelles).
//   - long-press sur un bouton       -> entre en "edit mode".
//     (en edit mode)
//   - drag d'un bouton               -> swap avec le bouton de la case
//                                       de depose (Option A).
//   - long-press sans bouger         -> sortie du edit mode.
//
// Feedback visuel :
//   - edit mode : tous les boutons "respirent" (scale 1.00 -> 1.05,
//     800 ms aller/retour, dephasage par idx). Le bouton saisi est
//     grossi a ~1.10x (lift) pendant le drag.
//   - normal mode : LV_STATE_PRESSED est manuellement applique au
//     bouton sur press/release pour conserver le style `pressed:`
//     defini en YAML (translate_y, bg_color, etc.).
//
// Architecture
// ------------
//   Button (toujours NON-CLICKABLE : ne recoit plus d'event directement)
//     +-- Overlay (enfant, TOUJOURS visible et clickable)
//         Unique point d'entree pour toutes les interactions. Route
//         soit vers double-click-relay, soit vers drag-edit selon
//         g_edit_mode.
//
// Etat
// ----
// - Stockage statique (inline variables) : ~300 octets, zero heap.
// - g_active / g_moved sont remis a 0/-1 en fin de chaque press.
// - L'identite d'un bouton (idx) est STABLE : g_buttons[idx] et
//   g_overlays[idx] ne sont jamais reordonnes. Les swaps ne touchent
//   que g_cell_of[] et g_button_at[], donc le user_data du callback
//   reste valide apres un nombre quelconque d'echanges.
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

// Fenetre maximum entre deux clics pour considerer un double-clic.
constexpr uint32_t DOUBLE_CLICK_WINDOW_MS = 400;

struct Cell { int16_t x; int16_t y; };

// Geometrie configurable (mise a jour par le Component ESPHome au setup)
inline int16_t g_cell_w = 150;
inline int16_t g_cell_h = 100;

// Tables FIXES, indexees par identite de bouton (idx).
inline lv_obj_t* g_buttons[MAX_BUTTONS]{};   // pointeur du bouton idx
inline lv_obj_t* g_overlays[MAX_BUTTONS]{};  // pointeur de l'overlay idx
inline Cell      g_cells[MAX_BUTTONS]{};     // position d'une cellule

// Mappings courants cellule <-> bouton. Mis a jour uniquement au swap.
inline int8_t    g_cell_of[MAX_BUTTONS]{};
inline int8_t    g_button_at[MAX_BUTTONS]{};

inline int8_t    g_count = 0;
inline int8_t    g_active = -1;        // -1 = idle, sinon idx de bouton
inline bool      g_moved = false;      // drag en cours si true
inline bool      g_long_fired = false; // le press en cours a deja tire
                                       // LONG_PRESSED -> ignorer le
                                       // relay double-click au RELEASED
inline bool      g_edit_mode = false;

// Double-click tracking (un seul bouton a la fois peut etre en attente)
inline int8_t    g_last_click_idx = -1;
inline uint32_t  g_last_click_time = 0;

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

// Breathing : pulse scale 1.00 -> ~1.047x, 800 ms A/R, infini.
inline void breathe_exec_cb(void* var, int32_t v) {
  lv_obj_set_style_transform_scale(
      static_cast<lv_obj_t*>(var), v, LV_PART_MAIN);
}

inline void start_breathe(lv_obj_t* btn, int8_t idx) {
  if (btn == nullptr) return;
  lv_obj_set_style_transform_pivot_x(btn, LV_PCT(50), LV_PART_MAIN);
  lv_obj_set_style_transform_pivot_y(btn, LV_PCT(50), LV_PART_MAIN);

  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, btn);
  lv_anim_set_exec_cb(&a, breathe_exec_cb);
  lv_anim_set_time(&a, 800);
  lv_anim_set_playback_time(&a, 800);
  lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
  lv_anim_set_values(&a, 256, 268);
  lv_anim_set_delay(&a, static_cast<uint32_t>(idx) * 80u);
  lv_anim_start(&a);
}

inline void stop_breathe(lv_obj_t* btn) {
  if (btn == nullptr) return;
  lv_anim_delete(btn, breathe_exec_cb);
  lv_obj_set_style_transform_scale(btn, 256, LV_PART_MAIN);
}

inline void set_edit_mode(bool enabled) {
  g_edit_mode = enabled;
  g_last_click_idx = -1;   // reset double-click state au changement de mode
  for (int8_t i = 0; i < g_count; ++i) {
    lv_obj_t* btn = g_buttons[i];
    if (btn == nullptr) continue;
    if (enabled) {
      start_breathe(btn, i);
    } else {
      stop_breathe(btn);
    }
  }
}

inline void toggle_edit_mode() { set_edit_mode(!g_edit_mode); }

// Callback attache A L'OVERLAY (toujours visible). Unique point
// d'entree pour toutes les interactions utilisateur.
//
// user_data = identite STABLE du bouton (idx d'origine a l'attach).
// Apres un swap, g_buttons[idx] et g_overlays[idx] ne changent PAS ;
// seul g_cell_of[idx] est mis a jour.
inline void overlay_event_cb(lv_event_t* e) {
  const lv_event_code_t code = lv_event_get_code(e);
  const int8_t idx = static_cast<int8_t>(
      reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
  if (idx < 0 || idx >= g_count) return;
  lv_obj_t* btn = g_buttons[idx];
  if (btn == nullptr) return;

  // --- LONG_PRESSED -------------------------------------------------
  // En edit mode : sortie du edit mode (sauf si on est en plein drag).
  // En normal mode : entree en edit mode.
  if (code == LV_EVENT_LONG_PRESSED) {
    if (g_edit_mode && g_active == idx && g_moved) return;  // drag actif
    g_long_fired = true;        // annule le relay double-click au RELEASED
    g_active = -1;
    g_moved = false;
    g_last_click_idx = -1;
    lv_obj_remove_state(btn, LV_STATE_PRESSED);  // propre cote visuel
    toggle_edit_mode();
    return;
  }

  // --- PRESSED ------------------------------------------------------
  if (code == LV_EVENT_PRESSED) {
    g_active = idx;
    g_moved = false;
    g_long_fired = false;
    if (g_edit_mode) {
      // Lift visuel du bouton saisi
      lv_obj_move_foreground(btn);
      lv_anim_delete(btn, breathe_exec_cb);
      lv_obj_set_style_transform_scale(btn, 282, LV_PART_MAIN);  // ~1.10x
    } else {
      // Applique le style `pressed:` du YAML (translate_y, bg_color,...)
      lv_obj_add_state(btn, LV_STATE_PRESSED);
    }
    return;
  }

  // --- PRESSING (drag en edit mode uniquement) ----------------------
  if (code == LV_EVENT_PRESSING) {
    if (!g_edit_mode) return;
    if (g_active != idx) return;
    lv_indev_t* indev = lv_indev_active();
    if (indev == nullptr) return;
    lv_point_t v;
    lv_indev_get_vect(indev, &v);
    if (v.x == 0 && v.y == 0) return;
    g_moved = true;
    int32_t nx = lv_obj_get_x_aligned(btn) + v.x;
    int32_t ny = lv_obj_get_y_aligned(btn) + v.y;
    lv_obj_set_pos(btn, nx, ny);
    return;
  }

  // --- RELEASED / PRESS_LOST ----------------------------------------
  if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
    if (g_active != idx) return;
    g_active = -1;

    // ---- NORMAL MODE : double-click relay ---------------------------
    if (!g_edit_mode) {
      lv_obj_remove_state(btn, LV_STATE_PRESSED);
      const bool short_click =
          (code == LV_EVENT_RELEASED) && !g_moved && !g_long_fired;
      g_moved = false;
      g_long_fired = false;
      if (!short_click) {
        g_last_click_idx = -1;
        return;
      }
      const uint32_t now = lv_tick_get();
      if (g_last_click_idx == idx &&
          (now - g_last_click_time) <= DOUBLE_CLICK_WINDOW_MS) {
        // Second clic dans la fenetre -> relaye au bouton pour declencher
        // le on_press: (et symetriquement RELEASED / CLICKED pour ne pas
        // laisser le bouton dans un etat incoherent).
        lv_obj_send_event(btn, LV_EVENT_PRESSED, nullptr);
        lv_obj_send_event(btn, LV_EVENT_RELEASED, nullptr);
        lv_obj_send_event(btn, LV_EVENT_CLICKED, nullptr);
        g_last_click_idx = -1;
      } else {
        g_last_click_idx = idx;
        g_last_click_time = now;
      }
      return;
    }

    // ---- EDIT MODE : fin du drag + swap -----------------------------
    g_moved = false;
    g_long_fired = false;
    start_breathe(btn, idx);    // le lift se resorbe, le breathe reprend

    const int8_t src_cell = g_cell_of[idx];

    int32_t cx = lv_obj_get_x_aligned(btn) + g_cell_w / 2;
    int32_t cy = lv_obj_get_y_aligned(btn) + g_cell_h / 2;
    int8_t  dst_cell = nearest_cell(cx, cy);

    if (dst_cell == src_cell) {
      lv_obj_set_pos(btn, g_cells[src_cell].x, g_cells[src_cell].y);
      return;
    }

    const int8_t other_idx = g_button_at[dst_cell];
    lv_obj_t* other = (other_idx >= 0 && other_idx < g_count)
                          ? g_buttons[other_idx]
                          : nullptr;
    if (other != nullptr) {
      lv_obj_set_pos(other, g_cells[src_cell].x, g_cells[src_cell].y);
    }
    lv_obj_set_pos(btn, g_cells[dst_cell].x, g_cells[dst_cell].y);

    g_cell_of[idx] = dst_cell;
    g_button_at[dst_cell] = idx;
    if (other_idx >= 0 && other_idx < g_count) {
      g_cell_of[other_idx] = src_cell;
      g_button_at[src_cell] = other_idx;
    }
  }
}

// Enregistre un bouton : set_pos, cree l'overlay transparent enfant
// (TOUJOURS visible / clickable), desactive CLICKABLE sur le bouton
// pour qu'aucun event ne l'atteigne directement.
inline void attach(lv_obj_t* obj, int idx, int16_t cx, int16_t cy) {
  if (obj == nullptr) return;
  if (idx < 0 || idx >= MAX_BUTTONS) return;

  g_cells[idx].x = cx;
  g_cells[idx].y = cy;
  g_buttons[idx] = obj;
  g_cell_of[idx] = idx;
  g_button_at[idx] = idx;
  if (idx + 1 > g_count) g_count = idx + 1;
  lv_obj_set_pos(obj, cx, cy);

  // Bouton non-clickable : plus jamais d'event direct. L'overlay est
  // l'unique chemin vers le bouton (via lv_obj_send_event sur double
  // clic ou via un relay manuel d'etat LV_STATE_PRESSED).
  lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);

  // Overlay transparent enfant, TOUJOURS visible et clickable.
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
