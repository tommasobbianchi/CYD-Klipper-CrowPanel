// panels/move.py: 4x2 jog grid (Home, Y+, Disable Motors, Z+ / X-, Y-, X+, Z-), position readout, distance toggles,
// and an options page (invert axes, XY / Z speed). Reference implementation for the other panels.
#include <Arduino.h>
#include "ks_widgets.h"
#include "ks_mr.h"
#include "ks_cfg.h"
#include "ks_panels.h"

static const char* const DISTANCES[] = {".1", ".5", "1", "5", "10", "25", "50"};
static int g_dist = 5;                                  // distances[-2] = 25 mm, like Sovol
static lv_obj_t *g_pos[3];

static void jog(lv_event_t* e) {
    char axis = (char)((intptr_t)lv_event_get_user_data(e) >> 8);
    int dir = ((intptr_t)lv_event_get_user_data(e) & 0xFF) == '+' ? 1 : -1;
    char key[12]; snprintf(key, sizeof key, "invert_%c", axis + 32);
    if (ks_cfg_bool(key, false)) dir = -dir;
    int speed = ks_cfg_int(axis == 'Z' ? "move_speed_z" : "move_speed_xy", axis == 'Z' ? 10 : 50);
    ks_gcode("G91\nG0 %c%c%s F%d", axis, dir > 0 ? '+' : '-', DISTANCES[g_dist], 60 * (speed < 1 ? 1 : speed));
    if (ks_b("gcode_move", "absolute_coordinates", true)) ks_gcode("G90");
}
static void home(lv_event_t*) { ks_gcode("G28"); }
static void motors_off_ok(void*) { ks_gcode("M18"); }
static void motors_off(lv_event_t*) { ks_confirm("Are you sure you wish to disable motors?", "", "Disable", motors_off_ok, nullptr); }
static void on_distance(int idx, void*) { g_dist = idx; }

static void on_state(lv_event_t*) {                      // process_update(): show gcode_position, '?' for axes not homed
    char homed[8] = "";
    ks_str("toolhead", "homed_axes", homed, sizeof homed);
    for (int i = 0; i < 3; i++) {
        char b[24];
        if (strchr(homed, "xyz"[i])) snprintf(b, sizeof b, "%c: %.2f", "XYZ"[i], ks_fi("gcode_move", "gcode_position", i));
        else snprintf(b, sizeof b, "%c: ?", "XYZ"[i]);
        lv_label_set_text(g_pos[i], b);
    }
}

// ---- options page ---------------------------------------------------------------------------------------------------------------
static lv_obj_t* invert_row(lv_obj_t* list, const char* name, const char* key) {
    lv_obj_t* r = ks_row(list, name);
    ks_switch(ks_row_right(r, 120), ks_cfg_bool(key, false), [](lv_event_t* e) {
        ks_cfg_set_bool((const char*)lv_event_get_user_data(e), lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
    }, (void*)key);
    return r;
}
static void speed_row(lv_obj_t* list, const char* name, const char* key, int def, int max) {
    lv_obj_t* r = ks_row(list, name);
    lv_obj_t* box = ks_row_right(r, 340);
    lv_obj_t* val = ks_label(box, "", &ks_font_22);
    lv_obj_set_width(val, 50);
    char b[8]; snprintf(b, sizeof b, "%d", ks_cfg_int(key, def)); lv_label_set_text(val, b);
    lv_obj_t* s = ks_slider(box, 1, max, ks_cfg_int(key, def), 270, [](lv_event_t* e) {
        lv_obj_t* sl = lv_event_get_target(e);
        ks_cfg_set_int((const char*)lv_event_get_user_data(e), lv_slider_get_value(sl));
    }, (void*)key);
    lv_obj_add_event_cb(s, [](lv_event_t* e) {            // live value while dragging
        char t[8]; snprintf(t, sizeof t, "%d", (int)lv_slider_get_value(lv_event_get_target(e)));
        lv_label_set_text((lv_obj_t*)lv_event_get_user_data(e), t);
    }, LV_EVENT_VALUE_CHANGED, val);
}
static void options_init(lv_obj_t* c) {
    lv_obj_t* l = ks_list(c);
    int vmax = (int)ks_f("toolhead", "max_velocity", 300); if (vmax < 10) vmax = 300;
    int zmax = (int)ks_f("toolhead", "max_z_velocity", 20); if (zmax < 5) zmax = 20;
    invert_row(l, "Invert X", "invert_x");
    invert_row(l, "Invert Y", "invert_y");
    invert_row(l, "Invert Z", "invert_z");
    speed_row(l, "XY Speed (mm/s)", "move_speed_xy", 50, vmax);
    speed_row(l, "Z Speed (mm/s)", "move_speed_z", 10, zmax);
}
static void open_options(lv_event_t*) { ks_open("Settings", options_init); }

// ---- main page -------------------------------------------------------------------------------------------------------------------
static lv_obj_t* jog_btn(lv_obj_t* grid, const char* icon, const char* label, int color, lv_event_cb_t cb, void* user, int col, int row) {
    lv_obj_t* b = ks_button_sized(grid, icon, 56, label, color, cb, user);
    lv_obj_set_grid_cell(b, LV_GRID_ALIGN_STRETCH, col, 1, LV_GRID_ALIGN_STRETCH, row, 1);
    return b;
}
#define AX(a, d) ((void*)(intptr_t)(((a) << 8) | (d)))

void ks_panel_move(lv_obj_t* c) {
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(c, 2, 0);
    lv_obj_set_style_pad_row(c, 2, 0);

    lv_obj_t* grid = ks_card(c, lv_pct(100), 246);                       // buttons: 4 columns x 2 rows
    static lv_coord_t cw[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
    static lv_coord_t rh[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
    lv_obj_set_grid_dsc_array(grid, cw, rh);
    lv_obj_set_style_pad_gap(grid, 2, 0);
    jog_btn(grid, "home", "Home", 4, home, nullptr, 0, 0);
    jog_btn(grid, "arrow-up", "Y+", 2, jog, AX('Y', '+'), 1, 0);
    jog_btn(grid, "motor-off", "Disable Motors", 4, motors_off, nullptr, 2, 0);
    jog_btn(grid, "z-farther", "Z+", 3, jog, AX('Z', '+'), 3, 0);
    jog_btn(grid, "arrow-left", "X-", 1, jog, AX('X', '-'), 0, 1);
    jog_btn(grid, "arrow-down", "Y-", 2, jog, AX('Y', '-'), 1, 1);
    jog_btn(grid, "arrow-right", "X+", 1, jog, AX('X', '+'), 2, 1);
    jog_btn(grid, "z-closer", "Z-", 3, jog, AX('Z', '-'), 3, 1);

    lv_obj_t* info = ks_card(c, lv_pct(100), 84);                         // positions + "Move Distance (mm)" + options button
    lv_obj_t* cols = ks_card(info, 600, lv_pct(100));
    lv_obj_set_flex_flow(cols, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(cols, LV_FLEX_ALIGN_SPACE_AROUND, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    for (int i = 0; i < 3; i++) {
        g_pos[i] = ks_label(cols, "", &ks_font_28);
        lv_obj_set_width(g_pos[i], 190);
        lv_obj_set_style_text_align(g_pos[i], LV_TEXT_ALIGN_CENTER, 0);
    }
    lv_obj_t* md = ks_label(cols, "Move Distance (mm)", &ks_font_22);
    lv_obj_set_width(md, 590);
    lv_obj_set_style_text_align(md, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t* opt = ks_button_sized(info, "settings", 40, nullptr, 2, open_options, nullptr);
    lv_obj_set_size(opt, 100, 80);
    lv_obj_align(opt, LV_ALIGN_RIGHT_MID, 0, 0);

    ks_toggles(c, DISTANCES, 7, g_dist, on_distance, nullptr);
    ks_on_state(info, on_state);
    on_state(nullptr);
}
