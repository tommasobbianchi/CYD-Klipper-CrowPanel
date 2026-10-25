// panels/input_shaper.py: Auto Calibration (Measure X / Y / Both -> SHAPER_CALIBRATE), Manual Calibration (frequency slider +
// shaper type per axis -> SET_INPUT_SHAPER), status line with the last gcode response, SAVE_CONFIG after a calibration.
// Additions over the Python (which prints everything into one status line): a Results page with the fitted-shaper table
// and an explicit Save Config button.
#include <Arduino.h>
#include <ctype.h>
#include <math.h>
#include "ks_widgets.h"
#include "ks_mr.h"
#include "ks_panels.h"
#include "ks_calib_common.h"

static const char* const SHAPERS[] = {"zv", "mzv", "zvd", "ei", "2hump_ei", "3hump_ei"};
#define NSH 6

struct IS {
    lv_obj_t *root, *calib, *calib_lbl, *status;
    lv_obj_t *slider[2], *val[2], *tog[2];
    int type[2];                       // index into SHAPERS
    char calibrating;                  // 0, 'x', 'y' or 'b'(oth)
    bool busy;                         // calibration running
};
static IS* g_st;
static uint32_t g_gen;
static KcStore* g_store;

// fitted shaper table; kept across panel visits (cleared when a calibration starts)
struct Fit { char axis; char type[12]; float freq, vib, smooth; int max_accel; bool rec; };
static Fit g_fit[16];
static int g_nfit;
static char g_cur_axis;

static void set_calib(IS* s, const char* text, bool enabled) {
    lv_label_set_text(s->calib_lbl, text);
    if (enabled) lv_obj_clear_state(s->calib, LV_STATE_DISABLED); else lv_obj_add_state(s->calib, LV_STATE_DISABLED);
}

// ---- SET_INPUT_SHAPER from the manual controls ----------------------------------------------------------------------------
static void send_shaper() {
    IS* s = g_st; if (!s) return;
    ks_gcode("SET_INPUT_SHAPER SHAPER_FREQ_X=%.1f SHAPER_TYPE_X=%s SHAPER_FREQ_Y=%.1f SHAPER_TYPE_Y=%s",
             lv_slider_get_value(s->slider[0]) / 10.0, SHAPERS[s->type[0]],
             lv_slider_get_value(s->slider[1]) / 10.0, SHAPERS[s->type[1]]);
}
static void show_val(IS* s, int ax) {
    char b[16]; snprintf(b, sizeof b, "%.1f", lv_slider_get_value(s->slider[ax]) / 10.0);
    lv_label_set_text(s->val[ax], b);
}
static void on_slider_released(lv_event_t*) { send_shaper(); }
static void on_slider_changed(lv_event_t* e) { if (g_st) show_val(g_st, (int)(intptr_t)lv_event_get_user_data(e)); }
static void on_type(int idx, void* u) {
    IS* s = g_st; if (!s) return;
    s->type[(int)(intptr_t)u] = idx;
    send_shaper();
}
static void set_axis(IS* s, int ax, const char* type, float freq) {
    for (int i = 0; i < NSH; i++) if (!strcmp(SHAPERS[i], type)) { s->type[ax] = i; ks_toggles_select(s->tog[ax], i); }
    lv_slider_set_value(s->slider[ax], (int)(freq * 10 + 0.5f), LV_ANIM_OFF);
    show_val(s, ax);
}

// ---- save --------------------------------------------------------------------------------------------------------------------
static void save_ok(void*) { ks_gcode("SAVE_CONFIG"); }
static void ask_save() { ks_confirm("Save configuration?", "Klipper will reboot", "Save", save_ok, nullptr); }
static void on_save(lv_event_t*) { ask_save(); }

// ---- calibration menu (Measure X / Y / Both) ----------------------------------------------------------------------------------
static lv_obj_t* g_menu;
static void menu_pick(lv_event_t* e) {
    int m = (int)(intptr_t)lv_event_get_user_data(e);
    if (g_menu) { lv_obj_del(g_menu); g_menu = nullptr; }
    IS* s = g_st; if (!s) return;
    char homed[8] = "";
    ks_str("toolhead", "homed_axes", homed, sizeof homed);
    if (strcmp(homed, "xyz")) ks_gcode("G28");
    s->calibrating = m == 0 ? 'x' : m == 1 ? 'y' : 'b';
    g_nfit = 0;
    s->busy = true;
    if (m == 0) ks_gcode("SHAPER_CALIBRATE AXIS=X");
    else if (m == 1) ks_gcode("SHAPER_CALIBRATE AXIS=Y");
    else ks_gcode("SHAPER_CALIBRATE");
    set_calib(s, "Calibrating...", false);
}
static void menu_close(lv_event_t* e) { if (lv_event_get_target(e) == lv_event_get_current_target(e) && g_menu) { lv_obj_del(g_menu); g_menu = nullptr; } }
static void on_calib(lv_event_t*) {
    static const char* const L[] = {"Measure X", "Measure Y", "Measure Both"};
    g_menu = lv_obj_create(lv_layer_top());
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
    for (int i = 0; i < 3; i++) {
        lv_obj_t* b = ks_button_sized(card, nullptr, 0, L[i], 0, menu_pick, (void*)(intptr_t)i);
        lv_obj_set_size(b, lv_pct(100), 64);
        lv_obj_set_style_bg_color(b, KS_ACTIVE, 0);
    }
}

// ---- gcode responses ----------------------------------------------------------------------------------------------------------
static void on_msg(const char* msg, void*) {
    IS* s = g_st; if (!s) return;
    char low[256]; size_t n = 0;
    for (; msg[n] && n < sizeof low - 1; n++) low[n] = tolower((unsigned char)msg[n]);
    low[n] = 0;

    // status line: data.replace('shaper_', '').replace('damping_', '')
    char st[256]; size_t o = 0;
    for (size_t i = 0; msg[i] && o < sizeof st - 1;) {
        if (!strncmp(msg + i, "shaper_", 7)) i += 7;
        else if (!strncmp(msg + i, "damping_", 8)) i += 8;
        else st[o++] = msg[i++];
    }
    st[o] = 0;
    lv_label_set_text(s->status, st);

    if (strstr(low, "got 0")) set_calib(s, "Check ADXL Wiring", false);
    if (strstr(low, "unknown command:\"accelerometer_query\"")) set_calib(s, "ADXL Not Configured", false);
    if ((strstr(low, "adxl345 values") || strstr(low, "axes noise")) && !s->busy) set_calib(s, "Auto-calibrate", true);

    // result table
    const char* p;
    if ((p = strstr(low, "input shaper parameters for "))) g_cur_axis = toupper((unsigned char)p[28]);
    if ((p = strstr(low, "fitted shaper '")) && g_nfit < 16) {
        Fit f = {}; f.axis = g_cur_axis ? g_cur_axis : '?';
        if (sscanf(msg + (p - low) + 15, "%11[^']' frequency = %f Hz (vibrations = %f%%, smoothing ~= %f)", f.type, &f.freq, &f.vib, &f.smooth) == 4)
            g_fit[g_nfit++] = f;
    }
    if ((p = strstr(low, "suggested max_accel <= "))) {
        int acc = atoi(p + 23);
        const char* q = strstr(low, "smoothing with '");
        char t[12] = "";
        if (q) sscanf(msg + (q - low) + 16, "%11[^']", t);
        for (int i = g_nfit - 1; i >= 0; i--) if (g_fit[i].axis == g_cur_axis && !strcmp(g_fit[i].type, t)) { g_fit[i].max_accel = acc; break; }
    }

    // Recommended shaper_type_y = ei, shaper_freq_y = 48.4 Hz
    if ((p = strstr(low, "recommended shaper_type_"))) {
        char ax = 0, type[12] = ""; float freq = 0;
        if (sscanf(p, "recommended shaper_type_%c = %11[^,], shaper_freq_%*c = %f", &ax, type, &freq) == 3 && (ax == 'x' || ax == 'y')) {
            set_axis(s, ax == 'x' ? 0 : 1, type, freq);
            for (int i = 0; i < g_nfit; i++) if (g_fit[i].axis == toupper(ax)) g_fit[i].rec = !strcmp(g_fit[i].type, type) && fabsf(g_fit[i].freq - freq) < 0.06f;
            if (s->calibrating == ax || (s->calibrating == 'b' && ax == 'y')) {
                s->calibrating = 0;
                s->busy = false;
                set_calib(s, "Calibrated", true);
                ask_save();
            }
        }
    }
    // shaper_type_y:ei shaper_freq_y:48.400 damping_ratio_y:0.100000   (reply to a bare SET_INPUT_SHAPER)
    if ((p = strstr(low, "shaper_type_")) && !strstr(low, "recommended")) {
        char ax = 0, type[12] = ""; float freq = 0;
        if (sscanf(p, "shaper_type_%c:%11s shaper_freq_%*c:%f", &ax, type, &freq) == 3 && (ax == 'x' || ax == 'y'))
            set_axis(s, ax == 'x' ? 0 : 1, type, freq);
    }
}

static void on_cfg(JsonObjectConst cfg, bool ok, void*) {      // initial values until the SET_INPUT_SHAPER reply arrives
    IS* s = g_st; if (!s || !ok) return;
    for (int ax = 0; ax < 2; ax++) {
        char kt[16], kf[16]; snprintf(kt, sizeof kt, "shaper_type_%c", "xy"[ax]); snprintf(kf, sizeof kf, "shaper_freq_%c", "xy"[ax]);
        const char* t = kc_val(cfg, "input_shaper", kt);
        const char* f = kc_val(cfg, "input_shaper", kf);
        if (t && f) set_axis(s, ax, t, atof(f));
    }
}

// ---- results page --------------------------------------------------------------------------------------------------------------
static void results_init(lv_obj_t* c) {
    lv_obj_t* l = ks_list(c);
    if (!g_nfit) {
        lv_obj_t* t = ks_label(l, "No calibration result yet. Run Auto-calibrate first.", &ks_font_22);
        lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP); lv_obj_set_width(t, lv_pct(100));
        return;
    }
    for (int i = 0; i < g_nfit; i++) {
        Fit& f = g_fit[i];
        char a[56], b[96];
        snprintf(a, sizeof a, "%c  %s  %.1f Hz%s", f.axis, f.type, f.freq, f.rec ? "  *" : "");
        if (f.max_accel) snprintf(b, sizeof b, "vibrations %.1f%%, smoothing ~%.3f, max accel %d", f.vib, f.smooth, f.max_accel);
        else snprintf(b, sizeof b, "vibrations %.1f%%, smoothing ~%.3f", f.vib, f.smooth);
        lv_obj_t* r = ks_row(l, a, b);
        if (f.rec) lv_obj_set_style_border_color(r, ks_color(3), 0);
    }
}
static void on_results(lv_event_t*) { ks_open("Input Shaper results", results_init); }

// ---- panel ----------------------------------------------------------------------------------------------------------------------
static void panel_del(lv_event_t*) {
    g_gen++;
    kc_store_stop(g_store); g_store = nullptr;
    if (g_menu) { lv_obj_del(g_menu); g_menu = nullptr; }
    delete g_st; g_st = nullptr;
}

static void axis_rows(lv_obj_t* l, IS* s, int ax) {
    lv_obj_t* r = ks_row(l, ax ? "Y" : "X");
    lv_obj_t* box = ks_row_right(r, 320);
    s->val[ax] = ks_label(box, "0.0", &ks_font_22);
    lv_obj_set_width(s->val[ax], 62);
    s->slider[ax] = ks_slider(box, 0, 1330, 0, 240, on_slider_released, nullptr);
    lv_obj_add_event_cb(s->slider[ax], on_slider_changed, LV_EVENT_VALUE_CHANGED, (void*)(intptr_t)ax);
    s->tog[ax] = ks_toggles(l, SHAPERS, NSH, 0, on_type, (void*)(intptr_t)ax);
}

void ks_panel_input_shaper(lv_obj_t* c) {
    g_gen++;
    delete g_st;
    g_st = new IS();
    IS* s = g_st;
    memset(s, 0, sizeof *s);
    s->root = ks_card(c, lv_pct(100), lv_pct(100));
    lv_obj_add_event_cb(s->root, panel_del, LV_EVENT_DELETE, nullptr);
    lv_obj_t* l = ks_list(s->root);

    lv_obj_t* r = ks_row(l, "Auto Calibration");               // title + calibrate button
    s->calib = ks_button_sized(ks_row_right(r, 300), "move", 40, nullptr, 1, on_calib, nullptr);
    lv_obj_set_size(s->calib, 300, 64);
    lv_obj_set_flex_flow(s->calib, LV_FLEX_FLOW_ROW);
    s->calib_lbl = ks_label(s->calib, "Finding ADXL", &ks_font_22);
    lv_obj_add_state(s->calib, LV_STATE_DISABLED);

    ks_row(l, "Manual Calibration", "Applied at once; use Save Config to keep the values in printer.cfg.");
    axis_rows(l, s, 0);
    axis_rows(l, s, 1);

    lv_obj_t* bar = ks_card(l, lv_pct(100), 72);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t* b1 = ks_button_sized(bar, "complete", 40, "Save Config", 3, on_save, nullptr);
    lv_obj_t* b2 = ks_button_sized(bar, "info", 40, "Results", 4, on_results, nullptr);
    for (lv_obj_t* b : {b1, b2}) { lv_obj_set_size(b, 340, 66); lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW); lv_obj_set_style_pad_column(b, 8, 0); lv_obj_set_width(lv_obj_get_child(b, 1), LV_SIZE_CONTENT); }

    s->status = ks_label(l, "", &ks_font_18);
    lv_label_set_long_mode(s->status, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s->status, lv_pct(100));

    static const char* const secs[] = {"input_shaper"};
    kc_fetch(&g_gen, secs, 1, nullptr, 0, on_cfg, nullptr);
    g_store = kc_store_start(&g_gen, on_msg, nullptr, 1000);
    char ps[16] = "";
    ks_str("print_stats", "state", ps, sizeof ps);
    if (!strcmp(ps, "printing")) set_calib(s, "Busy (printing)", false);
    else {                                                              // activate(): current values, ADXL presence, noise test
        ks_gcode("SET_INPUT_SHAPER");
        ks_gcode("ACCELEROMETER_QUERY");
        ks_gcode("MEASURE_AXES_NOISE");
    }
}
