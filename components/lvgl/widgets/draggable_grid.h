#pragma once
//
// Draggable 3x3 grid helper for ESPHome LVGL 9.5.
//
// Option A implementation : absolute positioning, swap-on-drop.
// Tout l'etat est statique (inline variables), aucune allocation dynamique,
// aucune memoire retenue entre deux drags : g_active_slot est remis a -1
// IMMEDIATEMENT au debut de on_release(), avant toute autre logique.
//
// Utilisation depuis un YAML ESPHome :
//
//   esphome:
//     includes:
//       - draggable_grid.h
//
//     on_boot:
//       priority: -200
//       then:
//         - lambda: |-
//             using namespace draggable_grid;
//             register_button(id(btn_light)->obj,    0);
//             register_button(id(btn_alarm)->obj,    1);
//             register_button(id(btn_test3)->obj,    2);
//             register_button(id(btn_player)->obj,   3);
//             register_button(id(btn_camera)->obj,   4);
//             register_button(id(btn_test4)->obj,    5);
//             register_button(id(btn_settings)->obj, 6);
//             register_button(id(btn_test)->obj,     7);
//             register_button(id(btn_test2)->obj,    8);
//
// Et sur chaque bouton :
//
//   - button:
//       id: btn_light
//       x: 10
//       y: 220
//       width: 150
//       height: 100
//       on_press:
//         - lambda: draggable_grid::on_press(id(btn_light)->obj);
//       on_pressing:
//         - lambda: draggable_grid::on_pressing(id(btn_light)->obj);
//       on_release:
//         - lambda: draggable_grid::on_release(id(btn_light)->obj);
//

#include "lvgl.h"
#include <cstdint>
#include <climits>

namespace draggable_grid {

// ------------------------------------------------------------------
// Constantes de layout (correspondent a waveshare (3).yaml page_home)
// ------------------------------------------------------------------
constexpr int MAX_BUTTONS = 9;
constexpr int16_t BTN_W = 150;
constexpr int16_t BTN_H = 100;

struct Cell { int16_t x; int16_t y; };

// Grille 3 colonnes x 3 lignes
// cell 0..2 : ligne y=220
// cell 3..5 : ligne y=345
// cell 6..8 : ligne y=480
inline constexpr Cell CELLS[MAX_BUTTONS] = {
  { 10, 220}, {200, 220}, {380, 220},
  { 10, 345}, {200, 345}, {380, 345},
  { 10, 480}, {200, 480}, {380, 480},
};

// ------------------------------------------------------------------
// Etat global statique (inline variables = stockage unique, pas de heap)
// ------------------------------------------------------------------
struct Slot { lv_obj_t* obj; };
inline Slot   g_slots[MAX_BUTTONS]{};  // zero-initialise
inline int8_t g_active_slot = -1;      // -1 = idle, sinon index du slot en drag

// ------------------------------------------------------------------
// Helpers internes
// ------------------------------------------------------------------
inline int8_t find_slot(lv_obj_t* obj) {
  for (int8_t i = 0; i < MAX_BUTTONS; ++i) {
    if (g_slots[i].obj == obj) return i;
  }
  return -1;
}

inline int8_t nearest_cell(int32_t cx, int32_t cy) {
  int32_t best = INT32_MAX;
  int8_t  best_i = 0;
  for (int8_t i = 0; i < MAX_BUTTONS; ++i) {
    int32_t dx = cx - (CELLS[i].x + BTN_W / 2);
    int32_t dy = cy - (CELLS[i].y + BTN_H / 2);
    int32_t d  = dx * dx + dy * dy;
    if (d < best) { best = d; best_i = i; }
  }
  return best_i;
}

// ------------------------------------------------------------------
// API publique
// ------------------------------------------------------------------

// A appeler dans on_boot pour chaque bouton, dans l'ordre de la grille.
inline void register_button(lv_obj_t* obj, int cell_idx) {
  if (obj == nullptr) return;
  if (cell_idx < 0 || cell_idx >= MAX_BUTTONS) return;
  g_slots[cell_idx].obj = obj;
  lv_obj_set_pos(obj, CELLS[cell_idx].x, CELLS[cell_idx].y);
}

// on_press : memorise le slot actif et remonte le bouton au premier plan.
inline void on_press(lv_obj_t* obj) {
  int8_t s = find_slot(obj);
  if (s < 0) return;
  g_active_slot = s;
  lv_obj_move_foreground(obj);
}

// on_pressing : deplace le bouton avec le vecteur du dernier refresh.
// Zero allocation. Les variables lv_point_t et lv_indev_t* sont sur la pile.
inline void on_pressing(lv_obj_t* /*obj*/) {
  if (g_active_slot < 0) return;
  lv_indev_t* indev = lv_indev_active();
  if (indev == nullptr) return;
  lv_point_t v;
  lv_indev_get_vect(indev, &v);
  if (v.x == 0 && v.y == 0) return;
  lv_obj_t* o = g_slots[g_active_slot].obj;
  if (o == nullptr) return;
  int32_t nx = lv_obj_get_x_aligned(o) + v.x;
  int32_t ny = lv_obj_get_y_aligned(o) + v.y;
  lv_obj_set_pos(o, nx, ny);
}

// on_release : snap si pas de deplacement significatif, sinon swap avec
// le bouton occupant la case la plus proche. A la fin, TOUT l'etat
// transitoire est remis a zero avant de sortir -> RAM "videe".
inline void on_release(lv_obj_t* /*obj*/) {
  if (g_active_slot < 0) return;

  // 1) On libere immediatement le slot actif. Meme si une exception ou un
  //    early-return se produit plus bas, g_active_slot sera deja a -1.
  int8_t src = g_active_slot;
  g_active_slot = -1;

  lv_obj_t* o = g_slots[src].obj;
  if (o == nullptr) return;

  // 2) Calcul de la case cible (centre du bouton drague).
  int32_t cx = lv_obj_get_x_aligned(o) + BTN_W / 2;
  int32_t cy = lv_obj_get_y_aligned(o) + BTN_H / 2;
  int8_t  dst = nearest_cell(cx, cy);

  // 3a) Meme case : snap back a la position d'origine.
  if (dst == src) {
    lv_obj_set_pos(o, CELLS[src].x, CELLS[src].y);
    return;
  }

  // 3b) Swap : l'occupant de dst va en src, le drague va en dst.
  lv_obj_t* other = g_slots[dst].obj;
  if (other != nullptr) {
    lv_obj_set_pos(other, CELLS[src].x, CELLS[src].y);
  }
  lv_obj_set_pos(o, CELLS[dst].x, CELLS[dst].y);

  // 4) Mise a jour du registre (swap des pointeurs uniquement, pas d'alloc).
  g_slots[src].obj = other;
  g_slots[dst].obj = o;
}

// Reset complet (optionnel) - utile pour remettre la grille dans l'ordre
// initial depuis un bouton "Restore layout".
inline void reset_all() {
  g_active_slot = -1;
  // Les slots sont re-associes par register_button() appele a nouveau.
}

} // namespace draggable_grid
