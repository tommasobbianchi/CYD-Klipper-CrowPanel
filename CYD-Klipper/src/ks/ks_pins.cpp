// panels/pins.py: one row per [output_pin NAME] (names starting with '_' are hidden): a switch for on/off pins,
// a 0-100 % slider with a "cancel" (set to 0) button for pwm / hardware_pwm pins -> SET_PIN PIN=NAME VALUE=v.
#include <Arduino.h>
#include "ks_widgets.h"
#include "ks_mr.h"
#include "ks_panels.h"
#include "ks_calib_common.h"

#define MAXPINS 16
struct Pin { char obj[40]; bool pwm; lv_obj_t* w; uint32_t hold; };     // w: lv_switch or lv_slider; hold: ignore status until this tick
struct PN { lv_obj_t* root; Pin pin[MAXPINS]; int n; };
static PN* g_st;
static uint32_t g_gen;

static bool parse_bool(const char* s) {
    if (!s) return false;
    char c = tolower((unsigned char)s[0]);
    return c == 't' || c == 'y' || c == '1' || c == 'o' && tolower((unsigned char)s[1]) == 'n';
}

static void send(int i, float v) {
    PN* s = g_st; if (!s) return;
    s->pin[i].hold = millis() + 1500;                               // the old value would flash back before Klipper applies the new one
    ks_gcode("SET_PIN PIN=%s VALUE=%g", s->pin[i].obj + 11, v);
}
static void on_switch(lv_event_t* e) {
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    send(i, lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED) ? 1 : 0);
}
static void on_slider(lv_event_t* e) {
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    send(i, lv_slider_get_value(lv_event_get_target(e)) / 100.0f);
}
static void on_min(lv_event_t* e) {
    PN* s = g_st; if (!s) return;
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    lv_slider_set_value(s->pin[i].w, 0, LV_ANIM_OFF);
    send(i, 0);
}

static void on_state(lv_event_t*) {
    PN* s = g_st; if (!s) return;
    for (int i = 0; i < s->n; i++) {
        Pin& p = s->pin[i];
        if (!p.w || (int32_t)(millis() - p.hold) < 0) continue;
        float v = ks_f(p.obj, "value", 0);
        if (p.pwm) { if (!lv_obj_has_state(p.w, LV_STATE_PRESSED)) lv_slider_set_value(p.w, (int)(v * 100 + 0.5f), LV_ANIM_OFF); }
        else if (v >= 1) lv_obj_add_state(p.w, LV_STATE_CHECKED);
        else lv_obj_clear_state(p.w, LV_STATE_CHECKED);
    }
}

static void build(JsonObjectConst cfg, bool ok, void*) {
    PN* s = g_st; if (!s) return;
    lv_obj_clean(s->root);
    char names[MAXPINS * 2][40];
    int cnt = ks_objects("output_pin ", names, MAXPINS * 2);
    // sorted by name, hidden '_' pins dropped
    int idx[MAXPINS * 2], m = 0;
    for (int i = 0; i < cnt; i++) if (names[i][11] != '_') idx[m++] = i;
    for (int i = 1; i < m; i++) for (int j = i; j > 0 && strcmp(names[idx[j]], names[idx[j - 1]]) < 0; j--) { int t = idx[j]; idx[j] = idx[j - 1]; idx[j - 1] = t; }
    if (m == 0) {
        lv_obj_center(ks_label(s->root, "No output pins", &ks_font_28));
        return;
    }
    lv_obj_t* l = ks_list(s->root);
    s->n = 0;
    for (int k = 0; k < m && s->n < MAXPINS; k++) {
        Pin& p = s->pin[s->n];
        strlcpy(p.obj, names[idx[k]], sizeof p.obj);
        char sect[40]; strlcpy(sect, p.obj, sizeof sect);
        for (char* c = sect; *c; c++) *c = tolower((unsigned char)*c);
        p.pwm = ok && (parse_bool(kc_val(cfg, sect, "pwm")) || parse_bool(kc_val(cfg, sect, "hardware_pwm")));
        p.hold = 0;
        float v = ks_f(p.obj, "value", 0);
        lv_obj_t* r = ks_row(l, p.obj + 11);
        if (p.pwm) {
            lv_obj_t* box = ks_row_right(r, 300);
            lv_obj_t* mn = ks_button(box, "cancel", nullptr, 1, on_min, (void*)(intptr_t)s->n);
            lv_obj_set_size(mn, 64, 60);
            p.w = ks_slider(box, 0, 100, (int)(v * 100 + 0.5f), 220, on_slider, (void*)(intptr_t)s->n);
        } else {
            p.w = ks_switch(ks_row_right(r, 120), v >= 1, on_switch, (void*)(intptr_t)s->n);
        }
        s->n++;
    }
    on_state(nullptr);
}

static void panel_del(lv_event_t*) { g_gen++; delete g_st; g_st = nullptr; }

void ks_panel_pins(lv_obj_t* c) {
    g_gen++;
    delete g_st;
    g_st = new PN();
    memset(g_st, 0, sizeof *g_st);
    g_st->root = ks_card(c, lv_pct(100), lv_pct(100));
    lv_obj_add_event_cb(g_st->root, panel_del, LV_EVENT_DELETE, nullptr);
    ks_on_state(g_st->root, on_state);
    lv_obj_center(ks_label(g_st->root, "Loading...", &ks_font_28));
    static const char* const keys[] = {"pwm", "hardware_pwm"};
    kc_fetch(&g_gen, nullptr, 0, keys, 2, build, nullptr);
}
