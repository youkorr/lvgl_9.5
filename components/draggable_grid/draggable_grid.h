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
// v3 : feedback visuel "breathing" doux
//   En edit mode tous les boutons pulsent legerement (scale 1.00 -> 1.05,
//   800 ms aller/retour, infini). C'est plus discret qu'une oscillation
//   et reste lisible. L'anim est stoppee et la transform remise a neutre
//   a la sortie du edit mode.
//
// Etat
// ----
// - Stockage statique (inline variables) : ~280 octets, zero heap.
// - g_active est remis a -1 DES la premiere ligne de la branche RELEASED /
//   PRESS_LOST du callback overlay -> aucune RAM residuelle entre drags.
// - L'identite d'un bouton (idx) est STABLE pour toute la duree de vie :
//   g_buttons[idx] et g_overlays[idx] ne sont jamais reordonnes. Seules
//   les correspondances cellule<->bouton (g_cell_of / g_button_at) sont
//   mises a jour au swap. Consequence : le user_data stocke dans le
//   callback overlay reste toujours valide apres un swap.
//
// Hierarchie
// ----------
//   Button (recoit LV_EVENT_LONG_PRESSED -> entre en edit mode)
//     +-- Overlay (enfant, HIDDEN par defaut)
//         - en edit mode : devient visible, capture PRESSED/PRESSING/
//           RELEASED/PRESS_LOST pour executer le drag + swap, et
//           LONG_PRESSED pour quitter le edit mode (puisque l'overlay
//           bloque le LONG_PRESSED du bouton sous-jacent).
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

// Geometrie configurable (mise a jour par le Component ESPHome au setup)
inline int16_t g_cell_w = 150;
inline int16_t g_cell_h = 100;

// Tables FIXES, indexees par identite de bouton (idx).
inline lv_obj_t* g_buttons[MAX_BUTTONS]{};   // pointeur du bouton idx
inline lv_obj_t* g_overlays[MAX_BUTTONS]{};  // pointeur de l'overlay idx
inline Cell      g_cells[MAX_BUTTONS]{};     // position d'une cellule

// Mappings courants cellule <-> bouton. Mis a jour uniquement au swap.
// - g_cell_of[btn]  = index de la cellule ou se trouve actuellement btn
// - g_button_at[c]  = index du bouton actuellement dans la cellule c
inline int8_t    g_cell_of[MAX_BUTTONS]{};
inline int8_t    g_button_at[MAX_BUTTONS]{};

inline int8_t    g_count = 0;
inline int8_t    g_active = -1;        // -1 = idle, sinon idx de bouton
inline bool      g_moved = false;      // true si le press en cours a deja
                                       // bouge -> on est en drag, pas en
                                       // long-press d'exit edit mode
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

// Breathing : applique transform_scale (256 = 1.00x) via une anim LVGL
// qui pulse de 256 -> 268 (~1.047x) en 800 ms aller/retour, infini.
// Pivot au centre pour que le scale soit symetrique. Petit decalage
// par idx (delay) pour eviter l'effet "tous synchros".
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
  // delay echelonne (~80 ms par idx) pour un rendu plus naturel
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
  for (int8_t i = 0; i < g_count; ++i) {
    lv_obj_t* ov  = g_overlays[i];
    lv_obj_t* btn = g_buttons[i];
    if (enabled) {
      // Overlay visible + bouton NON clickable : empeche on_press (donc
      // l'ouverture de page) de se declencher pendant le drag, meme si
      // le doigt tombe sur la bordure/padding non couvert par l'overlay.
      if (ov  != nullptr) lv_obj_clear_flag(ov, LV_OBJ_FLAG_HIDDEN);
      if (btn != nullptr) {
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        start_breathe(btn, i);
      }
    } else {
      if (ov  != nullptr) lv_obj_add_flag(ov, LV_OBJ_FLAG_HIDDEN);
      if (btn != nullptr) {
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        stop_breathe(btn);
      }
    }
  }
}

inline void toggle_edit_mode() { set_edit_mode(!g_edit_mode); }

// Retourne true si la page qui contient les boutons est visible a l'ecran.
// Si un ancetre du bouton est HIDDEN, c'est que sa page est inactive.
inline bool buttons_currently_visible() {
  if (g_count == 0 || g_buttons[0] == nullptr) return false;
  for (lv_obj_t* p = g_buttons[0]; p != nullptr; p = lv_obj_get_parent(p)) {
    if (lv_obj_has_flag(p, LV_OBJ_FLAG_HIDDEN)) return false;
  }
  return true;
}

// Callback attache AU BOUTON lui-meme : detecte uniquement le long-press
// pour basculer edit mode. Tout le reste passe par le on_click habituel.
// Garde-fou : si l'utilisateur a tap (on_press -> page.show) et garde le
// doigt pose > LONG_PRESS_TIME, LVGL tire encore LONG_PRESSED alors que
// la home est deja cachee. On skip pour eviter un edit mode fantome qui
// se reveillerait au retour sur la page.
inline void btn_long_press_cb(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_LONG_PRESSED) return;
  if (!buttons_currently_visible()) return;
  toggle_edit_mode();
}

// Callback attache a L'OVERLAY (visible uniquement en edit mode).
// user_data = identite STABLE du bouton (idx d'origine a l'attach).
// Apres un swap, g_buttons[idx] et g_overlays[idx] ne changent PAS ;
// seul g_cell_of[idx] est mis a jour. Donc ce callback continue de
// pointer correctement sur son propre bouton meme apres des swaps.
inline void overlay_event_cb(lv_event_t* e) {
  const lv_event_code_t code = lv_event_get_code(e);
  const int8_t idx = static_cast<int8_t>(
      reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
  if (idx < 0 || idx >= g_count) return;
  lv_obj_t* btn = g_buttons[idx];
  if (btn == nullptr) return;

  // En edit mode, l'overlay bloque LONG_PRESSED du bouton sous-jacent.
  // On gere donc la sortie de edit mode ici, MAIS uniquement si le
  // press en cours n'est pas un drag (g_moved == false). Sinon LVGL
  // declenche LONG_PRESSED en pleine manipulation et on perdrait le
  // swap au RELEASED qui suit.
  if (code == LV_EVENT_LONG_PRESSED) {
    if (g_active == idx && g_moved) return;   // drag actif, on ignore
    g_active = -1;
    toggle_edit_mode();
    return;
  }

  if (code == LV_EVENT_PRESSED) {
    g_active = idx;
    g_moved = false;
    lv_obj_move_foreground(btn);
    // "Lift" : on stoppe le breathing du bouton saisi et on le grossit
    // legerement pour signaler qu'il est en cours de drag.
    lv_anim_delete(btn, breathe_exec_cb);
    lv_obj_set_style_transform_scale(btn, 282, LV_PART_MAIN);  // ~1.10x
    return;
  }

  if (code == LV_EVENT_PRESSING) {
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

  if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
    if (g_active != idx) return;
    g_active = -1;   // RAM cleanup immediat
    g_moved = false;

    // Fin du "lift" : on relance le breathing si on est toujours en
    // edit mode (sinon on reste a 1.0x, ce que stop_breathe a fait).
    if (g_edit_mode) {
      start_breathe(btn, idx);
    } else {
      lv_obj_set_style_transform_scale(btn, 256, LV_PART_MAIN);
    }

    const int8_t src_cell = g_cell_of[idx];

    int32_t cx  = lv_obj_get_x_aligned(btn) + g_cell_w / 2;
    int32_t cy  = lv_obj_get_y_aligned(btn) + g_cell_h / 2;
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

    // Mise a jour des mappings cellule <-> bouton UNIQUEMENT.
    // g_buttons[] et g_overlays[] restent inchanges -> l'identite idx
    // stockee dans user_data reste valide pour le prochain drag.
    g_cell_of[idx] = dst_cell;
    g_button_at[dst_cell] = idx;
    if (other_idx >= 0 && other_idx < g_count) {
      g_cell_of[other_idx] = src_cell;
      g_button_at[src_cell] = other_idx;
    }
  }
}

// Enregistre un bouton : set_pos, ajoute le long-press cb sur le bouton,
// cree l'overlay transparent enfant (cache) et y attache le cb de drag.
inline void attach(lv_obj_t* obj, int idx, int16_t cx, int16_t cy) {
  if (obj == nullptr) return;
  if (idx < 0 || idx >= MAX_BUTTONS) return;

  g_cells[idx].x = cx;
  g_cells[idx].y = cy;
  g_buttons[idx] = obj;
  g_cell_of[idx] = idx;       // au depart chaque bouton occupe sa cellule
  g_button_at[idx] = idx;
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
