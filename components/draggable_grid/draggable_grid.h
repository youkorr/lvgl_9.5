#pragma once
//
// Native ESPHome component for LVGL draggable grid (Option A, iOS-style).
// 100% declaratif cote YAML : aucune lambda ne doit etre ecrite par
// l'utilisateur, et le YAML des boutons reste inchange.
//
// Gestuelle (v5) :
//   - double clic sur un bouton      -> l'event LV_EVENT_PRESSED est
//                                       relaye au bouton -> on_press se
//                                       declenche (ouvre la page).
//   - simple clic sur un bouton      -> ne fait rien (evite les
//                                       ouvertures accidentelles).
//   - long-press sur un bouton       -> entre en "edit mode".
//     (en edit mode)
//   - drag d'un bouton               -> REFLOW iOS : les autres widgets
//                                       glissent pour combler la place
//                                       pendant que le doigt bouge.
//                                       Au lacher, le bouton se pose sur
//                                       la derniere case survolee.
//   - long-press sans bouger         -> sortie du edit mode.
//
// Feedback visuel (tout base sur la translation = PPA-friendly sur
// ESP32-P4 ; transform_scale est volontairement evite car l'unite PPA
// rejette les blits scales et bascule en software rendering) :
//   - edit mode : tous les boutons "respirent" par bob translate_y de
//     0 -> -2 px (1000 ms A/R, dephasage par idx). Le bouton saisi
//     est souleve de -6 px (translate_y) pendant le drag.
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
#include "esphome/core/automation.h"
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
inline bool      g_pinned[MAX_BUTTONS]{};    // draggable=false : pas de
                                             // overlay, pas dans le reflow

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

// Parent scroll lock pendant le drag : l'indev LVGL peut sinon
// convertir la gestuelle en scroll sur un ancetre scrollable.
inline lv_obj_t* g_drag_parent = nullptr;
inline bool      g_parent_was_scrollable = false;

// Double-click tracking (un seul bouton a la fois peut etre en attente)
inline int8_t    g_last_click_idx = -1;
inline uint32_t  g_last_click_time = 0;

// Reflow : snapshot de la sequence de boutons au debut d'un drag, plus
// le cell actuellement "occupe" par le bouton saisi (mis a jour pendant
// le PRESSING). Permet de calculer la disposition reflowee a chaque
// changement de case survolee.
inline int8_t    g_pre_drag_order[MAX_BUTTONS]{};  // order[cell] = btn idx
inline int8_t    g_last_target_cell = -1;

// --- helpers internes ---------------------------------------
// Renvoie -1 si aucune cellule libre (tout pinned, improbable).
inline int8_t nearest_cell(int32_t cx, int32_t cy) {
  int32_t best = INT32_MAX;
  int8_t  best_i = -1;
  for (int8_t i = 0; i < g_count; ++i) {
    // Une cellule occupee par un bouton pinne est intangible : le
    // bouton drague ne peut pas s'y poser ni pousser son occupant.
    const int8_t occupant = g_button_at[i];
    if (occupant >= 0 && g_pinned[occupant]) continue;
    int32_t dx = cx - (g_cells[i].x + g_cell_w / 2);
    int32_t dy = cy - (g_cells[i].y + g_cell_h / 2);
    int32_t d  = dx * dx + dy * dy;
    if (d < best) { best = d; best_i = i; }
  }
  return best_i;
}

// Animation de position packee (x+y dans un seul lv_anim_t, 1 call
// lv_obj_set_pos par frame au lieu de 2). Cible : ESP32-P4 sans CPU
// burn inutile.
struct MoveAnim {
  lv_obj_t* btn;
  int16_t start_x, start_y;
  int16_t end_x,   end_y;
};
inline MoveAnim g_move_anims[MAX_BUTTONS]{};

inline void anim_pos_exec_cb(void* var, int32_t v) {
  MoveAnim* m = static_cast<MoveAnim*>(var);
  if (m == nullptr || m->btn == nullptr) return;
  int32_t x = m->start_x + ((m->end_x - m->start_x) * v) / 1000;
  int32_t y = m->start_y + ((m->end_y - m->start_y) * v) / 1000;
  lv_obj_set_pos(m->btn, x, y);
}

// Reflow en ~150 ms : snap mais visible, proche du feel iOS.
constexpr uint32_t REFLOW_MS = 150;

inline void animate_btn_to(int8_t btn_idx, int32_t dst_x, int32_t dst_y) {
  if (btn_idx < 0 || btn_idx >= MAX_BUTTONS) return;
  lv_obj_t* btn = g_buttons[btn_idx];
  if (btn == nullptr) return;

  MoveAnim& m = g_move_anims[btn_idx];
  m.btn     = btn;
  m.start_x = static_cast<int16_t>(lv_obj_get_x_aligned(btn));
  m.start_y = static_cast<int16_t>(lv_obj_get_y_aligned(btn));
  m.end_x   = static_cast<int16_t>(dst_x);
  m.end_y   = static_cast<int16_t>(dst_y);

  if (m.start_x == m.end_x && m.start_y == m.end_y) return;

  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, &m);
  lv_anim_set_exec_cb(&a, anim_pos_exec_cb);
  lv_anim_set_time(&a, REFLOW_MS);
  lv_anim_set_values(&a, 0, 1000);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  lv_anim_start(&a);
}

inline void kill_pos_anim(int8_t btn_idx) {
  if (btn_idx < 0 || btn_idx >= MAX_BUTTONS) return;
  lv_anim_delete(&g_move_anims[btn_idx], anim_pos_exec_cb);
}

// Reflow : calcule la nouvelle disposition si le bouton saisi occupait
// target_cell (en gardant l'ordre relatif des autres), puis anime les
// voisins vers leurs nouvelles cellules. Les boutons pinned restent
// dans leur cellule. g_cell_of / g_button_at sont mis a jour.
inline void reflow_to(int8_t dragged_idx, int8_t target_cell) {
  if (target_cell == g_last_target_cell) return;
  if (target_cell < 0) return;
  g_last_target_cell = target_cell;

  int8_t new_order[MAX_BUTTONS];
  for (int8_t c = 0; c < g_count; ++c) new_order[c] = -1;

  // 1) cellules occupees par un bouton pinne : intouchables.
  for (int8_t c = 0; c < g_count; ++c) {
    int8_t occupant = g_button_at[c];
    if (occupant >= 0 && g_pinned[occupant]) new_order[c] = occupant;
  }

  // 2) bouton drague : va explicitement sur target_cell.
  new_order[target_cell] = dragged_idx;

  // 3) reste : parcours du snapshot, on remplit les slots encore vides
  //    dans l'ordre, en sautant drague + pinned.
  int8_t write_src = 0;
  for (int8_t c = 0; c < g_count; ++c) {
    if (new_order[c] != -1) continue;
    while (write_src < g_count) {
      int8_t cand = g_pre_drag_order[write_src];
      if (cand == dragged_idx || (cand >= 0 && g_pinned[cand])) {
        ++write_src;
        continue;
      }
      break;
    }
    if (write_src < g_count) new_order[c] = g_pre_drag_order[write_src++];
  }

  // Snapshot de g_cell_of avant l'application pour detecter les mouvements.
  int8_t old_cell_of[MAX_BUTTONS];
  for (int8_t i = 0; i < g_count; ++i) old_cell_of[i] = g_cell_of[i];

  // Applique la nouvelle disposition.
  for (int8_t c = 0; c < g_count; ++c) {
    int8_t b = new_order[c];
    if (b < 0 || b >= g_count) continue;
    g_button_at[c] = b;
    g_cell_of[b]   = c;
  }

  // Anime chaque voisin dont la cellule a change.
  for (int8_t i = 0; i < g_count; ++i) {
    if (i == dragged_idx) continue;
    if (g_cell_of[i] == old_cell_of[i]) continue;
    animate_btn_to(i, g_cells[g_cell_of[i]].x, g_cells[g_cell_of[i]].y);
  }
}

// Breathing : bob translate_y de +-2 px, 1000 ms A/R, infini.
// On evite volontairement transform_scale : l'unite PPA de l'ESP32-P4
// rejette les blits scales (voir lv_draw_ppa.c ppa_evaluate) et bascule
// en software scaling, tres couteux. Une translation pure reste dans
// le chemin rapide PPA (fill + image blit acceleres).
inline void breathe_exec_cb(void* var, int32_t v) {
  lv_obj_set_style_translate_y(
      static_cast<lv_obj_t*>(var), v, LV_PART_MAIN);
}

inline void start_breathe(lv_obj_t* btn, int8_t idx) {
  if (btn == nullptr) return;

  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, btn);
  lv_anim_set_exec_cb(&a, breathe_exec_cb);
  lv_anim_set_time(&a, 1000);
  lv_anim_set_playback_time(&a, 1000);
  lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
  lv_anim_set_values(&a, 0, -2);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
  lv_anim_set_delay(&a, static_cast<uint32_t>(idx) * 100u);
  lv_anim_start(&a);
}

inline void stop_breathe(lv_obj_t* btn) {
  if (btn == nullptr) return;
  lv_anim_delete(btn, breathe_exec_cb);
  lv_obj_set_style_translate_y(btn, 0, LV_PART_MAIN);
}

// Coupe le breathing sur tous les boutons : appele au debut d'un drag
// pour degager le pipeline PPA (translate_y sur N boutons en parallele
// coute un redraw chacun, pas necessaire pendant un drag).
inline void pause_all_breathe() {
  for (int8_t i = 0; i < g_count; ++i) {
    if (g_pinned[i]) continue;
    stop_breathe(g_buttons[i]);
  }
}

// Relance le breathing sur tous les boutons apres un drop si on est
// toujours en edit mode.
inline void resume_all_breathe() {
  for (int8_t i = 0; i < g_count; ++i) {
    if (g_pinned[i]) continue;
    start_breathe(g_buttons[i], i);
  }
}

inline void set_edit_mode(bool enabled) {
  g_edit_mode = enabled;
  g_last_click_idx = -1;   // reset double-click state au changement de mode
  for (int8_t i = 0; i < g_count; ++i) {
    lv_obj_t* btn = g_buttons[i];
    if (btn == nullptr) continue;
    if (g_pinned[i]) continue;  // les pinned ne respirent pas : pas draggables
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
      // Coupe TOUTES les anims de respiration pour liberer le pipeline
      // PPA pendant le drag + reflow (gain majeur sur ESP32-P4).
      pause_all_breathe();
      // Lift visuel du bouton saisi : translate_y pure, pas de scale,
      // donc reste dans le chemin rapide PPA.
      lv_obj_move_foreground(btn);
      lv_obj_set_style_translate_y(btn, -6, LV_PART_MAIN);
      // Annule toute anim de position en cours sur les voisins / sur le
      // bouton saisi (qui suit le doigt 1:1).
      for (int8_t i = 0; i < g_count; ++i) kill_pos_anim(i);
      // Snapshot de la sequence actuelle pour le reflow.
      for (int8_t c = 0; c < g_count; ++c) {
        g_pre_drag_order[c] = g_button_at[c];
      }
      g_last_target_cell = g_cell_of[idx];
      // Bloque le scroll du parent pendant le drag : sinon un mouvement
      // vers le bas fait descendre tout l'affichage (LVGL convertit le
      // geste en scroll sur l'ancetre scrollable).
      g_drag_parent = lv_obj_get_parent(btn);
      if (g_drag_parent != nullptr) {
        g_parent_was_scrollable =
            lv_obj_has_flag(g_drag_parent, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(g_drag_parent, LV_OBJ_FLAG_SCROLLABLE);
      }
    } else {
      // Applique le style `pressed:` du YAML (translate_y, bg_color,...)
      lv_obj_add_state(btn, LV_STATE_PRESSED);
    }
    return;
  }

  // --- PRESSING (drag + reflow en edit mode uniquement) -------------
  if (code == LV_EVENT_PRESSING) {
    if (!g_edit_mode) return;
    if (g_active != idx) return;
    lv_indev_t* indev = lv_indev_active();
    if (indev == nullptr) return;
    lv_point_t v;
    lv_indev_get_vect(indev, &v);
    if (v.x == 0 && v.y == 0) return;
    g_moved = true;
    // 1) le bouton saisi suit le doigt.
    int32_t nx = lv_obj_get_x_aligned(btn) + v.x;
    int32_t ny = lv_obj_get_y_aligned(btn) + v.y;
    lv_obj_set_pos(btn, nx, ny);
    // 2) reflow eventuel si le bouton saisi est desormais plus proche
    //    d'une autre case.
    int32_t cx = nx + g_cell_w / 2;
    int32_t cy = ny + g_cell_h / 2;
    int8_t  target = nearest_cell(cx, cy);
    if (target != g_last_target_cell) {
      reflow_to(idx, target);
    }
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

    // ---- EDIT MODE : fin du drag (reflow deja applique pdt PRESSING) -
    g_moved = false;
    g_long_fired = false;
    g_last_target_cell = -1;

    // Le bouton saisi se pose anime sur la case finale (celle deja
    // inscrite dans g_cell_of par le dernier reflow_to).
    const int8_t dst_cell = g_cell_of[idx];
    animate_btn_to(idx, g_cells[dst_cell].x, g_cells[dst_cell].y);

    // Restaure le scroll du parent si on l'avait desactive.
    if (g_drag_parent != nullptr && g_parent_was_scrollable) {
      lv_obj_add_flag(g_drag_parent, LV_OBJ_FLAG_SCROLLABLE);
    }
    g_drag_parent = nullptr;
    g_parent_was_scrollable = false;

    // Relance le breathing global apres le drop (si on est encore en
    // edit mode, ce qui est le cas par definition ici).
    resume_all_breathe();
  }
}

// Enregistre un bouton :
//  - draggable=true  : set_pos + overlay transparent enfant pour
//                      intercepter toutes les interactions (double-clic
//                      relay + drag en edit mode).
//  - draggable=false : set_pos uniquement. Le bouton garde son
//                      comportement YAML natif (on_press, on_long_press,
//                      etc.), il sert de slot "pinne" que le reflow
//                      ne touchera pas.
inline void attach(lv_obj_t* obj, int idx, int16_t cx, int16_t cy,
                   bool draggable = true) {
  if (obj == nullptr) return;
  if (idx < 0 || idx >= MAX_BUTTONS) return;

  g_cells[idx].x = cx;
  g_cells[idx].y = cy;
  g_buttons[idx] = obj;
  g_cell_of[idx] = idx;
  g_button_at[idx] = idx;
  g_pinned[idx]  = !draggable;
  if (idx + 1 > g_count) g_count = idx + 1;
  lv_obj_set_pos(obj, cx, cy);

  // Bouton pinne : on n'installe PAS d'overlay, on ne touche pas a
  // CLICKABLE. L'utilisateur garde on_press / on_long_press YAML natif.
  if (!draggable) {
    g_overlays[idx] = nullptr;
    return;
  }

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
  bool      draggable;
};

class DraggableGridComponent : public Component {
 public:
  void add(lv_obj_t* obj, int idx, int16_t x, int16_t y,
           bool draggable = true) {
    if (this->count_ >= ::draggable_grid::MAX_BUTTONS) return;
    this->entries_[this->count_++] =
        {obj, static_cast<int8_t>(idx), x, y, draggable};
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
        ::draggable_grid::attach(e.obj, e.idx, e.x, e.y, e.draggable);
      }
    }
  }

  float get_setup_priority() const override {
    return setup_priority::LATE;
  }

  // Declencheurs externes : permettent d'entrer / sortir du edit mode
  // depuis un bouton non-draggable ou depuis une autre source YAML.
  void toggle_edit_mode() { ::draggable_grid::toggle_edit_mode(); }
  void set_edit_mode(bool enabled) {
    ::draggable_grid::set_edit_mode(enabled);
  }

 private:
  PendingEntry entries_[::draggable_grid::MAX_BUTTONS]{};
  int8_t  count_ = 0;
  int16_t cell_w_ = 150;
  int16_t cell_h_ = 100;
};

// ------------------------------------------------------------
// Actions YAML (external_trigger) :
//   on_long_press:
//     - draggable_grid.toggle_edit_mode: my_grid_id
//     - draggable_grid.set_edit_mode:
//         id: my_grid_id
//         value: true
// ------------------------------------------------------------
template<typename... Ts>
class ToggleEditModeAction : public Action<Ts...> {
 public:
  explicit ToggleEditModeAction(DraggableGridComponent* parent)
      : parent_(parent) {}
  void play(Ts... /*x*/) override { this->parent_->toggle_edit_mode(); }

 private:
  DraggableGridComponent* parent_;
};

template<typename... Ts>
class SetEditModeAction : public Action<Ts...> {
 public:
  explicit SetEditModeAction(DraggableGridComponent* parent)
      : parent_(parent) {}
  TEMPLATABLE_VALUE(bool, value)
  void play(Ts... x) override {
    this->parent_->set_edit_mode(this->value_.value(x...));
  }

 private:
  DraggableGridComponent* parent_;
};

}  // namespace draggable_grid_cmpt
}  // namespace esphome
