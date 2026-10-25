// panels/bed_level.py: manual bed levelling. Screw buttons (corners / edges / centre) move the nozzle to each screw;
// "Screws Adjust" runs SCREWS_TILT_CALCULATE and the results (CW/CCW turns, Reference, done tick) replace the button label+icon.
#include <Arduino.h>
#include <math.h>
#include "ks_widgets.h"
#include "ks_mr.h"
#include "ks_cfg.h"
#include "ks_panels.h"
#include "icons.h"
#include "ks_calib_common.h"

enum { P_BL, P_BM, P_BR, P_LM, P_RM, P_FL, P_FM, P_FR, P_CENTER, P_N };
static const char* const KEYS[P_N] = {"bl", "bm", "br", "lm", "rm", "fl", "fm", "fr", "center"};
// bed-level icon of each button (button_scale = 2 -> 72 px; the centre "increase" button is half size)
static const char* const ICONS[P_N] = {"bed-level-t-l", "bed-level-t-m", "bed-level-t-r", "bed-level-l-m", "bed-level-r-m",
                                        "bed-level-b-l", "bed-level-b-m", "bed-level-b-r", "increase"};
// grid cell (col,row) of each button inside the 3x3 bed grid
static const int8_t CELL[P_N][2] = {{0, 0}, {1, 0}, {2, 0}, {0, 1}, {2, 1}, {0, 2}, {1, 2}, {2, 2}, {1, 1}};
// screw_rotation: which physical screw each button goes to (button order P_BL,P_BM,P_BR,P_LM,P_RM,P_FL,P_FM,P_FR)
static const int8_t ROT[4][8] = {
    {P_BL, P_BM, P_BR, P_LM, P_RM, P_FL, P_FM, P_FR},       //   0
    {P_FL, P_LM, P_BL, P_FM, P_BM, P_FR, P_RM, P_BR},       //  90
    {P_FR, P_FM, P_FL, P_RM, P_LM, P_BR, P_BM, P_BL},       // 180
    {P_BR, P_RM, P_FR, P_BM, P_FM, P_BL, P_LM, P_FL},       // 270
};

struct Screw { char name[24]; float x, y; };      // x,y already include the probe offset for screws_tilt_adjust
struct BtnInfo { lv_obj_t *b, *img, *lbl; bool shown; int target; };   // target: index into found[] this button drives

struct BL_St {
    lv_obj_t* root;
    Screw screws[16]; int n;
    float xo, yo, lift_speed, hmz, hspeed, pzh;
    bool z_tilt;
    float found[P_N][2]; bool has[P_N];     // find_closest() result per position
    BtnInfo btn[P_N];
    lv_obj_t *dm, *screws_btn;
};
static uint32_t g_gen;
static BL_St* g_st;

static bool find_closest(BL_St* s, bool* used, float px, float py, float maxd, bool remove, int* out) {
    int best = -1; float bd = 1e9f;
    for (int i = 0; i < s->n; i++) {
        if (used[i]) continue;
        float d = hypotf(s->screws[i].x - px, s->screws[i].y - py);
        if (d < bd) { bd = d; best = i; }
    }
    if (best < 0 || bd > maxd) return false;
    if (remove) used[best] = true;
    *out = best;
    return true;
}

// ---- actions ------------------------------------------------------------------------------------------------------------
static void home_if_needed() {
    char homed[8] = "";
    ks_str("toolhead", "homed_axes", homed, sizeof homed);
    if (strcmp(homed, "xyz")) {
        ks_gcode("G28");
        if (g_st && g_st->z_tilt) ks_gcode("Z_TILT_ADJUST");
    }
}
static void go_to(lv_event_t* e) {
    BL_St* s = g_st;
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    if (!s || !s->has[s->btn[k].target]) return;
    float* p = s->found[s->btn[k].target];
    home_if_needed();
    ks_gcode("G90\nG1 Z%g F%g\nG1 X%g Y%g F%g\nG1 Z%g F%g", s->hmz, s->lift_speed * 60, p[0], p[1], s->hspeed * 60,
             s->pzh, s->lift_speed * 60);
}
static void motors_off_ok(void*) { ks_gcode("M18"); }
static void disable_motors(lv_event_t*) { ks_confirm("Are you sure you wish to disable motors?", "", "Disable", motors_off_ok, nullptr); }
static void screws_calculate(lv_event_t*) { home_if_needed(); ks_gcode("SCREWS_TILT_CALCULATE"); }

// ---- live state: busy -> disable buttons, screws_tilt_adjust results -> labels ------------------------------------------
static void set_btn(BtnInfo& b, const char* icon, const char* text) {
    if (icon) lv_img_set_src(b.img, ks_icon(icon, strcmp(icon, "increase") ? 72 : 40));
    if (text) lv_label_set_text(b.lbl, text);
}
static void apply_results(JsonObjectConst root, void* user) {
    BL_St* s = (BL_St*)user;
    JsonObjectConst res = root["screws_tilt_adjust"]["results"].as<JsonObjectConst>();
    if (res.isNull()) return;
    for (JsonPairConst kv : res) {
        const Screw* sc = nullptr;
        for (int i = 0; i < s->n; i++) if (!strcmp(s->screws[i].name, kv.key().c_str())) sc = &s->screws[i];
        if (!sc) continue;
        const char* sign = kv.value()["sign"] | "CW";
        const char* adj = kv.value()["adjust"] | "00:00";
        bool base = kv.value()["is_base"] | false;
        int k;
        for (k = 0; k < P_N; k++) {                 // screw_dict: button whose target screw is this one
            if (!s->btn[k].shown) continue;
            int t = s->btn[k].target;
            if (s->has[t] && fabsf(s->found[t][0] - sc->x) < 0.05f && fabsf(s->found[t][1] - sc->y) < 0.05f) break;
        }
        if (k == P_N) continue;
        char txt[24], ic[8];
        int mm = 0, ss = 0; sscanf(adj, "%d:%d", &mm, &ss);
        if (base) snprintf(txt, sizeof txt, "Reference");
        else snprintf(txt, sizeof txt, "%s %s", sign, adj);
        if (!base && mm == 0 && ss < 6) snprintf(ic, sizeof ic, "complete");
        else snprintf(ic, sizeof ic, "%s", !strcasecmp(sign, "CCW") ? "ccw" : "cw");
        set_btn(s->btn[k], ic, txt);
    }
}
static void on_state(lv_event_t*) {
    BL_St* s = g_st;
    if (!s || !s->dm) return;
    char st[16] = "";
    ks_str("idle_timeout", "state", st, sizeof st);
    bool busy = !strcasecmp(st, "printing");
    for (int k = -1; k < P_N; k++) {
        lv_obj_t* b = k < 0 ? s->dm : s->btn[k].b;
        if (!b) continue;
        if (busy) lv_obj_add_state(b, LV_STATE_DISABLED); else lv_obj_clear_state(b, LV_STATE_DISABLED);
    }
    ks_with_status(apply_results, s);
}

// ---- build the panel once the printer config is known -------------------------------------------------------------------
static void build(JsonObjectConst cfg, bool ok, void*) {
    BL_St* s = g_st;
    if (!s) return;
    lv_obj_clean(s->root);
    s->dm = s->screws_btn = nullptr;
    const char* sect = nullptr;
    if (ok && !cfg["screws_tilt_adjust"].isNull()) sect = "screws_tilt_adjust";
    else if (ok && !cfg["bed_screws"].isNull()) sect = "bed_screws";
    s->z_tilt = ok && !cfg["z_tilt"].isNull();
    if (!sect) {
        lv_obj_t* l = ks_label(s->root, ok ? "No screws_tilt_adjust or bed_screws section in the printer config"
                                           : "Could not read the printer config", &ks_font_28);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP); lv_obj_set_width(l, 600); lv_obj_center(l);
        return;
    }
    bool tilt = !strcmp(sect, "screws_tilt_adjust");
    s->lift_speed = 5; s->hmz = 5; s->hspeed = 50; s->pzh = 0; s->xo = s->yo = 0; s->n = 0;
    if (tilt) {                                         // screws_tilt_adjust uses PROBE positions: add the probe offset
        s->xo = roundf(kc_float(cfg, "probe", "x_offset", 0) * 10) / 10;
        s->yo = roundf(kc_float(cfg, "probe", "y_offset", 0) * 10) / 10;
    }
    for (JsonPairConst kv : cfg[sect].as<JsonObjectConst>()) {
        const char* k = kv.key().c_str();
        const char* v = kv.value().as<const char*>();
        if (!v) continue;
        if (!strcmp(k, "probe_speed")) s->lift_speed = atof(v);
        else if (!strcmp(k, "speed")) s->hspeed = atof(v);
        else if (!strcmp(k, "horizontal_move_z")) s->hmz = atof(v);
        else if (!strcmp(k, "probe_height")) s->pzh = atof(v);
        else if (s->n < 16) {                           // re.match(r"([\-0-9\.]+)\s*,\s*([\-0-9\.]+)")
            float x, y;
            if (sscanf(v, "%f , %f", &x, &y) == 2 && (v[0] == '-' || v[0] == '.' || isdigit((unsigned char)v[0]))) {
                Screw& sc = s->screws[s->n++];
                strlcpy(sc.name, k, sizeof sc.name);
                sc.x = roundf((x) * 10) / 10 + s->xo; sc.y = roundf((y) * 10) / 10 + s->yo;
                sc.x = roundf(sc.x * 10) / 10; sc.y = roundf(sc.y * 10) / 10;
            }
        }
    }
    // sorted(key=(y, x))
    for (int i = 1; i < s->n; i++)
        for (int j = i; j > 0 && (s->screws[j].y < s->screws[j - 1].y ||
                                  (s->screws[j].y == s->screws[j - 1].y && s->screws[j].x < s->screws[j - 1].x)); j--)
            { Screw t = s->screws[j]; s->screws[j] = s->screws[j - 1]; s->screws[j - 1] = t; }

    // buttons that exist on the left even when the screw layout is unusable
    lv_obj_set_flex_flow(s->root, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(s->root, 4, 0);
    lv_obj_t* left = ks_card(s->root, 190, lv_pct(100));
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(left, 4, 0);
    s->dm = ks_button_sized(left, "motor-off", 56, "Disable Motors", 3, disable_motors, nullptr);
    lv_obj_set_size(s->dm, lv_pct(100), 204);
    if (tilt) {
        s->screws_btn = ks_button_sized(left, "refresh", 56, "Screws Adjust", 4, screws_calculate, nullptr);
        lv_obj_set_size(s->screws_btn, lv_pct(100), 204);
    }

    int nscrews = s->n;
    bool valid = nscrews > 0 && nscrews != 3 && nscrews != 5 && nscrews != 7;     // default screw_positions: auto-detection needs 4/6/8/9
    if (!valid) {
        char b[160];
        snprintf(b, sizeof b, "Bed screw configuration: %d\n\nNot supported for auto-detection, it needs to be configured in klipperscreen.conf", nscrews);
        lv_obj_t* l = ks_label(s->root, b, &ks_font_28);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP); lv_obj_set_width(l, 500);
        return;
    }

    float min_x = 1e9, max_x = -1e9, min_y = 1e9, max_y = -1e9;
    int xc = 0, yc = 0;
    for (int i = 0; i < s->n; i++) {
        bool nx = true, ny = true;
        for (int j = 0; j < i; j++) { if (s->screws[j].x == s->screws[i].x) nx = false; if (s->screws[j].y == s->screws[i].y) ny = false; }
        xc += nx; yc += ny;
        min_x = fminf(min_x, s->screws[i].x); max_x = fmaxf(max_x, s->screws[i].x);
        min_y = fminf(min_y, s->screws[i].y); max_y = fmaxf(max_y, s->screws[i].y);
    }
    float mid_x = roundf((min_x + max_x) / 2), mid_y = roundf((min_y + max_y) / 2);
    float maxd = ceilf(hypotf(max_x - min_x, max_y - min_y) / fminf(fminf(xc, yc), 3));

    // find_closest in the same order as the Python (fl, bl, br, fr, fm, bm, lm, rm, center), removing each match
    bool used[16] = {};
    struct { int key; float x, y; } order[] = {{P_FL, min_x, min_y}, {P_BL, min_x, max_y}, {P_BR, max_x, max_y}, {P_FR, max_x, min_y},
                                               {P_FM, mid_x, min_y}, {P_BM, mid_x, max_y}, {P_LM, min_x, mid_y}, {P_RM, max_x, mid_y},
                                               {P_CENTER, mid_x, mid_y}};
    for (auto& o : order) {
        int idx;
        s->has[o.key] = find_closest(s, used, o.x, o.y, maxd, true, &idx);
        if (s->has[o.key]) { s->found[o.key][0] = s->screws[idx].x; s->found[o.key][1] = s->screws[idx].y; }
    }

    int rot = ks_cfg_int("screw_rotation", 0);
    int ri = rot == 90 ? 1 : rot == 180 ? 2 : rot == 270 ? 3 : 0;
    lv_obj_t* grid = ks_card(s->root, 520, lv_pct(100));
    static lv_coord_t cw[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
    static lv_coord_t rh[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
    lv_obj_set_grid_dsc_array(grid, cw, rh);
    lv_obj_set_style_pad_gap(grid, 2, 0);
    for (int k = 0; k < P_N; k++) {
        s->btn[k] = {nullptr, nullptr, nullptr, false, k == P_CENTER ? P_CENTER : ROT[ri][k]};
        if (!s->has[k]) continue;                       // button only if that position was found
        bool c = k == P_CENTER;
        lv_obj_t* b = ks_button_sized(grid, ICONS[k], c ? 40 : 72, "", 0, go_to, (void*)(intptr_t)k);
        lv_obj_set_grid_cell(b, LV_GRID_ALIGN_STRETCH, CELL[k][0], 1, LV_GRID_ALIGN_STRETCH, CELL[k][1], 1);
        s->btn[k].b = b; s->btn[k].img = lv_obj_get_child(b, 0); s->btn[k].lbl = lv_obj_get_child(b, 1); s->btn[k].shown = true;
    }
    on_state(nullptr);
}

static void panel_del(lv_event_t*) {
    g_gen++;
    delete g_st;
    g_st = nullptr;
}

void ks_panel_bed_level(lv_obj_t* c) {
    g_gen++;
    delete g_st;
    g_st = new BL_St();
    memset(g_st, 0, sizeof *g_st);
    // content survives panel switches (only its children are cleaned): own the lifetime with a child holder
    lv_obj_t* root = ks_card(c, lv_pct(100), lv_pct(100));
    g_st->root = root;
    lv_obj_add_event_cb(root, panel_del, LV_EVENT_DELETE, nullptr);
    ks_on_state(root, on_state);
    lv_obj_t* l = ks_label(root, "Loading...", &ks_font_28);
    lv_obj_center(l);
    static const char* const secs[] = {"screws_tilt_adjust", "bed_screws", "probe", "z_tilt"};
    kc_fetch(&g_gen, secs, 4, nullptr, 0, build, nullptr);
}

