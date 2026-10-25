// panels/zcalibrate.py: manual Z offset calibration. Start (Endstop / Probe / Bed mesh / Delta chooser), Raise / Lower nozzle
// (TESTZ Z=+/-distance), Accept / Abort while manual_probe is active, live "Z:" position and Probe Offset Saved / New.
// The Sovol "Calibrate Zoffset" menu entry (_Delay_Calibrate, panel 'calibrateZoffset') lives in ks_menu.cpp, not here.
#include <Arduino.h>
#include <math.h>
#include "ks_widgets.h"
#include "ks_mr.h"
#include "ks_panels.h"
#include "ks_calib_common.h"

static const char* const DIST[] = {".01", ".05", ".1", ".5", "1", "5"};

enum Method { M_ENDSTOP, M_PROBE, M_MESH, M_DELTA, M_DELTA_MANUAL };
static const char* const M_LABEL[] = {"Endstop", "Probe", "Bed mesh", "Delta Automatic", "Delta Manual"};

struct ZC {
    lv_obj_t *root, *zpos, *zoffset, *saved_val, *offblk, *zpos_b, *zneg_b, *start, *accept, *abort_b;
    int dist;                                    // index into DIST (distances[-2] = "1")
    bool has_off; float z_offset;
    // from the printer config
    bool cfg_ok; float x_off, y_off, z_hop, speed, safe_x, safe_y; bool has_safe, has_hop, has_speed, delta, have_xy;
    float xmax, ymax;
    // from /printer/gcode/help
    bool help_ok; int fn[5]; int nfn;
    bool calibrating;
};
static ZC* g_st;
static uint32_t g_gen;
static KcStore* g_store;

static void flat(lv_obj_t* b, bool enabled, int color) {      // buttons_calibrating / buttons_not_calibrating
    if (enabled) { lv_obj_clear_state(b, LV_STATE_DISABLED); lv_obj_set_style_border_width(b, 6, 0); lv_obj_set_style_border_color(b, ks_color(color), 0); }
    else { lv_obj_add_state(b, LV_STATE_DISABLED); lv_obj_set_style_border_width(b, 0, 0); }
}
static void set_calibrating(ZC* s, bool on) {
    s->calibrating = on;
    flat(s->start, !on && s->cfg_ok && s->help_ok && s->nfn > 0, 3);
    flat(s->zpos_b, on, 4); flat(s->zneg_b, on, 1); flat(s->accept, on, 3); flat(s->abort_b, on, 2);
}

// ---- start -------------------------------------------------------------------------------------------------------------------
static void move_to_position(ZC* s) {
    float z_hop = s->has_hop ? s->z_hop : 5, speed = s->has_speed ? s->speed : 15;
    ks_gcode("G91\nG0 Z%g F%g", z_hop, speed * 60);
    if (ks_b("gcode_move", "absolute_coordinates", true)) ks_gcode("G90");
    if (s->has_safe) ks_gcode("G0 X%g Y%g F3000", s->safe_x, s->safe_y);
    else if (s->delta) ks_gcode("G0 X0 Y0 F3000");
    else if (s->have_xy) ks_gcode("G0 X%g Y%g F3000", s->xmax / 2 - s->x_off, s->ymax / 2 - s->y_off);   // middle of the bed, nozzle over the probe offset
}
static void start_method(int m) {
    ZC* s = g_st; if (!s) return;
    lv_obj_add_state(s->start, LV_STATE_DISABLED);
    char homed[8] = "";
    ks_str("toolhead", "homed_axes", homed, sizeof homed);
    if (strcmp(homed, "xyz")) ks_gcode("G28");
    ks_gcode("SET_GCODE_OFFSET Z=0");
    if (m == M_MESH) { ks_gcode("BED_MESH_CALIBRATE"); return; }
    ks_gcode("BED_MESH_CLEAR");
    if (m == M_PROBE) { move_to_position(s); ks_gcode("PROBE_CALIBRATE"); }
    else if (m == M_DELTA) ks_gcode("DELTA_CALIBRATE");
    else if (m == M_DELTA_MANUAL) ks_gcode("DELTA_CALIBRATE METHOD=manual");
    else if (m == M_ENDSTOP) ks_gcode("Z_ENDSTOP_CALIBRATE");
}
static lv_obj_t* g_menu;
static void menu_pick(lv_event_t* e) {
    int m = (int)(intptr_t)lv_event_get_user_data(e);
    if (g_menu) { lv_obj_del(g_menu); g_menu = nullptr; }
    start_method(m);
}
static void menu_close(lv_event_t* e) { if (lv_event_get_target(e) == lv_event_get_current_target(e) && g_menu) { lv_obj_del(g_menu); g_menu = nullptr; } }
static void on_start(lv_event_t*) {
    ZC* s = g_st; if (!s) return;
    if (s->nfn == 1) { start_method(s->fn[0]); return; }
    g_menu = lv_obj_create(lv_layer_top());                          // the Gtk.Popover with one button per method
    lv_obj_set_size(g_menu, 800, 480);
    lv_obj_set_style_bg_color(g_menu, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(g_menu, LV_OPA_50, 0);
    lv_obj_set_style_border_width(g_menu, 0, 0);
    lv_obj_set_style_radius(g_menu, 0, 0);
    lv_obj_clear_flag(g_menu, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(g_menu, menu_close, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* card = lv_obj_create(g_menu);
    lv_obj_set_size(card, 320, LV_SIZE_CONTENT);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_black(), 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_border_color(card, KS_ACTIVE, 0);
    lv_obj_set_style_radius(card, 0, 0);
    lv_obj_set_style_pad_all(card, 12, 0);
    lv_obj_set_style_pad_row(card, 8, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < s->nfn; i++) {
        lv_obj_t* b = ks_button_sized(card, nullptr, 0, M_LABEL[s->fn[i]], 0, menu_pick, (void*)(intptr_t)s->fn[i]);
        lv_obj_set_size(b, lv_pct(100), 64);
        lv_obj_set_style_bg_color(b, KS_ACTIVE, 0);
    }
}

// ---- buttons -------------------------------------------------------------------------------------------------------------------
static void on_move(lv_event_t* e) {
    ZC* s = g_st; if (!s) return;
    ks_gcode("TESTZ Z=%c%s", (char)(intptr_t)lv_event_get_user_data(e), DIST[s->dist]);
}
static void on_accept(lv_event_t*) { ks_gcode("ACCEPT"); }
static void on_abort(lv_event_t*) {
    ks_gcode("ABORT");
    if (g_st) set_calibrating(g_st, false);
    ks_back();                                                       // _menu_go_back()
}
static void on_dist(int idx, void*) { if (g_st) g_st->dist = idx; }

// ---- live ----------------------------------------------------------------------------------------------------------------------
static void on_state(lv_event_t*) {
    ZC* s = g_st; if (!s || !s->zpos) return;
    char homed[8] = "";
    ks_str("toolhead", "homed_axes", homed, sizeof homed);
    char b[24];
    if (strcmp(homed, "xyz")) lv_label_set_text(s->zpos, "Z: ?");
    else {
        float z = ks_fi("gcode_move", "gcode_position", 2);
        snprintf(b, sizeof b, "Z: %.3f", z);
        lv_label_set_text(s->zpos, b);
        if (s->has_off && s->zoffset) { snprintf(b, sizeof b, "%.3f", fabsf(z - s->z_offset)); lv_label_set_text(s->zoffset, b); }
    }
    bool active = ks_b("manual_probe", "is_active", false);
    if (active != s->calibrating || (!active && (lv_obj_has_state(s->start, LV_STATE_DISABLED) != !(s->cfg_ok && s->help_ok && s->nfn > 0))))
        set_calibrating(s, active);
}
static void on_msg(const char* msg, void*) {                         // process_update(notify_gcode_response)
    char low[200]; size_t i = 0;
    for (; msg[i] && i < sizeof low - 1; i++) low[i] = tolower((unsigned char)msg[i]);
    low[i] = 0;
    if (strstr(low, "out of range")) ks_toast(msg);
    else if (strstr(low, "fail") && strstr(low, "use testz")) ks_toast("Failed, adjust position first");
}

// ---- data ---------------------------------------------------------------------------------------------------------------------
static void on_help(bool ok, JsonDocument& doc, void* u) {
    ZC* s = g_st;
    bool stale = *(uint32_t*)u != g_gen;
    delete (uint32_t*)u;
    if (!s || stale) return;
    s->help_ok = ok;
    JsonObjectConst r = doc["result"];
    auto has = [&](const char* c) { return ok && !r[c].isNull(); };
    s->nfn = 0;
    bool probe = false;
    if (has("Z_ENDSTOP_CALIBRATE")) s->fn[s->nfn++] = M_ENDSTOP;
    if (has("PROBE_CALIBRATE")) { s->fn[s->nfn++] = M_PROBE; probe = true; }
    if (has("BED_MESH_CALIBRATE") && !probe) s->fn[s->nfn++] = M_MESH;      // manual bed mesh when there is no probe
    if (has("DELTA_CALIBRATE")) {
        if (probe) s->fn[s->nfn++] = M_DELTA;
        s->fn[s->nfn++] = M_DELTA_MANUAL;
    }
    set_calibrating(s, s->calibrating);
}
static void on_cfg(JsonObjectConst cfg, bool ok, void*) {
    ZC* s = g_st; if (!s) return;
    s->cfg_ok = ok;
    if (ok) {
        const char* v;
        if ((v = kc_val(cfg, "probe", "z_offset"))) { s->has_off = true; s->z_offset = atof(v); }
        s->x_off = kc_float(cfg, "probe", "x_offset", 0); s->y_off = kc_float(cfg, "probe", "y_offset", 0);
        if ((v = kc_val(cfg, "probe", "sample_retract_dist"))) { s->has_hop = true; s->z_hop = atof(v); }
        if ((v = kc_val(cfg, "probe", "speed"))) { s->has_speed = true; s->speed = atof(v); }
        if ((v = kc_val(cfg, "safe_z_home", "home_xy_position")) && sscanf(v, "%f , %f", &s->safe_x, &s->safe_y) == 2) {
            s->has_safe = true;
            if ((v = kc_val(cfg, "safe_z_home", "z_hop"))) { s->has_hop = true; s->z_hop = atof(v); }
            if ((v = kc_val(cfg, "safe_z_home", "z_hop_speed"))) { s->has_speed = true; s->speed = atof(v); }
        }
        const char* k = kc_val(cfg, "printer", "kinematics");
        s->delta = k && strstr(k, "delta");
        const char* xm = kc_val(cfg, "stepper_x", "position_max");
        const char* ym = kc_val(cfg, "stepper_y", "position_max");
        if (xm && ym) { s->have_xy = true; s->xmax = atof(xm); s->ymax = atof(ym); }
        // the Saved / New labels only exist when the probe has a z_offset
        if (s->has_off && s->root) {
            char b[16]; snprintf(b, sizeof b, "%.3f", s->z_offset);
            lv_label_set_text(s->saved_val, b);
            lv_obj_clear_flag(s->offblk, LV_OBJ_FLAG_HIDDEN);
        }
    }
    set_calibrating(s, s->calibrating);
}

static void panel_del(lv_event_t*) {
    g_gen++;
    kc_store_stop(g_store); g_store = nullptr;
    if (g_menu) { lv_obj_del(g_menu); g_menu = nullptr; }
    delete g_st; g_st = nullptr;
}

static lv_obj_t* cell(lv_obj_t*, lv_obj_t* w, int col, int row, int span = 1) {
    lv_obj_set_grid_cell(w, LV_GRID_ALIGN_STRETCH, col, span, LV_GRID_ALIGN_STRETCH, row, 1);
    return w;
}

void ks_panel_zcalibrate(lv_obj_t* c) {
    g_gen++;
    delete g_st;
    g_st = new ZC();
    ZC* s = g_st;
    memset(s, 0, sizeof *s);
    s->dist = 4;
    s->root = ks_card(c, lv_pct(100), lv_pct(100));
    lv_obj_add_event_cb(s->root, panel_del, LV_EVENT_DELETE, nullptr);

    static lv_coord_t cw[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
    static lv_coord_t rh[] = {150, 150, LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
    lv_obj_t* grid = ks_card(s->root, lv_pct(100), lv_pct(100));
    lv_obj_set_grid_dsc_array(grid, cw, rh);
    lv_obj_set_style_pad_gap(grid, 2, 0);

    s->zpos_b = cell(grid, ks_button_sized(grid, "z-farther", 56, "Raise Nozzle", 4, on_move, (void*)(intptr_t)'+'), 0, 0);
    s->zneg_b = cell(grid, ks_button_sized(grid, "z-closer", 56, "Lower Nozzle", 1, on_move, (void*)(intptr_t)'-'), 0, 1);
    s->start = cell(grid, ks_button_sized(grid, "resume", 56, "Start", 3, on_start, nullptr), 1, 0);
    s->accept = cell(grid, ks_button_sized(grid, "complete", 56, "Accept", 3, on_accept, nullptr), 2, 0);
    s->abort_b = cell(grid, ks_button_sized(grid, "cancel", 56, "Abort", 2, on_abort, nullptr), 2, 1);

    lv_obj_t* pos = cell(grid, ks_card(grid, lv_pct(100), lv_pct(100)), 1, 1);      // position / probe offset block
    lv_obj_set_flex_flow(pos, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(pos, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    s->zpos = ks_label(pos, "Z: ?", &ks_font_28);
    lv_obj_t* offblk = s->offblk = ks_card(pos, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(offblk, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(offblk, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    ks_label(offblk, "Probe Offset:", &ks_font_22);
    lv_obj_t* two = ks_card(offblk, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(two, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(two, LV_FLEX_ALIGN_SPACE_AROUND, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_t* saved = ks_card(two, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(saved, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(saved, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    ks_label(saved, "Saved", &ks_font_22);
    s->saved_val = ks_label(saved, "?", &ks_font_22);
    lv_obj_t* nw = ks_card(two, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(nw, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(nw, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    ks_label(nw, "New", &ks_font_22);
    s->zoffset = ks_label(nw, "?", &ks_font_22);
    lv_obj_add_flag(offblk, LV_OBJ_FLAG_HIDDEN);                      // shown by on_cfg when the probe has a z_offset

    lv_obj_t* dist = cell(grid, ks_card(grid, lv_pct(100), lv_pct(100)), 0, 2, 3);
    lv_obj_set_flex_flow(dist, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(dist, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t* ml = ks_label(dist, "Move Distance (mm)", &ks_font_22);
    lv_obj_set_style_text_align(ml, LV_TEXT_ALIGN_CENTER, 0);
    ks_toggles(dist, DIST, 6, s->dist, on_dist, nullptr);

    ks_on_state(s->root, on_state);
    set_calibrating(s, ks_b("manual_probe", "is_active", false));
    on_state(nullptr);
    ks_get_async("/printer/gcode/help", on_help, new uint32_t(g_gen));
    static const char* const secs[] = {"probe", "safe_z_home", "stepper_x", "stepper_y", "printer"};
    kc_fetch(&g_gen, secs, 5, nullptr, 0, on_cfg, nullptr);
    g_store = kc_store_start(&g_gen, on_msg, nullptr, 2000);
}
