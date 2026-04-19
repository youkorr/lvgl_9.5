/* =========================
 * DnD структуры данных
 * ========================= */
typedef struct {
    lv_obj_t *obj;
    lv_obj_t *content;
    lv_obj_t *drag_overlay;
    lv_obj_t *card_root;
    uint8_t id;
    uint8_t dashboard_index;
    uint8_t col;
    uint8_t row;
    uint8_t w_cells;
    uint8_t h_cells;
    ui_dash_card_type_t card_type;
    bool draggable;
} dash_tile_t;

typedef struct {
    dash_tile_t *tile;
    lv_point_t press_point;
    int32_t press_offset_x;
    int32_t press_offset_y;
    uint8_t orig_col;
    uint8_t orig_row;
    bool dragging;
} dash_drag_state_t;

typedef struct {
    uint8_t id;
    uint8_t col;
    uint8_t row;
} dash_tile_pos_t;

static int16_t s_grid[UI_DASHBOARD_MAX_PAGES][DASH_GRID_ROWS][DASH_GRID_COLS];
static dash_drag_state_t s_drag[UI_DASHBOARD_MAX_PAGES];

/* =========================
 * Вспомогательные функции сетки
 * ========================= */
static void dash_grid_clear(uint8_t dashboard_index)
{
    if (dashboard_index >= UI_DASHBOARD_MAX_PAGES) return;

    for (int r = 0; r < DASH_GRID_ROWS; r++) {
        for (int c = 0; c < DASH_GRID_COLS; c++) {
            s_grid[dashboard_index][r][c] = -1;
        }
    }
}

static void dash_grid_mark_tile(uint8_t dashboard_index, dash_tile_t *t, bool occupy)
{
    if (dashboard_index >= UI_DASHBOARD_MAX_PAGES) return;
    if (!t) return;

    for (int r = t->row; r < (int)(t->row + t->h_cells); r++) {
        for (int c = t->col; c < (int)(t->col + t->w_cells); c++) {
            if (r >= 0 && r < DASH_GRID_ROWS && c >= 0 && c < DASH_GRID_COLS) {
                s_grid[dashboard_index][r][c] = occupy ? t->id : -1;
            }
        }
    }
}

static bool dash_grid_area_inside(int32_t col, int32_t row, int32_t w_cells, int32_t h_cells)
{
    if (col < 0 || row < 0) return false;
    if (col + w_cells > DASH_GRID_COLS) return false;
    if (row + h_cells > DASH_GRID_ROWS) return false;
    return true;
}

static bool dash_grid_area_is_free(uint8_t dashboard_index,
                                   int32_t col,
                                   int32_t row,
                                   int32_t w_cells,
                                   int32_t h_cells)
{
    if (dashboard_index >= UI_DASHBOARD_MAX_PAGES) return false;
    if (!dash_grid_area_inside(col, row, w_cells, h_cells)) return false;

    for (int r = row; r < row + h_cells; r++) {
        for (int c = col; c < col + w_cells; c++) {
            if (s_grid[dashboard_index][r][c] != -1) return false;
        }
    }
    return true;
}

static void dash_px_to_grid_anchor(int32_t px_x, int32_t px_y, int32_t *col, int32_t *row)
{
    int32_t step_x = DASH_CELL_W + DASH_TILE_GAP;
    int32_t step_y = DASH_CELL_H + DASH_TILE_GAP;

    int32_t rel_x = px_x - DASH_GRID_AREA_X;
    int32_t rel_y = px_y - DASH_GRID_AREA_Y;

    if (rel_x < 0) rel_x = 0;
    if (rel_y < 0) rel_y = 0;

    *col = (rel_x + step_x / 2) / step_x;
    *row = (rel_y + step_y / 2) / step_y;
}

static void dash_clamp_tile_pos(const dash_tile_t *t, int32_t *col, int32_t *row)
{
    if (!t || !col || !row) return;

    if (*col < 0) *col = 0;
    if (*row < 0) *row = 0;

    if (*col + t->w_cells > DASH_GRID_COLS) *col = DASH_GRID_COLS - t->w_cells;
    if (*row + t->h_cells > DASH_GRID_ROWS) *row = DASH_GRID_ROWS - t->h_cells;

    if (*col < 0) *col = 0;
    if (*row < 0) *row = 0;
}

static int32_t dash_tile_area(const dash_tile_t *t)
{
    return t ? ((int32_t)t->w_cells * (int32_t)t->h_cells) : 0;
}

static void dash_save_all_positions(uint8_t dashboard_index, dash_tile_pos_t *out_pos)
{
    if (dashboard_index >= UI_DASHBOARD_MAX_PAGES || !out_pos) return;

    for (uint8_t i = 0; i < s_tile_count[dashboard_index]; i++) {
        out_pos[i].id  = s_tiles[dashboard_index][i].id;
        out_pos[i].col = s_tiles[dashboard_index][i].col;
        out_pos[i].row = s_tiles[dashboard_index][i].row;
    }
}

static void dash_restore_all_positions(uint8_t dashboard_index, const dash_tile_pos_t *saved)
{
    if (dashboard_index >= UI_DASHBOARD_MAX_PAGES || !saved) return;

    dash_grid_clear(dashboard_index);

    for (uint8_t i = 0; i < s_tile_count[dashboard_index]; i++) {
        s_tiles[dashboard_index][i].col = saved[i].col;
        s_tiles[dashboard_index][i].row = saved[i].row;
        dash_grid_mark_tile(dashboard_index, &s_tiles[dashboard_index][i], true);
    }
}

static void dash_apply_all_positions_ui(uint8_t dashboard_index, bool anim)
{
    if (dashboard_index >= UI_DASHBOARD_MAX_PAGES) return;

    for (uint8_t i = 0; i < s_tile_count[dashboard_index]; i++) {
        dash_grid_snap_tile(&s_tiles[dashboard_index][i], anim);
    }
}

static void dash_sort_tiles_for_packing(dash_tile_t **arr, int count)
{
    if (!arr || count <= 1) return;

    for (int i = 0; i < count - 1; i++) {
        for (int j = i + 1; j < count; j++) {
            bool swap = false;

            if (!arr[i] || !arr[j]) continue;

            if (arr[j]->row < arr[i]->row) swap = true;
            else if (arr[j]->row == arr[i]->row && arr[j]->col < arr[i]->col) swap = true;
            else if (arr[j]->row == arr[i]->row &&
                     arr[j]->col == arr[i]->col &&
                     dash_tile_area(arr[j]) > dash_tile_area(arr[i])) swap = true;

            if (swap) {
                dash_tile_t *tmp = arr[i];
                arr[i] = arr[j];
                arr[j] = tmp;
            }
        }
    }
}

static bool dash_find_first_free_position_for_tile(uint8_t dashboard_index,
                                                   const dash_tile_t *t,
                                                   int32_t *out_col,
                                                   int32_t *out_row)
{
    if (dashboard_index >= UI_DASHBOARD_MAX_PAGES) return false;
    if (!t || !out_col || !out_row) return false;

    for (int row = 0; row <= DASH_GRID_ROWS - t->h_cells; row++) {
        for (int col = 0; col <= DASH_GRID_COLS - t->w_cells; col++) {
            if (dash_grid_area_is_free(dashboard_index, col, row, t->w_cells, t->h_cells)) {
                *out_col = col;
                *out_row = row;
                return true;
            }
        }
    }
    return false;
}

/* =========================
 * Ключевая функция перестроения сетки (repack)
 * ========================= */
static bool dash_try_full_repack_move(uint8_t dashboard_index,
                                      dash_tile_t *dragged,
                                      int32_t target_col,
                                      int32_t target_row)
{
    if (dashboard_index >= UI_DASHBOARD_MAX_PAGES) return false;
    if (!dragged) return false;
    if (!dash_grid_area_inside(target_col, target_row, dragged->w_cells, dragged->h_cells)) return false;

    dash_tile_pos_t saved[DASH_GRID_MAX_TILES];
    dash_tile_t *others[DASH_GRID_MAX_TILES];
    int other_count = 0;

    dash_save_all_positions(dashboard_index, saved);

    for (uint8_t i = 0; i < s_tile_count[dashboard_index]; i++) {
        if (&s_tiles[dashboard_index][i] != dragged) {
            others[other_count++] = &s_tiles[dashboard_index][i];
        }
    }

    dash_sort_tiles_for_packing(others, other_count);

    dash_grid_clear(dashboard_index);

    dragged->col = (uint8_t)target_col;
    dragged->row = (uint8_t)target_row;
    dash_grid_mark_tile(dashboard_index, dragged, true);

    for (int i = 0; i < other_count; i++) {
        int32_t new_col = 0;
        int32_t new_row = 0;

        if (!dash_find_first_free_position_for_tile(dashboard_index, others[i], &new_col, &new_row)) {
            dash_restore_all_positions(dashboard_index, saved);
            return false;
        }

        others[i]->col = (uint8_t)new_col;
        others[i]->row = (uint8_t)new_row;
        dash_grid_mark_tile(dashboard_index, others[i], true);
    }

    return true;
}

static void dash_get_target_cell_from_pointer(uint8_t dashboard_index,
                                              dash_tile_t *t,
                                              lv_point_t *p,
                                              int32_t *target_col,
                                              int32_t *target_row)
{
    if (dashboard_index >= UI_DASHBOARD_MAX_PAGES) return;
    if (!t || !p || !target_col || !target_row) return;

    ui_dashboard_runtime_page_t *page = &s_pages[dashboard_index];
    if (!page->grid_root || !lv_obj_is_valid(page->grid_root)) return;

    lv_area_t grid_coords;
    lv_obj_get_coords(page->grid_root, &grid_coords);

    int32_t local_x = p->x - grid_coords.x1 - s_drag[dashboard_index].press_offset_x;
    int32_t local_y = p->y - grid_coords.y1 - s_drag[dashboard_index].press_offset_y;

    dash_px_to_grid_anchor(local_x, local_y, target_col, target_row);
    dash_clamp_tile_pos(t, target_col, target_row);
}

/* =========================
 * Обработчик событий перетаскивания
 * ========================= */
static void dash_drag_event_cb(lv_event_t *e)
{
    if (!s_edit_mode) return;

    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *obj = lv_event_get_target_obj(e);
    dash_tile_t *t = (dash_tile_t *)lv_event_get_user_data(e);

    if (!obj || !t) return;
    if (!dash_tile_is_draggable(t)) return;

    uint8_t dashboard_index = t->dashboard_index;
    if (dashboard_index >= UI_DASHBOARD_MAX_PAGES) return;

    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;

    lv_point_t p;
    lv_indev_get_point(indev, &p);

    if (code == LV_EVENT_PRESSED) {
        s_drag[dashboard_index].tile = t;
        s_drag[dashboard_index].dragging = true;
        s_drag[dashboard_index].press_point = p;
        s_drag[dashboard_index].orig_col = t->col;
        s_drag[dashboard_index].orig_row = t->row;

        lv_area_t obj_coords;
        lv_obj_get_coords(t->obj, &obj_coords);

        s_drag[dashboard_index].press_offset_x = p.x - obj_coords.x1;
        s_drag[dashboard_index].press_offset_y = p.y - obj_coords.y1;

        dash_grid_mark_tile(dashboard_index, t, false);
        lv_obj_move_foreground(t->obj);
        dash_tile_set_drag_style(t, true);
    }
    else if (code == LV_EVENT_PRESSING) {
        if (!s_drag[dashboard_index].dragging || s_drag[dashboard_index].tile != t) return;

        int32_t target_col = 0;
        int32_t target_row = 0;

        dash_get_target_cell_from_pointer(dashboard_index, t, &p, &target_col, &target_row);

        lv_obj_set_pos(t->obj,
                       dash_grid_col_to_x(target_col),
                       dash_grid_row_to_y(target_row));
    }
    else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if (!s_drag[dashboard_index].dragging || s_drag[dashboard_index].tile != t) return;

        int32_t target_col = 0;
        int32_t target_row = 0;
        bool placed = false;

        dash_get_target_cell_from_pointer(dashboard_index, t, &p, &target_col, &target_row);

        if (dash_try_full_repack_move(dashboard_index, t, target_col, target_row)) {
            dash_apply_all_positions_ui(dashboard_index, true);
            lv_obj_move_foreground(t->obj);
            placed = true;
        }

        if (!placed) {
            t->col = s_drag[dashboard_index].orig_col;
            t->row = s_drag[dashboard_index].orig_row;
            dash_grid_mark_tile(dashboard_index, t, true);
            dash_grid_snap_tile(t, true);
        }

        dash_tile_set_drag_style(t, false);

        s_drag[dashboard_index].tile = NULL;
        s_drag[dashboard_index].dragging = false;
    }
}

static void dash_drag_overlay_build(dash_tile_t *t)
{
    if (!t || !t->obj) return;

    t->drag_overlay = lv_obj_create(t->obj);
    lv_obj_set_size(t->drag_overlay, lv_pct(100), lv_pct(100));
    lv_obj_center(t->drag_overlay);
    lv_obj_add_flag(t->drag_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(t->drag_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(t->drag_overlay, LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_set_style_bg_opa(t->drag_overlay, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(t->drag_overlay, 0, 0);
    lv_obj_set_style_pad_all(t->drag_overlay, 0, 0);

    lv_obj_add_event_cb(t->drag_overlay, dash_drag_event_cb, LV_EVENT_PRESSED, t);
    lv_obj_add_event_cb(t->drag_overlay, dash_drag_event_cb, LV_EVENT_PRESSING, t);
    lv_obj_add_event_cb(t->drag_overlay, dash_drag_event_cb, LV_EVENT_RELEASED, t);
    lv_obj_add_event_cb(t->drag_overlay, dash_drag_event_cb, LV_EVENT_PRESS_LOST, t);

    lv_obj_add_flag(t->drag_overlay, LV_OBJ_FLAG_HIDDEN);
}