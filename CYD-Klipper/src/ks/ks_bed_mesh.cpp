// panels/bed_mesh.py: profile list (load / save / remove), Add profile, Clear, Calibrate, and the BedMap widget.
// The Python 3D view is replaced by a colour heat map of the probed matrix (blue = low, red = high) with the values,
// min / max / range underneath.
#include <Arduino.h>
#include <math.h>
#include "ks_widgets.h"
#include "ks_mr.h"
#include "ks_panels.h"

#define MAXN 16
#define MAXP 16

struct BM {
    lv_obj_t *root, *map, *info, *list, *clear, *calib;
    float m[MAXN][MAXN]; int rows, cols;
    char view[40];                    // profile shown in the map ("" = none)
    char active[40];                  // bed_mesh.profile_name
    char names[MAXP][40]; int np;
    char sig[MAXP * 40 + 48];         // names + active: rebuild the list when it changes
    bool calibrating;
};
static BM* g_st;

// ---- data from the status document --------------------------------------------------------------------------------------
static void fill_matrix(BM* s, JsonArrayConst a) {
    s->rows = s->cols = 0;
    for (JsonArrayConst row : a) {
        if (s->rows >= MAXN) break;
        int c = 0;
        for (JsonVariantConst v : row) { if (c >= MAXN) break; s->m[s->rows][c++] = v.as<float>(); }
        if (c > s->cols) s->cols = c;
        if (c) s->rows++;
    }
}
static void snapshot(JsonObjectConst root, void* u) {
    BM* s = (BM*)u;
    JsonObjectConst bm = root["bed_mesh"];
    strlcpy(s->active, bm["profile_name"] | "", sizeof s->active);
    s->np = 0;
    for (JsonPairConst kv : bm["profiles"].as<JsonObjectConst>()) if (s->np < MAXP) strlcpy(s->names[s->np++], kv.key().c_str(), 40);
    for (int i = 1; i < s->np; i++)                              // "default" first, then alphabetical
        for (int j = i; j > 0; j--) {
            bool def_j = !strcmp(s->names[j], "default"), def_p = !strcmp(s->names[j - 1], "default");
            if (def_p || (!def_j && strcmp(s->names[j - 1], s->names[j]) <= 0)) break;
            char t[40]; strcpy(t, s->names[j]); strcpy(s->names[j], s->names[j - 1]); strcpy(s->names[j - 1], t);
        }
    snprintf(s->sig, sizeof s->sig, "%s|", s->active);
    for (int i = 0; i < s->np; i++) { strlcat(s->sig, s->names[i], sizeof s->sig); strlcat(s->sig, ",", sizeof s->sig); }
}
static void load_view(JsonObjectConst root, void* u) {
    BM* s = (BM*)u;
    JsonObjectConst bm = root["bed_mesh"];
    s->rows = s->cols = 0;
    if (!s->view[0]) return;
    if (!strcmp(s->view, s->active)) fill_matrix(s, bm["probed_matrix"].as<JsonArrayConst>());
    else fill_matrix(s, bm["profiles"][s->view]["points"].as<JsonArrayConst>());
}

// ---- heat map -----------------------------------------------------------------------------------------------------------
static void draw_map(lv_event_t* e) {
    BM* s = (BM*)lv_event_get_user_data(e);
    if (!s || s->rows == 0) return;
    lv_draw_ctx_t* ctx = lv_event_get_draw_ctx(e);
    lv_obj_t* o = lv_event_get_target(e);
    lv_area_t a; lv_obj_get_coords(o, &a);
    int W = lv_area_get_width(&a), H = lv_area_get_height(&a);
    float mn = 1e9f, mx = -1e9f;
    for (int r = 0; r < s->rows; r++) for (int c = 0; c < s->cols; c++) { mn = fminf(mn, s->m[r][c]); mx = fmaxf(mx, s->m[r][c]); }
    float span = mx - mn;
    int cw = W / s->cols, ch = H / s->rows;
    int ox = a.x1 + (W - cw * s->cols) / 2, oy = a.y1 + (H - ch * s->rows) / 2;
    lv_draw_rect_dsc_t rd; lv_draw_rect_dsc_init(&rd);
    rd.border_width = 1; rd.border_color = KS_BG;
    lv_draw_label_dsc_t ld; lv_draw_label_dsc_init(&ld);
    ld.font = &ks_font_18; ld.align = LV_TEXT_ALIGN_CENTER;
    int lh = ks_font_18.line_height;
    for (int r = 0; r < s->rows; r++) {
        for (int c = 0; c < s->cols; c++) {
            float v = s->m[r][c];
            float t = span > 1e-6f ? (v - mn) / span : 0.5f;
            rd.bg_color = lv_color_hsv_to_rgb((uint16_t)(240 * (1 - t)), 85, 85);
            lv_area_t ca;
            ca.x1 = ox + c * cw; ca.x2 = ca.x1 + cw - 1;
            ca.y2 = oy + (s->rows - r) * ch - 1; ca.y1 = ca.y2 - ch + 1;       // row 0 = front of the bed = bottom
            lv_draw_rect(ctx, &rd, &ca);
            char b[12];
            if (cw >= 56) snprintf(b, sizeof b, "%.3f", v);
            else if (cw >= 40) snprintf(b, sizeof b, "%.2f", v);
            else continue;
            if (ch < lh) continue;
            if (b[0] == '0') memmove(b, b + 1, strlen(b));                      // 0.12 -> .12
            else if (b[0] == '-' && b[1] == '0') memmove(b + 1, b + 2, strlen(b + 1));
            ld.color = (t > 0.28f && t < 0.78f) ? lv_color_black() : lv_color_white();
            lv_area_t ta = ca; ta.y1 += (ch - lh) / 2;
            lv_draw_label(ctx, &ld, &ta, b, nullptr);
        }
    }
}
static void refresh_map(BM* s) {
    ks_with_status(load_view, s);
    char b[96];
    if (!s->rows) snprintf(b, sizeof b, "No mesh");
    else {
        float mn = 1e9f, mx = -1e9f;
        for (int r = 0; r < s->rows; r++) for (int c = 0; c < s->cols; c++) { mn = fminf(mn, s->m[r][c]); mx = fmaxf(mx, s->m[r][c]); }
        snprintf(b, sizeof b, "%s  min %.3f  max %.3f  range %.3f", s->view, mn, mx, mx - mn);
    }
    lv_label_set_text(s->info, b);
    lv_obj_invalidate(s->map);
}

// ---- actions ------------------------------------------------------------------------------------------------------------
static void sel_profile(lv_event_t* e) {
    BM* s = g_st; if (!s) return;
    const char* n = s->names[(intptr_t)lv_event_get_user_data(e)];
    ks_gcode("BED_MESH_PROFILE LOAD='%s'", n);
    strlcpy(s->view, n, sizeof s->view);
    refresh_map(s);
}
static void save_profile(lv_event_t* e) {
    BM* s = g_st; if (!s) return;
    ks_gcode("BED_MESH_PROFILE SAVE='%s'", s->names[(intptr_t)lv_event_get_user_data(e)]);
}
static char g_del[40];
static void remove_ok(void*) { ks_gcode("BED_MESH_PROFILE REMOVE='%s'", g_del); }
static void remove_profile(lv_event_t* e) {
    BM* s = g_st; if (!s) return;
    const char* n = s->names[(intptr_t)lv_event_get_user_data(e)];
    strlcpy(g_del, n, sizeof g_del);
    ks_confirm("Delete bed mesh profile?", n, "Delete", remove_ok, nullptr);
}
static void clear_mesh(lv_event_t*) { ks_gcode("BED_MESH_CLEAR"); }

static void do_calibrate() {
    ks_toast("Calibrating");
    char homed[8] = "";
    ks_str("toolhead", "homed_axes", homed, sizeof homed);
    if (strcmp(homed, "xyz")) ks_gcode("G28");
    ks_gcode("BED_MESH_CALIBRATE");
    // no probe: BED_MESH_CALIBRATE is a manual mesh, driven from the Z calibrate panel
    if (!ks_has("probe")) ks_open("Mesh calibrate", ks_panel_zcalibrate);
}
static void calibrate(lv_event_t* e) {
    lv_obj_add_state(lv_event_get_target(e), LV_STATE_DISABLED);       // like the Python: stays insensitive until the panel is reopened
    do_calibrate();
}

// ---- "Add profile" page ---------------------------------------------------------------------------------------------------
static lv_obj_t* g_name_ta;
static void create_profile(lv_event_t*) {
    if (!g_name_ta) return;
    char name[40]; strlcpy(name, lv_textarea_get_text(g_name_ta), sizeof name);
    for (char* p = name; *p; p++) if (*p == ' ' || *p == '\'' || *p == '"') *p = '_';
    if (!name[0]) { ks_toast("Enter a profile name"); return; }
    if (g_st && !g_st->active[0]) do_calibrate();                          // no active mesh: probe one first
    ks_gcode("BED_MESH_PROFILE SAVE=%s", name);
    ks_keyboard_hide();
    ks_back();
}
static void create_init(lv_obj_t* c) {
    lv_obj_t* box = ks_card(c, lv_pct(100), lv_pct(100));
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(box, 14, 0);
    lv_obj_set_style_pad_top(box, 60, 0);
    ks_label(box, "Profile Name:", &ks_font_28);
    lv_obj_t* row = ks_card(box, 640, 72);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    g_name_ta = ks_textarea(row, "", 400);
    lv_obj_set_height(g_name_ta, 60);
    lv_obj_add_event_cb(g_name_ta, [](lv_event_t*) { g_name_ta = nullptr; }, LV_EVENT_DELETE, nullptr);
    lv_obj_t* save = ks_button_sized(row, "complete", 40, "Save", 3, create_profile, nullptr);
    lv_obj_set_size(save, 200, 70);
}
static void show_create(lv_event_t*) { ks_open("Add profile", create_init); }

// ---- profile list -----------------------------------------------------------------------------------------------------------
static void rebuild_list(BM* s) {
    lv_obj_clean(s->list);
    for (int i = 0; i < s->np; i++) {
        bool act = !strcmp(s->names[i], s->active);
        lv_obj_t* r = ks_card(s->list, lv_pct(100), 64);                    // .frame-item
        lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(r, KS_BG, 0);
        lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(r, 4, 0);
        lv_obj_t* nb = ks_button_sized(r, nullptr, 0, nullptr, 0, sel_profile, (void*)(intptr_t)i);
        lv_obj_set_height(nb, 60); lv_obj_set_flex_grow(nb, 1);
        lv_obj_set_style_bg_color(nb, act ? KS_ACTIVE : KS_BG, 0);
        lv_obj_t* l = ks_label(nb, s->names[i], &ks_font_22);
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT); lv_obj_set_width(l, lv_pct(100));
        if (act) lv_obj_add_state(nb, LV_STATE_DISABLED);                    // set_sensitive(False) + button_active
        if (strcmp(s->names[i], "default")) {
            lv_obj_t* sv = ks_button_sized(r, "complete", 40, nullptr, 4, save_profile, (void*)(intptr_t)i);
            lv_obj_set_size(sv, 60, 60);
        }
        lv_obj_t* rm = ks_button_sized(r, "cancel", 40, nullptr, 2, remove_profile, (void*)(intptr_t)i);
        lv_obj_set_size(rm, 60, 60);
    }
}

static void on_state(lv_event_t*) {
    BM* s = g_st;
    if (!s || !s->list) return;
    char old_active[40]; strlcpy(old_active, s->active, sizeof old_active);
    char old_sig[sizeof s->sig]; strlcpy(old_sig, s->sig, sizeof old_sig);
    ks_with_status(snapshot, s);
    if (strcmp(old_sig, s->sig)) rebuild_list(s);
    if (strcmp(old_active, s->active)) strlcpy(s->view, s->active, sizeof s->view);     // activate_mesh(): graph follows the active profile
    refresh_map(s);
    if (s->active[0]) lv_obj_clear_state(s->clear, LV_STATE_DISABLED); else lv_obj_add_state(s->clear, LV_STATE_DISABLED);
}

static void panel_del(lv_event_t*) { delete g_st; g_st = nullptr; }

static lv_obj_t* top_btn(lv_obj_t* parent, const char* icon, const char* text, int color, lv_event_cb_t cb) {
    lv_obj_t* b = ks_button_sized(parent, icon, 40, text, color, cb, nullptr);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(b, 8, 0);
    lv_obj_set_size(b, 232, 64);
    lv_obj_set_width(lv_obj_get_child(b, 1), LV_SIZE_CONTENT);
    return b;
}

void ks_panel_bed_mesh(lv_obj_t* c) {
    delete g_st;
    g_st = new BM();
    BM* s = g_st;
    memset(s, 0, sizeof *s);
    s->root = ks_card(c, lv_pct(100), lv_pct(100));
    lv_obj_add_event_cb(s->root, panel_del, LV_EVENT_DELETE, nullptr);
    lv_obj_set_flex_flow(s->root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s->root, 4, 0);

    lv_obj_t* top = ks_card(s->root, lv_pct(100), 64);                      // topbar: Add profile, Clear, Calibrate
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    top_btn(top, "increase", " Add profile", 1, show_create);
    s->clear = top_btn(top, "cancel", " Clear", 2, clear_mesh);
    s->calib = top_btn(top, "refresh", " Calibrate", 3, calibrate);

    lv_obj_t* body = ks_card(s->root, lv_pct(100), 344);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(body, 6, 0);
    lv_obj_t* left = ks_card(body, 396, lv_pct(100));
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    s->map = ks_card(left, 396, 300);
    lv_obj_set_style_bg_opa(s->map, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s->map, KS_BG, 0);
    lv_obj_add_event_cb(s->map, draw_map, LV_EVENT_DRAW_MAIN_END, s);
    s->info = ks_label(left, "", &ks_font_18);
    lv_obj_set_width(s->info, 396);
    lv_obj_set_style_text_align(s->info, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s->info, LV_LABEL_LONG_DOT);
    s->list = ks_list(body);
    lv_obj_set_size(s->list, 306, lv_pct(100));

    ks_on_state(s->root, on_state);
    // first fill: the view starts on the active profile
    ks_with_status(snapshot, s);
    strlcpy(s->view, s->active, sizeof s->view);
    s->sig[0] = 0;
    on_state(nullptr);
}
